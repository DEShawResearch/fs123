#include "fs123/fs123server.hpp"
#include "fs123/evstream.hpp"
#include "fs123/httpheaders.hpp"
#include "fs123/content_codec.hpp"
#include "fs123/stat_serializev3.hpp"
#include <core123/autoclosers.hpp>
#include <core123/exnest.hpp>
#include <core123/syslog_number.hpp>
#include <core123/sew.hpp>
#include <core123/http_error_category.hpp>
#include <core123/log_channel.hpp>
#include <core123/diag.hpp>
#include <core123/svto.hpp>
#include <core123/unused.hpp>
#include <core123/threeroe.hpp>
#include <core123/base64.hpp>
#include <core123/netstring.hpp>
#include <core123/producerconsumerqueue.hpp>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <event2/thread.h>
#include <tuple>
#include <fstream>
#include <thread>
#include <netinet/tcp.h>

using namespace core123;


#if LIBEVENT_VERSION_NUMBER >= 0x02010100  // 2.1.1
#define HAVE_BUFFEREVENT_SET_MAX_SINGLE_WRITE
#endif
#if LIBEVENT_VERSION_NUMBER >= 0x02010400  // 2.1.4
#define HAVE_SET_DEFAULT_CONTENT_TYPE
#endif
#if LIBEVENT_VERSION_NUMBER >= 0x02010800  // 2.0.8
#define HAVE_EVUTIL_DATE_RFC1123
#endif
#if LIBEVENT_VERSION_NUMBER >= 0x02010100  // 2.1.1
#define HAVE_EVHTTP_SET_BEVCB
#endif

auto _proc = diag_name("proc");
auto _fs123server = diag_name("fs123server");
auto _secretbox = diag_name("secretbox");
auto _errs = diag_name("errs");

server_stats_t server_stats;

