#include "fs123request.hpp"
#include "selector_manager.hpp"
#include "fs123/httpheaders.hpp"
#include <core123/complaints.hpp>
#include <core123/strutils.hpp>
#include <core123/scoped_nanotimer.hpp>
#include <core123/http_error_category.hpp>
#include <core123/autoclosers.hpp>
#include <core123/diag.hpp>
#include <core123/sew.hpp>
#include <core123/svto.hpp>
#include <gflags/gflags.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <string>
#include <cstring>
#include <memory>

#include <netinet/tcp.h>    // for TCP_NODELAY

using namespace core123;

auto _bufev = diag_name("bufev");
auto _fs123req = diag_name("fs123req");

// Calling back-to-back writes with sizes smaller than small multiples
// of MSS (TCP_MAXSEG) tickles the Nagle Algorithm which, in turn,
// interacts very badly with Delayed Ack resulting in
// multi-millisecond latencies on retrievals near the MSS size.  On
// Linux, the localhost interface defaults to TCP_MAXSEG=65468.
// Ethernet interfaces seem to default to 1448.  To avoid Nagle, we
// want our outbound replies to go in a single write(), so if the
// client is making requests with ckib of 128, then this should
// be at least 131072+maximum_http_header_size.  But we know of no
// reason to try to "finesse" it.  Let's just make it big and
// hopefully not worry about it ever again (famous last words).
//
// bufferevent_max_single_write was introduced in libevent 2.1, so:
#if EVENT__NUMERIC_VERSION >= 0x2020000
#define HAVE_BUFFEREVENT_SET_MAX_SINGLE_WRITE
DEFINE_uint64(max_single_write, 16*1024*1024, "maximum number of bytes in any single write to an http socket");
#endif

// setting TCP_NODELAY is another way to avoid the problems that arise
// from the Nagle algorithm when write()s smaller than MSS.  Other
// alternatives are to adjust the MTU with ifconfig or the MSS with ip
// route.  The evhttp API in libevent doesn't give us a per-accept
// callback, so --tcp_nodelay does a setsockopt TCP_NODELAY for every
// request, even for keepalive connections, which seems wasteful.
// max_single_write is definitely preferred.
DEFINE_bool(tcp_nodelay, false, "set TCP_NODELAY on accepted sockets");

using namespace core123;

fs123Req::fs123Req(evhttp_request* evreq) :
    evreq_(evreq), decoded_path_{nullptr, std::free}
{
    scoped_nanotimer snt;

    method_ = evhttp_request_get_command(evreq_);
    switch (method_) {
    case EVHTTP_REQ_GET:
    case EVHTTP_REQ_HEAD:
        break;
    default:
        httpthrow(403, std::string("Unsupported request method ") + std::to_string(evhttp_request_get_command(evreq_))); break;
    }

    auto evcon = evhttp_request_get_connection(evreq_);
    if(evcon == nullptr)
        throw se(EINVAL, "No connection?");
    auto ehbe = evhttp_connection_get_bufferevent(evcon);

#ifdef HAVE_BUFFEREVENT_SET_MAX_SINGLE_WRITE
    bufferevent_set_max_single_write(ehbe, FLAGS_max_single_write);
#endif    

    if (FLAGS_tcp_nodelay) {
        int nodelay = 1;
        auto ehbesock = bufferevent_getfd(ehbe);
        sew::setsockopt(ehbesock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    auto inheaders = evhttp_request_get_input_headers(evreq_);
    const char *ae = evhttp_find_header(inheaders, "accept-encoding");
    if(ae)
        accept_encoding_ = ae;

    auto uri = evhttp_request_get_evhttp_uri(evreq_);
    // The uri (and the path and query) have the same lifetime as
    // evereq.  Getting the path and query are pretty much free.
    // get_evhttp_uri paid the price to pick them apart and strdup
    // them when it constructed the uri.
    path_ = evhttp_uri_get_path(uri);
    DIAGfkey(_fs123req, "path=%s\n", path_);
    if(path_ == nullptr)
        throw se(EINVAL, "evhttp_uri_get_path returned NULL");
    size_t decoded_path_sz;
    decoded_path_ = std::unique_ptr<char, decltype(std::free)*>{evhttp_uridecode(path_, 0, &decoded_path_sz), std::free};
    if (!decoded_path_)
        throw se(EINVAL, "failed to uridecode path");

    // The generic URL should look like:
    //   /SEL/ECT/OR/SIGIL/PROTOMajor[/PROTOMinor]/FUNCTION/relat/ive/to/expo/rt_ro/ot?QUERY
    // SIGIL = "fs123"
    // PROTOMajor and PROTOMinor are integers
    // FUNCTION is a single-letter.
    static char const SIGIL[] = "/fs123/";
    char *start_of_sigil = ::strstr(decoded_path_.get(), SIGIL);
    if(!start_of_sigil)
        httpthrow(400, "SIGIL not found in decoded path");

    *start_of_sigil = '\0';
    selector_ = decoded_path_.get();
    // Look for PROTOmajor[/PROTOminor]/FUNCTION
    size_t end_of_sigil = (start_of_sigil - selector_) + sizeof(SIGIL)-1;
    str_view sv(selector_ + end_of_sigil, decoded_path_sz - end_of_sigil);
    size_t nextoff;
    try{
        nextoff = svscan(sv, &url_proto_major_, 0) + 1;
        // nextoff is one past the slash that follows PROTOmajor
        if(sv.size() <= nextoff || sv[nextoff-1] != '/')
            httpthrow(400, "expected /... after PROTOmajor");
        url_proto_minor_ = 0;
        if( ::isdigit(sv[nextoff]) ){
            // It's /PROTOmajor/PROTOminor
            nextoff = svscan(sv, &url_proto_minor_, nextoff) + 1;
            // nextoff is one past the slash that follows PROTOmajor/PROTOminor
            if(sv.size() <= nextoff || sv[nextoff-1] != '/')
                httpthrow(400, "expected /... after PROTOmajor/PROTOminor");
        }
    }catch(std::exception& e){
        std::throw_with_nested(http_exception(400, "Couldn't parse PROTO at: .../" + std::string(sv)));
    }
    
    auto pidx = sv.find('/', nextoff);
    DIAGfkey(_fs123req, "pidx=%zu, nextoff=%zu, sv.size(): %zu, sv: %s\n", pidx, nextoff, sv.size(), std::string(sv).c_str());
    if(pidx != str_view::npos){
        // there is a slash after /FUNCTION.  The slash
        // is the first character of path_info.
        path_info_ = std::string(sv.substr(pidx));
        function_ = std::string(sv.substr(nextoff, pidx-nextoff));
    }else{
        // there's no next slash.  The function_ extends to
        // the end of the decoded_path.  The path_info itself is empty.
        path_info_ = "";
        function_ = std::string(sv.substr(nextoff));
    }
    DIAGfkey(_fs123req, "function: %s, path_info: %s\n", function_.c_str(), path_info_.c_str());

    const char *q = evhttp_uri_get_query(uri);
    query_ = q ? q : "";
    DIAGfkey(_fs123req, "fs123Req sel \"%s\" proto %d/%d function \"%s\" path \"(%s) len=%zu\" query \"%s\"\n",
	     selector_, url_proto_major_, url_proto_minor_, function_.c_str(), path_info_.c_str(), path_info_.size(), query_.c_str());
 }