namespace {

// Under high concurrency tests with ab, libevent spends too much time
// reporting "too many open files" errors, so we suppress consecutive
// duplicates to give it a chance to do some real work!
void dup_suppress_log(int severity, const char *msg) {
    static std::string prevmsg;
    static unsigned long prevcount{0};
    if (msg == prevmsg) {
	if (prevcount == 0) {
	    prevcount++;
	    complain("duplicate message, suppressing consecutive repeats after this...");
	}
	return;
    } else {
	prevmsg = msg;
	prevcount = 0;
    }
    int level;
    switch(severity){
    case EVENT_LOG_DEBUG:
        level = LOG_DEBUG; break;
    case EVENT_LOG_MSG:
        level = LOG_INFO; break;
    case EVENT_LOG_WARN:
        level = LOG_WARNING; break;
    case EVENT_LOG_ERR:
        level = LOG_ERR; break;
    default:
        level = LOG_ERR; break;
    }
    complain(level, "%s", msg);
}

fs123p7::method_e
evhttp_request_get_method(const evhttp_request *evreq){
    auto cmd = evhttp_request_get_command(evreq);
    switch(cmd){
#define CASE(SYM) case EVHTTP_REQ_##SYM : return fs123p7::SYM;
        CASE(GET);
        CASE(POST);
        CASE(HEAD);
        CASE(PUT);
        CASE(DELETE);
        CASE(OPTIONS);
        CASE(TRACE);
        CASE(CONNECT);
        CASE(PATCH);
#if LIBEVENT_VERSION_NUMBER >= 0x02020001
        CASE(PROPFIND);
        CASE(PROPPATCH);
        CASE(MKCOL);
        CASE(LOCK);
        CASE(UNLOCK);
        CASE(COPY);
        CASE(MOVE);
#endif
    }
    throw std::runtime_error(str("Unrecognized value of evhttp_command:", cmd));
}

int http_status_from_evnest(const std::exception& e){
    // Work from the bottom up...
    // If we see a system_error in the http category, then take the status from the http error.
    // If we see a system_error in another category (e.g., system), then it's a 500.
    // If we get all the way to the top, the status is 500.
    for(auto& er : rexnest(e)) {
        const std::system_error *sep = dynamic_cast<const std::system_error*>(&er);
        if(sep){
            return (sep->code().category() == http_error_category()) ?
                sep->code().value() :
                500;
        }
    }
    return 500;
}    

void add_hdr(evkeyvalq* hdrs, const char* n, const char* v) {
    // N.B.  evhttp_add_header strdup's both n and v, so
    // we don't have to worry about lifetimes.
    if( evhttp_add_header(hdrs, n, v) != 0 )
        throw se(EINVAL, fmt("evhttp_add_header(%s, %s) returned non-zero", n, v));
}

void add_hdr(evkeyvalq* hdrs, const char* n, const std::string& v){
    add_hdr(hdrs, n, v.c_str());
}

void add_hdr(evkeyvalq* hdrs, const std::string& n, const std::string& v){
    add_hdr(hdrs, n.c_str(), v.c_str());
}

void add_hdr(evkeyvalq* hdrs, const char* n, str_view v){
    add_hdr(hdrs, n, std::string(v));
}

std::string
etag_mangle(uint64_t etag_inner, const std::string& esid){
    uint64_t h = esid.empty() ? 0 : threeroe(esid).hash64();
    return '"' + std::to_string(etag_inner ^ h) + '"';
}

uint64_t
inm_demangle(str_view inm_sv, const std::string& esid){
    if(inm_sv.empty())
        return 0;
    // the If-None-Match header should be a double-quoted 64-bit integer,
    // exactly as provided by an earlier ETag header.
    try{
        uint64_t h = esid.empty() ? 0 : threeroe(esid).hash64();
        return h ^ parse_quoted_etag(inm_sv);
    }catch(std::exception& e){
        complain(LOG_WARNING, "If-None-Match not parseable as a uint64.  Someone is sending us bogus If-None-Match headers.");
        return 0;
    }
}

void sig_cb_adapter(int, short, void* _data){
    auto& data = *(fs123p7::sig_cb_adapter_data*)_data;
    std::get<std::function<void(int,void*)>>(data)(std::get<int>(data), std::get<void*>(data));
}

// validated_path - if the path_info argument looks sketchy in any way, throw
// an http category error with status 400.  If all is well, return.
void validate_path(str_view pi){
    if(pi.empty())
        return;
    if(pi.front() != '/')
        httpthrow(400, "path must start with a /");
    if(pi.back() == '/')
        httpthrow(400, "path may not end with a /");
    if(pi.find("//") != std::string::npos)
        httpthrow(400, "path may not contan //");

    if(pi.find('\0') != std::string::npos)
        httpthrow(400, "path may not contain NUL");

    if(pi.find("/../") != std::string::npos)
        httpthrow(400, "path may not contain /../");
    if(endswith(pi, "/.."))
        httpthrow(400, "path may not end with /..");
}

#if !defined(HAVE_EVUTIL_DATE_RFC1123)
// The libevent we're linking with is too old.  This is
// evutil_date_rfc1123 from a recent (2019) upstream commit
int
evutil_date_rfc1123(char *date, const size_t datelen, const struct tm *tm)
{
	static const char *DAYS[] =
		{ "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static const char *MONTHS[] =
		{ "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

	time_t t = time(NULL);

#if defined(EVENT__HAVE__GMTIME64_S) || !defined(_WIN32)
	struct tm sys;
#endif

	/* If `tm` is null, set system's current time. */
	if (tm == NULL) {
#if !defined(_WIN32)
		gmtime_r(&t, &sys);
		tm = &sys;
		/** detect _gmtime64()/_gmtime64_s() */
#elif defined(EVENT__HAVE__GMTIME64_S)
		errno_t err;
		err = _gmtime64_s(&sys, &t);
		if (err) {
			event_errx(1, "Invalid argument to _gmtime64_s");
		} else {
			tm = &sys;
		}
#elif defined(EVENT__HAVE__GMTIME64)
		tm = _gmtime64(&t);
#else
		tm = gmtime(&t);
#endif
	}

	return evutil_snprintf(
		date, datelen, "%s, %02d %s %4d %02d:%02d:%02d GMT",
		DAYS[tm->tm_wday], tm->tm_mday, MONTHS[tm->tm_mon],
		1900 + tm->tm_year, tm->tm_hour, tm->tm_min, tm->tm_sec);
}
#endif

#if !defined(HAVE_EVHTTP_SET_BEVCB)
void evhttp_set_bevcb(struct evhttp*, struct bufferevent *(*)(struct event_base *, void*), void*){
    throw std::runtime_error("How did we get here?  Is the code compiled with libevent-2.0.x, but (dynamically?) linked with 2.1.x?  And it's got an asynchronous handler?  No.  Just no.");
}
#endif

} // namespace <anon>

namespace fs123p7{
// async_reply_mechanism - see comment in fs123server.hpp.
//  Use a pipe to notify the event loop that it's time
//  to call evhttp_send_reply.  
//
//  N.B.  This is not the same as
//     https://github.com/libevent/libevent/issues/695 (see below)
//  It's more of a paranoid what-if-libevent-isn't-really-thread-safe
//  workaround.
struct async_reply {
    evhttp_request* evreq;
    int status;
};
static_assert(sizeof(async_reply) <= PIPE_BUF, "Huh?  How small is PIPE_BUF?");

struct async_reply_mechanism{
    async_reply_mechanism(){
        sew::pipe(acpipefd);
        for (int i = 0; i < 2; i++) {
            auto flags = sew::fcntl(acpipefd[i], F_GETFL);
            flags |= O_CLOEXEC|O_NONBLOCK;
            sew::fcntl(acpipefd[i], F_SETFL, flags);
        }
    }
    void send_reply(evhttp_request* evreq, int status){
        async_reply reply;
        ::memset(&reply, 0, sizeof(reply));
        reply.evreq = evreq;
        reply.status = status;
        // if sew::write throws an EAGAIN, we treat it like any other
        // error.  I.e., the caller catches it and logs it.  We don't
        // retry.  The reply is never sent.  The client (eventually)
        // times out.  Blocking risks deadlock, which is even worse
        // than ghosting one client.
        auto bytes = sew::write(acpipefd[1], &reply, sizeof(reply));
        if (bytes != sizeof(reply)){
            // This should never happen because:
            //      Reading or writing pipe data is atomic if the size of
            //      data written is not greater than PIPE_BUF.
            // But if it does happen, we're in deep trouble.  It means
            // reads on the other end of the pipe will be misaligned,
            // which can't possibly end well.  Better to abort() now
            // than segfault later...
            complain(LOG_CRIT, "async_reply_mechanism::send_reply:  short write.  Calling std::terminate().");
            std::terminate();
        }
    }
        
    int get_reader_fd() const { return acpipefd[0]; }

    decltype(make_autocloser((event*)nullptr, ::event_free)) ev{nullptr, ::event_free};
private:
    // a pair of autoclosing pipe(2) file descriptors used for inter-thread communication
    acfd acpipefd[2];
};
    
std::unique_ptr<async_reply_mechanism> /*private*/
server::setup_async_mechanism(struct event_base *eb) {
    auto arm = std::make_unique<async_reply_mechanism>();
    auto asynccb = [] (evutil_socket_t fd, short what, void *) -> void {
                       DIAGf(_fs123server, "asynccb");
                       if (!(what & EV_READ)){
                           complain(LOG_WARNING, "asynccb called with EV_READ unset.  How can this happen?");
                           return;
                       }
                       async_reply reply;
                       // loop?  Or just let libevent wake us up again?
                       auto nread = read(fd, &reply, sizeof(reply));
                       if(nread == sizeof(reply)){
                           DIAGf(_fs123server, "asynccb: evhttp_send_reply(%p, %d)", reply.evreq, reply.status);
                           evhttp_send_reply(reply.evreq, reply.status, nullptr, nullptr);
                       }else if(nread < 0 && errno == EAGAIN){
                           complain(LOG_WARNING, "asynccb:  EAGAIN on async_reply_mechanism pipe.  How can this happen?");
                       }else if(nread == 0){
                           complain(LOG_WARNING, "asynccb:  EOF on async_reply_mechanism pipe.  How can this happen?");
                           // Carry on and hope for the best?  Maybe we'll shut down cleanly before disaster strikes?
                       }else{
                           // This is *extremely* bad.  We've done a short read,
                           // which presumably means subsequent reads (if they
                           // work at all) will be mis-aligned.  The best thing
                           // to do here is to abort.  Maybe a watchdog will restart
                           // us, and maybe a core file will help us to avoid this
                           // in the future.
                           complain(LOG_CRIT, "asynccb:  short read on async_reply_mechanism pipe.  This can't happen!  std::terminate()!");
                           std::terminate();
                       }
                   };
    arm->ev = make_autocloser(event_new(eb, arm->get_reader_fd(), EV_READ|EV_PERSIST, asynccb, nullptr), ::event_free);
    if (!arm->ev)
        throw se(errno, "event_new on async_mechanism failed");
    if (event_add(arm->ev, nullptr) < 0)
        throw se(errno, "event_add on async_mechanism failed failed");
    return arm;
}

void /*private*/
req::maybe_call_logger(int status) {
    if(!evhr)
        return complain(LOG_ERR, "req::maybe_call_logger called with evhr==nullptr.  This *SHOULD NOT HAPPEN*.  Start debugging!");

    // N.B.  there's not much to be gained by short-circuiting this.
    // libevent called getpeername when the connection was established,
    // so there's not much point in eliding the call to evhttp_connection_get_peer.
    // Similarly for get_method and get_uri.  
    char *remote;
    uint16_t port;
    auto evcon = evhttp_request_get_connection(evhr);
    evhttp_connection_get_peer(evcon, &remote, &port);
    auto evmethod = evhttp_request_get_method(evhr);
    const char *evuri = evhttp_request_get_uri(evhr);
    auto length = evbuffer_get_length(evhttp_request_get_output_buffer(evhr));
    // I hate the idea of spending more time formatting the date than
    // composing and sending the data.  So what to do?
    //
    // libevent is obliged to construct a Date header.  But it hasn't
    // done so yet.  (And we can't wait till after it does so in
    // evhttp_send_reply).  If we create a Date header using the same
    // code that libevent would have used, the net-cost is zero.
    auto headers = evhttp_request_get_output_headers(evhr);
    char date[50];
    if (sizeof(date) - evutil_date_rfc1123(date, sizeof(date), NULL) > 0) {
        evhttp_add_header(headers, "Date", date);
    }else{
        complain(LOG_ERR, "evutil_date_rfc1123 didn't fit in 50 chars?");
        strcpy(date, "-");
    }
#if !defined(HAVE_SET_DEFAULT_CONTENT_TYPE)
    evhttp_add_header(headers, "Content-Type", "text/plain");
#endif

    server_stats.reply_bytes += length;
    switch(status){
    case 200: server_stats.reply_200s++; break;
    case 304: server_stats.reply_304s++; break;
    default:  server_stats.reply_others++; break;
    }
    try{
        svr.handler.logger(remote, evmethod, evuri, status, length, date);
    }catch(std::exception& e){
        complain(e, "exception thrown by logger handler");
    }
 }

void /*private*/
server::incast_collapse_workaround(evhttp_request *evreq){
    auto evcon = evhttp_request_get_connection(evreq);
    if(evcon == nullptr)
        throw se(EINVAL, "No connection?");
    auto ehbe = evhttp_connection_get_bufferevent(evcon);

#if defined(HAVE_BUFFEREVENT_SET_MAX_SINGLE_WRITE)
    bufferevent_set_max_single_write(ehbe, gopts->max_single_write);
#endif    

    if (gopts->tcp_nodelay) {
        int nodelay = 1;
        auto ehbesock = bufferevent_getfd(ehbe);
        sew::setsockopt(ehbesock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }
}

void
server::set_signal_handlers() {
    auto sigcb = [] (evutil_socket_t, short /*what*/, void *arg) -> void {
                     auto b = static_cast<struct event_base *>(arg);
                     event_base_loopbreak(b);
                     complain(LOG_NOTICE, "Caught one of SIGINT, SIGTERM, SIGHUP or SIGQUIT.  called event_base_loopbreak(%p)", arg);
                 };
    for (auto sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) {
	auto flags = EV_SIGNAL;
	auto e = event_new(ebac, sig, flags, sigcb, ebac.get());
        events2befreed.push_back(e);
	if (e == nullptr)
	    throw se(errno, "event_new on signal failed");
	if (event_add(e, nullptr) < 0)
	    throw se(errno, "event_add on signal failed");
    }
}

// maybe_encode_content only knows about secretbox.  It returns the
// 'secretid' used to encode the content, or an empty string if no
// encoding was done.  We may, eventually, have more than
// one possible encoding, at which point, the API will have to change.
// Note that ETag can't be added to the headers until maybe_encode_content
// is called because ETag needs to fold in the secretid (not(!) the secret).
std::string /* private */
req::maybe_encode_content(){
    if(!svr.the_secret_manager)
        return {};
    auto esid = svr.the_secret_manager->get_encode_sid();
    if(accept_encoding != content_codec::CE_FS123_SECRETBOX){
        httpthrow(406, "Request must specify Accept-encoding: fs123-secretbox");
    }
    // OK - let's do this...  We're encoding with secretbox!
    if(!blob) 
        allocate_pbuf(0);
    auto esecret = svr.the_secret_manager->get_sharedkey(esid);
    buf = content_codec::encode(accept_encoding, esid, esecret, buf, secretbox_padding);
    DIAGf(_secretbox, "encoded has length %zd, trsum %s\n", buf.size(), threeroe(buf).hexdigest().c_str());
    return esid;
}

void /* static private */
req::parse_and_handle(req::up req) try {
    server_stats.requests++;
    // If we're going to switch on method and/or /sel/ec/tor,
    // we have to do that here.  E.g., the mafs server
    // forwards all POST requests to a dropbucket handler.

    // N.B.  Use req->member to emphasize/remind ourselves what named
    // members are recorded in *this, for future use by _reply()
    // functions.
    switch (req->method) {
    case fs123p7::GET:
    case fs123p7::HEAD:
        break;
    default:
        httpthrow(403, std::string("Unsupported request method ") + std::to_string(evhttp_request_get_command(req->evhr))); break;
    }

    const evhttp_uri* uri = evhttp_request_get_evhttp_uri(req->evhr);
    auto inheaders = evhttp_request_get_input_headers(req->evhr);
    if(!inheaders)
        httpthrow(500, "evhttp_request_get_input_headers returned NULL");

    // If we're configured with secrets and the client *doesn't*
    // accept secretbox, we give up before wasting any more time.
    const char *ae = evhttp_find_header(inheaders, "Accept-Encoding");
    req->accept_encoding = ae ? content_codec::encoding_stoi(ae) : int16_t(content_codec::CE_IDENT);

    struct server& svr = req->svr;
    if(svr.the_secret_manager &&
       req->accept_encoding != content_codec::CE_FS123_SECRETBOX)
        httpthrow(406, "Request must specify Accept-encoding: fs123-secretbox");

    // The uri (and the path and query) have the same lifetime as
    // evhr.  Getting the path and query are pretty much free.
    // get_evhttp_uri paid the price to pick them apart and strdup
    // them when it constructed the uri.
    const char *uri_path = evhttp_uri_get_path(uri);
    if(uri_path == nullptr)
        throw se(EINVAL, "evhttp_uri_get_path returned NULL");
    size_t decoded_path_sz;
    req->decoded_path_up = std::unique_ptr<char, decltype(std::free)*>{evhttp_uridecode(uri_path, 0, &decoded_path_sz), std::free};
    if (!req->decoded_path_up)
        throw se(EINVAL, "failed to uridecode path");
    str_view upath_sv = req->decoded_path_up.get();
    DIAG(_fs123server, "upath_sv = " << upath_sv);

    // The generic 'path' part of the URI should look like:
    //   /SEL/ECT/OR/SIGIL/PROTOMajor/PROTOMinor/FUNCTION/relat/ive/to/expo/rt_ro/ot?QUERY
    // SIGIL = "fs123"
    // PROTOMajor is 7
    // PROTOMinor is an integer
    // FUNCTION is a single-letter.
    // /relat/ive/to/expo/rt_ro/ot is the 'path_info'
    // N.B.  This terminology is confusing.  Conventional usage would
    // be to call the whole thing the path.  We call it 'upath_sv'.

    // N.B.  We save ourselves the trouble of parsing the PROTOmajor
    // because (for this code) it's always 7.  When we know what
    // PROTOmajor=8 looks like, we'll know whether it's similar enough
    // to just add some logic here, or whether it needs an entirely
    // new framework.
    static char const SIGIL[] = "/fs123/7/";
    size_t start_of_sigil = upath_sv.find(SIGIL);
    if(start_of_sigil == str_view::npos)
        httpthrow(400, "SIGIL not found in decoded path");

    str_view selector = upath_sv.substr(0, start_of_sigil);
    unused(selector);
    size_t nextoff = start_of_sigil + sizeof(SIGIL)-1;
    // PROTOminor/
    nextoff = svscan(upath_sv, &req->proto_minor, nextoff) + 1;
    // nextoff is one past the slash that follows PROTOmajor/PROTOminor
    if(upath_sv.size() <= nextoff || upath_sv[nextoff-1] != '/')
        httpthrow(400, "expected /FUNCTION after PROTOmajor/PROTOminor");
    
    // FUNCTION[/PA/TH]
    size_t pidx = upath_sv.find('/', nextoff);
    if(pidx != str_view::npos){
        // there is a slash after /FUNCTION.  The slash
        // is the first character of path.
        req->path_info = upath_sv.substr(pidx);
        req->function = upath_sv.substr(nextoff, pidx-nextoff);
    }else{
        // there's no next slash.  The function_ extends to
        // the end of the decoded_path.  The path itself is empty.
        req->path_info = "";
        req->function = upath_sv.substr(nextoff);
    }

    if(req->function == "e"){
        // decrypt the path to obtain a new function and a new /function/path?query
        if(req->path_info.empty() || req->path_info[0] != '/')
            httpthrow(400, "path_info must be of the form /<base64(path_info)>");
        if(!svr.the_secret_manager)
            httpthrow(400, "decode_envelope:  no secret manager.  Can't decode");
        req->decode64 = macaron::Base64::Decode(std::string(req->path_info.substr(1)));
        // decode in-place, 
        auto decrypted_sv = as_str_view(content_codec::decode(content_codec::CE_FS123_SECRETBOX, as_uchar_span(req->decode64), *svr.the_secret_manager));
        // The decrypted string should look like '/FUNCTION/PA/TH?QUERY
        if(decrypted_sv.empty() || decrypted_sv[0] != '/')
            httpthrow(400, "decode_envelope: plaintext must start with /");
        auto queryidx = decrypted_sv.find_last_of('?');
        req->query = (queryidx==std::string::npos) ? str_view{nullptr, 0} : decrypted_sv.substr(queryidx+1);
        nextoff = 1;
        pidx = decrypted_sv.find('/', nextoff);
        if(pidx != str_view::npos){
            req->function = decrypted_sv.substr(nextoff, pidx-nextoff);
            req->path_info = decrypted_sv.substr(pidx, queryidx-pidx);
        }else{
            req->path_info = "";
            req->function = decrypted_sv.substr(nextoff, queryidx-nextoff);
        }
        DIAG(_fs123server, "/e request converted to:  query: " << req->query << ", function: " << req->function << ", path_info: " << req->path_info);
    }else{
        // not /e-ncrypted.  Are we willing to look at it?
        if(svr.the_secret_manager && !svr.gopts->allow_unencrypted_requests)
            httpthrow(406, "Requests must be encrypted and authenticated");
        // [?QUERY]
        const char *q = evhttp_uri_get_query(uri);
        req->query = q ? q : str_view{nullptr, 0};
    }
    // We've finally got path_info full decoded and decrypted.  Does it look ok?
    validate_path(req->path_info);  // throws if it's not ok.

    std::string esid = svr.the_secret_manager ? svr.the_secret_manager->get_encode_sid() : "";
    const char* inm_char = evhttp_find_header(inheaders, "If-None-Match"); // yes - it uses strcasecmp.
    if(inm_char)
        req->inm = inm_char;
    auto comma = req->inm.find(',');
    if( comma != str_view::npos ){
        complain(LOG_WARNING, "If-None-Match contains a comma.  check_inm only checks the first tag in the header");
        req->inm.remove_suffix(req->inm.size() - comma);
    }
        
    uint64_t inm64 = inm_demangle(req->inm, esid);
    if(inm64)
        server_stats.INM_requests++;
    DIAGf(_fs123server, "If-None-Match: %s inm64: %016" PRIx64, std::string(req->inm).c_str(), inm64);

    handler_base& handler = svr.handler;
    if(req->function == "a"){
        server_stats.a_requests++;
        handler.a(std::move(req));
    }else if(req->function == "d"){
        server_stats.d_requests++;
        // The query is Len;Begin;Offset
        // Len is an unsigned integer nuumber of kilobytes
        // Offset is a signed integer
        // Begin is 0 or 1
        uint64_t lenkib;
        int64_t offset;
        int begin;
        svscan(req->query, std::tie(lenkib, begin, offset), 0); // N.B.  permissive about separator.  Does not insist on semi-colon
        req->allocate_pbuf(lenkib*1024);
        handler.d(std::move(req), inm64, !!begin, offset);
    }else if(req->function == "f"){
        server_stats.f_requests++;
        // The query is Len;Offset
        // Len and offset are unsigned kibibytes.
        uint64_t lenkib, offsetkib;
        svscan(req->query, std::tie(lenkib, offsetkib)); // N.B.  permissive about separator.  Does not insist on semi-colon
        static size_t validator_space = 32; // room for a netstring(to_string(uint64_t));y
        req->requested_len = lenkib*1024;
        req->allocate_pbuf(req->requested_len + validator_space);
        req->buf = req->buf.subspan(validator_space, 0); // buf points to beginning of requested_len
        handler.f(std::move(req), inm64, req->requested_len, offsetkib*1024, req->buf.data());
    }else if(req->function == "l"){
        server_stats.l_requests++;
        handler.l(std::move(req));
    }else if(req->function == "s"){
        server_stats.s_requests++;
        handler.s(std::move(req));
    }else if(req->function == "x"){
        server_stats.x_requests++;
        // The query is Len;Name;
        uint64_t lenkib;
        auto first_semiidx = svscan(req->query, &lenkib);
        // The name extends to the next semicolon.  The name may not contain semicolon.
        if( first_semiidx >= req->query.size() || req->query[first_semiidx] != ';' )
            httpthrow(400, "no semicolon after Len");
        auto second_semiidx = req->query.find_first_of(";", first_semiidx+1);
        if( second_semiidx >= req->query.size() || req->query[second_semiidx] != ';' )
            httpthrow(400, "no semicolon after name");
        auto encnamelen = second_semiidx - (1 + first_semiidx);
        std::string encname(req->query.substr(first_semiidx+1, encnamelen));
        size_t namelen;
        // we "promise" that the 
        const char* malloced_name = evhttp_uridecode(encname.c_str(), 0, &namelen);
        if (!malloced_name)
            httpthrow(400, "failed to uridecode xattr name");
        std::string xname = malloced_name;
        ::free((void*)malloced_name);
        handler.x(std::move(req), lenkib*1024, std::move(xname));
    }else if(req->function == "n"){
        server_stats.n_requests++;
        handler.n(std::move(req));
    }else if(req->function == "p"){
        server_stats.p_requests++;
        evistream bufis(evhttp_request_get_input_buffer(req->evhr));
        handler.p(std::move(req), inm64, bufis);
    }else{
        httpthrow(400, fmt("Unknown fs123 /function: %s uri_path: %s", std::string(req->function).c_str(), uri_path));
    }
 }catch(std::exception& e){
    if(req)
        req->exception_reply(e);
    else
        complain(e, "exception thrown by handler, assuming the handler called a _reply function (perhaps in the req's destructor)");
 }

void /* private */
req::log_and_send_destructively(int status) try {
    if(replied)
        throw std::runtime_error("req::log_and_send_destructively has already been called");
    // Only try once.  If something in here throws, replied is still set,
    // so there won't be any complaints from ~req.
    replied = true;
    // evhttp_send_reply with a null databuf is undocumented, but
    // looking at the code, it "clearly" sends the data already
    // associated with evhttp_request_get_output_buffer(evreq).
    // It modifies the output_buffer, though, so get the length
    // before calling evhttp_send_reply.
    DIAGf(_fs123server, "log_and_send_destructively(%d)\n", status);
    maybe_call_logger(status);
    if(arm && !synchronous_reply){
        DIAGf(_fs123server, "svr.arm.send_reply(%p, %d)", evhr, status);
        arm->send_reply(evhr, status);
    }else{
        evhttp_send_reply(evhr, status, nullptr, nullptr);
    }
    // The evhttp_request (evhr) is no longer usable!
    // evhttp_send_reply starts a chain of callbacks in the libevent
    // loop that results in a call to evhttp_request_free(evhr).  It
    // might take a while, and it might happen in another thread, so
    // we might "get away with it" for a while before it bites us, but
    // any access through evhr is an invitation to a SEGV.  After the
    // evhttp_request_free, all the str_views in the req are trashed.
    // E.g., the 'path_info', 'uri', 'function' and 'inm' str_views
    // point into data that was "owned" by evhr.
    evhr = nullptr;
 }catch(std::exception& e){
    complain(LOG_CRIT, e, "Exception thrown by final log_and_send_destructively.  Reply not sent.  Client will eventually time out.  Leakage likely");
 }

void /* private */
req::common_reply200(const std::string& cc, uint64_t etag64/*=0*/, const char* fs123_errno/*="0"*/){
    evhttp_request* evreq = evhr;
    svr.incast_collapse_workaround(evreq);
    auto ohdrs = evhttp_request_get_output_headers(evreq);
    add_hdr(ohdrs, "Cache-control", cc);
    add_hdr(ohdrs, "Content-type", "application/octet-stream");
    // Encrypt.
    std::string esid = maybe_encode_content();
    if(!esid.empty())
        add_hdr(ohdrs, "Content-encoding", "fs123-secretbox");
    if(etag64){
        DIAGf(_fs123server, "etag64: %016" PRIx64 ", mangled: %s", etag64, etag_mangle(etag64, esid).c_str());
        add_hdr(ohdrs, "ETag", etag_mangle(etag64, esid));
    }

    auto ob = evhttp_request_get_output_buffer(evreq);
    size_t content_len = 0;
    core123::threeroe tr;
    if(method == fs123p7::GET){
        unsigned char* blobptr = blob.release();
        DIAG(_fs123server, "evbuffer_add_reference(buf.size()=" << buf.size() << ")");
        if(0 > evbuffer_add_reference(ob,
                                      buf.data(), buf.size(),
                                      [](const void *, size_t, void *extra){
                                          delete[] (unsigned char*)extra;
                                      }, blobptr)){
            httpthrow(500, "evbuffer_add_reference failed");
        }
        if(esid.empty())
            tr.update(buf);
    }
    if(fs123_errno)
        add_hdr(ohdrs, HHERRNO, fs123_errno);
    if(method == fs123p7::GET && esid.empty() && content_len > 0)
        add_hdr(ohdrs, HHTRSUM, tr.hexdigest());

    log_and_send_destructively(200);
 }

// exception_reply - complain to logs, send an http 5xx (or other
// error status), and return the error status to the caller.  It must
// not throw because it's called from (among other places) the
// exception handler in the http_cb callback.  There's nobody higher
// up the stack to catch anything it might throw.
void
req::exception_reply(const std::exception& e) {
    complain(e, "fs123server library caught exception:");
    // clear any headers or content that had already been
    // associated with evhr before the throw:
    evhttp_clear_headers(evhttp_request_get_output_headers(evhr));
    auto evb = evhttp_request_get_output_buffer(evhr);
    evbuffer_drain(evb, evbuffer_get_length(evb));
    // Add some content from the thrown exception
    for(auto& ep : exnest(e)){
        evbuffer_add_printf(evb, "%s\n", ep.what());
    }
    // If any of the nested exceptions was a system error in the
    // http category we use the status of the deepest such
    // exception.
    auto status = http_status_from_evnest(e);
    log_and_send_destructively(status);
 }

// http_cb is the callback that's invoked directly by libevent.
void /* static private */
req::http_cb(evhttp_request* evreq, void *varg) try {
    auto& arg = *(http_cb_arg*)varg;
    auto svr = std::get<server*>(arg);
    svr->time_of_last_request = std::chrono::system_clock::now();
    // Can't call make_unique because the constuctor is private.  We're a friend.
    DIAGf(_fs123server, "req::make_up(%p, %p, %p) evcon=%p", evreq, svr, std::get<async_reply_mechanism*>(arg), evhttp_request_get_connection(evreq));
    auto req = fs123p7::req::make_up(evreq, svr, std::get<async_reply_mechanism*>(arg));
    parse_and_handle(std::move(req));
 }catch(std::exception& e){
    complain(e, "exception thrown in http_cb");
 }

std::unique_ptr<async_reply_mechanism>
server::setup_async(struct event_base* eb, struct evhttp* eh){
    if(!strictly_synchronous_handlers){
        // It's *possible* that we don't need threadsafe bufferevents if we use the
        // 'async_reply_mechanism'.  But why take chances??
        evhttp_set_bevcb(eh, [](struct event_base* evb, void*){ return bufferevent_socket_new(evb, -1, BEV_OPT_THREADSAFE);}, nullptr);
        if(gopts->async_reply_mechanism)
            return setup_async_mechanism(eb);
    }
    return {};
}    

struct sockaddr_in
server::get_sockaddr_in() const{
    if (ehsock == nullptr)
        httpthrow(500, "get_sockaddr_in with null ehsock?");
    auto sockfd = evhttp_bound_socket_get_fd(ehsock);
    evutil_make_listen_socket_reuseable(sockfd);
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    sew::getsockname(sockfd, (struct sockaddr*)&sa, &salen);
    return sa;
}

std::string
server::get_baseurl() const{
    sockaddr_in sain = get_sockaddr_in();
    char sockname[INET_ADDRSTRLEN];
    if(!inet_ntop(AF_INET, &sain.sin_addr, sockname, sizeof(sockname)))
        throw se("inet_ntop failed");
    std::string url = "http://";
    if(strcmp(sockname, "0.0.0.0") == 0){
        // I'm sure we're going to to through several iterations of this!
#ifndef HOST_NAME_MAX // MacOS
#define HOST_NAME_MAX 255
#endif	
        char hname[HOST_NAME_MAX];
        sew::gethostname(hname, sizeof(hname));
        url += hname;
    }else{
        url += sockname;
    }
    url += ":" + str(htons(sain.sin_port));
    return url;
}

void
server::evhttp_bind_socket(struct event_base* eb, struct evhttp* eh) /*private */{
    // additional thread, existing socket.
    //
    // We can't call evhttp_accept_socket_with_handle because it
    // creates a listener with the LEV_OPT_CLOSE_ON_FREE flag set,
    // and that introduces a race condition when the thread shuts
    // down and tries to close the common socket.  So we do
    // exactly what evhttp_accept_socket_with_handle would have
    // done - except that we don't set LEV_OPT_CLOSE_ON_FREE in
    // flags.
    const int flags = LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_EXEC; // |LEV_OPT_CLOSE_ON_FREE;
    auto listener = evconnlistener_new(eb, NULL, NULL, flags, 0, evhttp_bound_socket_get_fd(ehsock));
    if(!listener)
        throw se("thread failed in evconnlistener_new");
    auto bound = evhttp_bind_listener(eh, listener);
    if(!bound){
        evconnlistener_free(listener);
        throw se("thread failed in evhttp_bind_listener");
    }
}

void
server::setup_evhttp(struct evhttp *eh, async_reply_mechanism* arm) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lg(mtx);
    cbargs.push_back(std::make_unique<http_cb_arg>(this, arm));
    evhttp_set_gencb(eh, req::http_cb, cbargs.back().get());
    // N.B.  libevent defaults to
    //    Content-Type: text/html; charset=ISO-8859-1 )
    // which is definitely not right.  application/octet-stream is
    // arguably better, but if you happen to enter an fs123 URL in a
    // browser, it tries to save to a file while text/plain
    // is pretty readable.
#if defined(HAVE_SET_DEFAULT_CONTENT_TYPE)
    evhttp_set_default_content_type(eh, "text/plain");
#endif
    // some default settings for all http listeners
    evhttp_set_max_headers_size(eh, gopts->max_http_headers_size);
    evhttp_set_max_body_size(eh, gopts->max_http_body_size);
    evhttp_set_timeout(eh, gopts->max_http_timeout);
    evhttp_set_allowed_methods(eh, EVHTTP_REQ_GET|EVHTTP_REQ_HEAD);
}

server::server(const server_options& opts, handler_base& h) :
    handler(h)
{
    strictly_synchronous_handlers = h.strictly_synchronous();
    if(!strictly_synchronous_handlers){
        // libevent has several commits entitled:
        //   "Fix crashing http server when callback do not reply in place from *gencb*"
        // See:
        //  https://github.com/libevent/libevent/issues/695
        //  https://github.com/libevent/libevent/commit/306747e51c1f0de679a3b165b9429418c89f8d6a
        //  https://github.com/libevent/libevent/commit/5ff8eb26371c4dc56f384b2de35bea2d87814779
        //  https://github.com/libevent/libevent/commit/b25813800f97179b2355a7b4b3557e6a7f568df2
        //
        // The first two commits are on the 2.2 branch (not yet
        // released as of Jan 2020), but the last one is on the 2.1
        // branch, and is part of the 2.1.9 and later releases.
        //
        // We can't continue if we don't have those patches:
        if(event_get_version_number() < 0x02010900){
            throw std::runtime_error(fmt("Asynchronous handlers require libevent 2.1.9 or newer.  This binary is currently compiled with %s and linked with 0x%x.  Recompile and relink.", LIBEVENT_VERSION, event_get_version_number()));
        }
        evthread_use_pthreads();
    }
    gopts = std::make_unique<const server_options>(opts);
    the_secret_manager = h.get_secret_manager();

    if(gopts->libevent_debug){
#ifdef EVENT_DBG_ALL
        event_enable_debug_logging(EVENT_DBG_ALL);
        set_complaint_level(LOG_DEBUG);
#else
        complain(LOG_WARNING, "this version of libevent does not have an EVENT_DBG_ALL option");
#endif
    }

    if(gopts->exit_after_idle > 0){
        idle_timeout = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(gopts->exit_after_idle));
    }else{
        idle_timeout = std::chrono::system_clock::duration::max();
    }

    // somebody's going to write to a pipe with nobody at the
    // other end.  We don't want to hear about it...
    struct sigaction sa = {};
    sa.sa_handler = SIG_IGN;
    sew::sigaction(SIGPIPE, &sa, 0);

    event_set_log_callback(dup_suppress_log);
    ebac = make_autocloser(event_base_new(), event_base_free);
    if (!ebac)
	throw se(errno, "event_base_new failed");

    ehac = make_autocloser(evhttp_new(ebac), evhttp_free);
    if (!ehac)
	throw se(errno, "evhttp_new failed");
    DIAGf(_fs123server, "evhttp_bind_socket_with_handle(%p, %s, %d)",
          ehac.get(), gopts->bindaddr.c_str(), gopts->port);
    ehsock = evhttp_bind_socket_with_handle(ehac, gopts->bindaddr.c_str(), gopts->port);
    if (ehsock == nullptr)
        throw se(errno, "evhttp_bind_socket failed");
    auto sockfd = evhttp_bound_socket_get_fd(ehsock);
    evutil_make_listen_socket_reuseable(sockfd);

    armup = setup_async(ebac, ehac);
    setup_evhttp(ehac, armup.get());

    // Set up the done-checker for the primary thread:
    donecheck_cb_arg = std::make_unique<donecheck_cb_arg_t>(ebac, this);
    donecheck_ev = make_autocloser(event_new(ebac, -1, EV_PERSIST, donecheck_cb, donecheck_cb_arg.get()), ::event_free);
    if(!donecheck_ev)
        throw se("event_new(..., donecheck_cb) failed");
    const struct timeval donecheck_tv{thread_done_delay_secs, 0};
    if (event_add(donecheck_ev, &donecheck_tv) < 0)
        throw se("event_add donecheck failed");
}

server::~server(){
    for(auto e : events2befreed)
        event_free(e);
    events2befreed.clear();
}

req::req(evhttp_request* evreq, server* _server, async_reply_mechanism* _arm) :
    evhr(evreq),
    svr(*_server),
    arm(_arm),
    replied(false),
    synchronous_reply(_server->strictly_synchronous_handlers)
{
    method = evhttp_request_get_method(evreq);
    uri = evhttp_request_get_uri(evreq); // unparsed, not decoded
}

req::~req(){
    // see comment about deleted constructors in fs123server.hpp.
    if(!replied)
        exception_reply(http_exception(500, "fs123p7::req destroyed before xxx_reply called."));
}

void req::errno_reply(int eno, const std::string& cc) {
    DIAGf(_errs, "errno_reply(eno=%d, cc=%s)\n", eno, cc.c_str());
    common_reply200(cc, 0, std::to_string(eno).c_str());
}

void req::not_modified_reply(const std::string& cc) {
    if(inm.empty())
        httpthrow(500, "handler called not_modified_reply but there is no If-None-Match header");
    auto ohdrs = evhttp_request_get_output_headers(evhr);
    add_hdr(ohdrs, "ETag", inm);
    add_hdr(ohdrs, "Cache-control", cc.c_str());
    log_and_send_destructively(304);  // 304 is the HTTP Not Modified code.
}

void req::redirect_reply(const std::string& location, const std::string& cc) {
    auto ohdrs = evhttp_request_get_output_headers(evhr);
    add_hdr(ohdrs, "Location", location.c_str());
    add_hdr(ohdrs, "Cache-control", cc.c_str());
    log_and_send_destructively(302);  // 302 is the HTTP Found status
}

void req::a_reply(const struct stat& sb, uint64_t validator, uint64_t esc, const std::string& cc) try {
    if(function != "a")
        httpthrow(500, "handler replied to " + std::string(function) + " with a_reply");
    auto evreq = evhr;
    auto ohdrs = evhttp_request_get_output_headers(evreq);
    add_hdr(ohdrs, HHCOOKIE, std::to_string(esc));
    std::ostringstream oss;
    // No longer support proto_minor=0.  Always append the monotonic validator.
    oss << sb << '\n' << validator;
    copy_to_pbuf(oss.str());
    common_reply200(cc);
 }catch(std::exception& e) { exception_reply(e); }

bool req::add_dirent(core123::str_view name, long offset, int type, uint64_t estale_cookie){
    if(function != "d")
        httpthrow(500, "handler called add_dirent while handling " + std::string(function) + " request");
    if(name.size() > 255) // 255 == NAME_MAX on Linux and is hardwired into the client as well
        throw core123::se(ENAMETOOLONG, "dirbuf::add");
    if(name.size() == 0)
        throw core123::se(EINVAL, "dirbuf::add:  zero-length name");
    std::ostringstream oss;
    oss << core123::netstring(name) << " " << type << " " << estale_cookie << "\n";
    size_t osslen = core123::ostream_size(oss);
    if(osslen >= buf.avail_back())  // >=, not > so there's room for the final newline
        return false;
    buf = buf.append(oss.str());
    dir_lastoff = offset;
    return true;
}

#ifndef __APPLE__
bool req::add_dirent(const ::dirent& de, uint64_t estale_cookie){
    return add_dirent(de.d_name, de.d_off, de.d_type, estale_cookie);
}
#else
bool req::add_dirent(const ::dirent& de, uint64_t estale_cookie, long d_off){
    return add_dirent(de.d_name, d_off, de.d_type, estale_cookie);
}
#endif

void req::add_header(const std::string& name, const std::string& value){
    if(function != "p")
        httpthrow(500, "handler called add_header while handling " + std::string(function) + " request");
    add_hdr(evhttp_request_get_output_headers(evhr), name, value);
}

void req::d_reply(bool at_eof, uint64_t etag64, uint64_t esc, const std::string& cc) try {
        if(function != "d")
            httpthrow(500, "handler replied to " + std::string(function) + " with d_reply");
        auto evreq = evhr;
        auto ohdrs = evhttp_request_get_output_headers(evreq);

        add_hdr(ohdrs, HHCOOKIE, std::to_string(esc));
        long final_telldir;
        if(buf.empty()){
            if(!at_eof)
                httpthrow(500, "empty dirents and not EOF.  Something is wrong");
            final_telldir = 0; // does not matter.
        }else{
            final_telldir = dir_lastoff;
        }
        buf = buf.append("\n");            // N.B.  we left room for this in dirbuf.add()
        add_hdr(ohdrs, HHNO, std::to_string(final_telldir) + (at_eof?" EOF":""));
        common_reply200(cc, etag64);
 }catch(std::exception& e) { exception_reply(e); }

void req::f_reply(size_t nread, uint64_t content_validator, uint64_t etag64, uint64_t esc, const std::string& cc) try {
        if(function != "f")
            httpthrow(500, "handler replied to " + std::string(function) + " with f_reply");
        auto evreq = evhr;
        auto ohdrs = evhttp_request_get_output_headers(evreq);
        if(nread > requested_len)
            httpthrow(500, "f_reply called with nread > requested number of bytes");
        
        buf = buf.grow_back(nread);
        DIAGf(_fs123server, "prepend content validator (%llu) to buf.  buf.avail_front() = %zd, buf.avail_back()=%zd",
              (unsigned long long)content_validator, buf.avail_front(), buf.avail_back());
        if(proto_minor >= 2) // always?
            buf = buf.prepend(core123::netstring(std::to_string(content_validator)));

        add_hdr(ohdrs, HHCOOKIE, std::to_string(esc));
        common_reply200(cc, etag64);
 }catch(std::exception& e) { exception_reply(e); }

void req::l_reply(const std::string& target, const std::string& cc) try {
        if(function != "l")
            httpthrow(500, "handler replied to " + std::string(function) + " with l_reply");
        copy_to_pbuf(target);
        common_reply200(cc);
 }catch(std::exception& e) { exception_reply(e); }

void req::s_reply(const struct statvfs& sv, const std::string& cc) try {
        if(function != "s")
            httpthrow(500, "handler replied to " + std::string(function) + " with s_reply");
        std::ostringstream oss;
        oss << sv;
        copy_to_pbuf(oss.str());
        common_reply200(cc);
 }catch(std::exception& e) { exception_reply(e); }

void req::x_reply(const std::string& xattr, const std::string& cc) try {
        if(function != "x")
            httpthrow(500, "handler replied to " + std::string(function) + " with x_reply");
        copy_to_pbuf(xattr);
        common_reply200(cc);
 }catch(std::exception& e) { exception_reply(e); }

void req::n_reply(const std::string& body, const std::string& cc) try {
        if(function != "n")
            httpthrow(500, "handler replied to " + std::string(function) + " with n_reply");
        copy_to_pbuf(body + str(server_stats));
        common_reply200(cc);
 }catch(std::exception& e) { exception_reply(e); }

void req::p_reply(const std::string& body, uint64_t etag64, const std::string& cc) try {
        if(function != "p")
            httpthrow(500, "handler replied to " + std::string(function) + " with p_reply");
        copy_to_pbuf(body);
        common_reply200(cc, etag64, nullptr); // if fs123-errno is needed, the handler should have done add_header.
 }catch(std::exception& e) { exception_reply(e); }

void server::add_sig_handler(int sig, std::function<void(int, void*)> cb, void* arg){
    if(!ebac)
        throw se(EINVAL, "Oops.  add_sig_handler called too soon!");
    sig_cb_adapter_data_ll.emplace_back(sig, cb, arg);
    auto& ad = sig_cb_adapter_data_ll.back(); // in C++17, emplace_back returns this
    struct event* e = event_new(ebac, sig, EV_SIGNAL|EV_PERSIST,
                                sig_cb_adapter, &ad);
    if(!e)
        throw se(errno, "event_new on signal failed");
    events2befreed.push_back(e);
    if(event_add(e, nullptr) < 0)
        throw se(errno, "event_add on signal failed");
}

// The *only* inter-thread communication is through the done atomic.
// Secondary threads check it periodically.  We have to pack
// the address of done and a pointer to our event_base into the
// arg that  we pass to the cb.
void
server::donecheck_cb(evutil_socket_t, short, void *varg){
    auto& arg = *(donecheck_cb_arg_t*)varg;
    auto svr = std::get<server*>(arg);
    if (svr->done.load() ||
        (std::chrono::system_clock::now() - svr->time_of_last_request.load()) > svr->idle_timeout )
        event_base_loopbreak(std::get<event_base*>(arg));
}

void
server::run() try {
    // Start additional http listener/server threads if requested:
    std::vector<std::thread> threads;
    if (gopts->nlisteners > 1) {
	// by using a separate event base and http listener for each
	// thread, all events for each thread are kept separate so no
	// inter-thread synchronization is needed (other than the done
	// atomic).  The trick to starting additional libevent http
	// servers on the same (already bound) socket is to get the
	// socket fd and call evhttp_bind_listener on a newly created
	// evhttp_connlistener (see the comment in
	// sever::evhttp_bind_socket).  Accepts() will round-robin
	// randomly across the different libevent http servers, and
	// the sockets produced by those accepts will then be handled
	// in the lucky thread that gets the fd on accept; the other
	// threads do wakeup, but get -1 (EAGAIN) and dive back into
	// epoll_wait.  Keepalive should work fine too, since the same
	// thread handles that socket thereafter.  Each thread can
	// handle lots of connections/clients, thanks to each thread
	// having a separate event loop.
	auto threadrun = [this] () {
	    try {
                // Do a bunch of things that were done in the primary
                // thread in the server constructor.  FIXME - refactor
                // this so it's all in one place!
		auto ebthr = make_autocloser(event_base_new(), event_base_free);
		if (!ebthr)
		    throw se("thread failed to create event_base");
		auto ehthr = make_autocloser(evhttp_new(ebthr.get()), evhttp_free);
		if (!ehthr)
		    throw se("thread failed to create evhttp");
                auto armthr = setup_async(ebthr, ehthr); // unique_ptr.  Will be destroyed when lambda returns
                // N.B.  armthr is a unique_ptr.  It will be destroyed when the
                // lambda returns (after event_base_loop is done).
                evhttp_bind_socket(ebthr, ehthr);
		setup_evhttp(ehthr, armthr.get());
                donecheck_cb_arg_t donecheck_cb_argthr(ebthr.get(), this);
		auto e = event_new(ebthr, -1, EV_PERSIST, donecheck_cb, &donecheck_cb_argthr);
		const struct timeval donecheck_tv{thread_done_delay_secs, 0};
		if (event_add(e, &donecheck_tv) < 0)
		    throw se("event_add donecheck failed");

                // LOOP UNTIL SOMEBODY DOES done.store(true) 
		if (event_base_loop(ebthr, 0) < 0) 
		    throw se("thead event_base_loop failed");
                event_free(e);
	    } catch(std::exception &e) {
		complain(e, "fs123p7::server::run:  listener thread terminating unexpectedly on exception.");
	    }
	};
	// already running one main thread so start count at 1
	for (unsigned i = 1; !done.load() && i < gopts->nlisteners; i++) {
	    threads.emplace_back(threadrun);
	}
    }

    // LOOP UNTIL SOMEBODY DOES done.store(true)
    if (event_base_loop(ebac, 0) < 0)
	complain("event_base_loop failed");

    complain(LOG_NOTICE, "server::run:  primary thread returned from event_base_loop.  Joining secondary threads");
    done.store(true);
    for (auto& t : threads) {
	t.join();
    }

    DIAGfkey(_proc, "finished main thread proc\n");
 }catch(std::exception& e){
    done.store(true);
    std::throw_with_nested(std::runtime_error("fs123p7::server::run:  exception caught in primary thread.  This may not end well... done.store(true) called.  Maybe the secondary threads will exit on their own?"));
 }

void server::stop(){
    done.store(true);
}
} // namespace fs123p7
