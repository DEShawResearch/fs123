#include "backend123_http.hpp"
#include "opensslthreadlock.h"
#include "fs123/httpheaders.hpp"
#include "fs123/content_codec.hpp"
#include "fs123/acfd.hpp"
#include <core123/complaints.hpp>
#include <core123/scoped_nanotimer.hpp>
#include <core123/expiring.hpp>
#include <core123/sew.hpp>
#include <core123/http_error_category.hpp>
#include <core123/diag.hpp>
#include <core123/exnest.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/strutils.hpp>
#include <core123/svto.hpp>
#include <core123/envto.hpp>
#include <curl/curl.h>
#include <cctype>
#include <deque>
#include <mutex>
#include <cstddef>
#include <exception>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <regex>

using namespace core123;

static auto _http = diag_name("http");
static auto _namecache = diag_name("namecache");
static auto _transactions = diag_name("transactions");

namespace{

#define LIBCURL_STATISTICS \
    STATISTIC(curl_inits) \
    STATISTIC(curl_cleanups) \
    STATISTIC(curl_reuses) \
    STATISTIC_NANOTIMER(backend_curl_perform_inuse_sec)
#define STATS_STRUCT_TYPENAME libcurl_statistics_t
#define STATS_MACRO_NAME LIBCURL_STATISTICS
#include <core123/stats_struct_builder>

libcurl_statistics_t libcurl_stats;
    
// libcurl docs say we should call curl_global_init when there's only
// one thread running and we should balance it with
// curl_global_cleanup.  See opensslthreadlock.c for
// thread_{setup,cleanup}.  We could also do it in main, which would
// allow us to do it after fiddling with mallopt options, but here
// seems safer, since it will outlive any handlers running in detached
// threads.
struct curl_global_raii{
    curl_global_raii(){
        curl_global_init(CURL_GLOBAL_ALL);
        thread_setup();
    }
    ~curl_global_raii(){
        thread_cleanup();
        curl_global_cleanup();
    }
};
static curl_global_raii curl_global;

// throw a system_error in the libcurl_category().  Note that by
// convention, if the CURLcode is 0 (CURLE_OK), the catcher is "advised"
// to suppress the error report.  But ultimately, the decision is made
// at the catch site (see caught() in mount.fs123.cpp and handler
// in diskcache::fg_refresh in diskcache.cpp).  
[[noreturn]] inline void 
libcurl_throw(CURLcode c, const std::string& msg, int os_errno=0){
    throw libcurl_category_t::make_libcurl_error(c, os_errno, msg);
}

// Wrapping the easy_setopts so they throw on error is slightly
// tricky.  Use function overloading on the type of the last argument.
void wrap_curl_easy_setopt(CURL* curl, CURLoption option, long l){
    auto ret = curl_easy_setopt(curl, option, l);
    if(ret != CURLE_OK)
        libcurl_throw(ret, fmt("curl_easy_setopt(%p, %d, (long)%ld)", curl, option, l));
}

void wrap_curl_easy_setopt(CURL* curl, CURLoption option, void *v){
    auto ret = curl_easy_setopt(curl, option, v);
    if(ret != CURLE_OK)
        libcurl_throw(ret, fmt("curl_easy_setopt(%p, %d, (void*)%p)", curl, option, v));
}

void wrap_curl_easy_setopt(CURL* curl, CURLoption option, const std::string& s){
    auto ret = curl_easy_setopt(curl, option, s.c_str());
    if(ret != CURLE_OK)
        libcurl_throw(ret, fmt("curl_easy_setopt(%p, %d, %s)", curl, option, s.c_str()));
}

void wrap_curl_easy_setopt(CURL *curl, CURLoption option, curl_write_callback cb){
    auto ret = curl_easy_setopt(curl, option, cb);
    if(ret != CURLE_OK)
        libcurl_throw(ret, fmt("curl_easy_setopt(%p, %d, (curl_write_callback*)%p)", curl, option, cb));
}

void wrap_curl_easy_setopt(CURL *curl, CURLoption option, int (*cb)(CURL* handle, curl_infotype, char*, size_t, void*)){
    auto ret = curl_easy_setopt(curl, option, cb);
    if(ret != CURLE_OK)
        libcurl_throw(ret, fmt("curl_easy_setopt(%p, %d, (debug_callback*)%p)", curl, option, cb));
}

template <typename T>
void wrap_curl_easy_getinfo(CURL* curl, CURLINFO option, T *tp){
    auto ret = curl_easy_getinfo(curl, option, tp);
    if(ret != CURLE_OK)
        libcurl_throw(ret, fmt("curl_easy_getinfo(%p, %d, %p)", curl, option, tp));
}

struct wrapped_curl_slist{
    // construct an empty slist
    wrapped_curl_slist() : chunk{nullptr} {}
    // The copy-constructor and assignment operators are asking
    // for trouble.  Delete them until/unless we need them.
    wrapped_curl_slist(const wrapped_curl_slist& rhs) = delete;
    wrapped_curl_slist(wrapped_curl_slist&&) = delete;
    wrapped_curl_slist& operator=(const wrapped_curl_slist&) = delete;
    wrapped_curl_slist& operator=(wrapped_curl_slist&&) = delete;
    
    curl_slist* get() const{
        return chunk;
    }

    void append(const std::string& s){
        append(s.c_str());
    }

    template<typename ITER>
    void append(ITER b, ITER e){
        while(b!=e)
            append(*b++);
    }

    void append(const char *s){
        auto newchunk = curl_slist_append(chunk, s);
        if(newchunk == nullptr){
            // Is this recoverable???  Did append
            // trash the existing list?  Let's assume it didn't,
            // in which case leaving the original 'chunk' untouched
            // is the least damaging/surprising thing we can do.
            throw se(EIO, "curl_slist_append returned NULL.  This might be very bad.");
        }
        chunk = newchunk;
    }

    ~wrapped_curl_slist(){
        reset();
    }

    void reset(){
        if(chunk)
            curl_slist_free_all(chunk);
        chunk = nullptr;
    }
private:
    curl_slist* chunk;
};

inline // silence a -Wunused-function warning
std::ostream& operator<<(std::ostream& os, const curl_slist* sl){
    os << "slist[";
    for( ; sl; sl=sl->next){
        os << ((sl->data)?sl->data:"<null>");
        if(sl->next)
            os << "|";
    }
    os << "]";
    return os;
}

// Let's build some RAII autoclosing magic for CURL*.
struct CURLcloser{
    void operator()(CURL *c){ ::curl_easy_cleanup(c); libcurl_stats.curl_cleanups++; }
};
 using CURL_ac = autocloser_t<::CURL, CURLcloser, fs123_autoclose_err_handler>;

#if 1
thread_local CURL_ac tl_curl;

// get_curl - returns an autoclosing CURL_ac.  If the caller lets it
// go out-of-scope, the autocloser calls curl_easy_cleanup.  BUT - if
// the caller calls release_curl, instead, then the RAII object will
// be stashed in the thread_local 'tl_curl', where it may be found and
// reused by a subsequent call to get_curl.
//
// Bottom line: release_curl is optional and highly recommended, but
// not mandatory.  It's perfectly reasonable to skip the call to
// release_curl if anything "funny" happens while using the CURL_ac
// object.  This should avoid cascading failures, with minimal
// performance impact.
CURL_ac get_curl(){
    if(tl_curl){
        curl_easy_reset(tl_curl.get());
        libcurl_stats.curl_reuses++;
        return std::move(tl_curl);
    }
    libcurl_stats.curl_inits++;
    return CURL_ac(curl_easy_init());
}

void release_curl(CURL_ac c){
    tl_curl = std::move(c);
}

#else
const int CURLpool_target_size = 10;
std::deque<CURL_ac> CURLpool;
std::mutex CURLpoolmtx;
CURL_ac get_curl(){
    CURL_ac ret;
    std::lock_guard<std::mutex> lg(CURLpoolmtx);
    if( CURLpool.empty() ){
        libcurl_stats.curl_inits++;
        ret.reset(curl_easy_init());
    }else{
        libcurl_stats.curl_reuses++;
        std::swap(ret, CURLpool.back());
        CURLpool.pop_back();
    }
    if(!ret)
        throw se(EINVAL, "curl_easy_init failed");
    curl_easy_reset(ret);  // returns void.  No need for wrap_
    return ret;
}

void release_curl(CURL_ac c){
    std::lock_guard<std::mutex> lg(CURLpoolmtx);
    if(CURLpool.size() < CURLpool_target_size)
        CURLpool.push_back(std::move(c));
}
#endif

// stats are a singleton, so the RefcountedScopedTimerCtl ought to be
// a singleton too, even if by some miracle we manage to instantiate
// multiple backend_http's.
refcounted_scoped_nanotimer_ctrl refcountedtimerctrl(libcurl_stats.backend_curl_perform_inuse_sec);

int curl_debug_to_diag_stream(CURL* /*handle*/, curl_infotype type, char *data, size_t /*size*/, void */*userptr*/){
    switch(type){
    case CURLINFO_TEXT:
        DIAGf(_http, "CURLINFO_TEXT: %s", data);
        return 0;
    default:
        return 0;
    }
    return 0;
}

} // namespace <anonymous>
 
const std::error_category& libcurl_category() noexcept{
    static libcurl_category_t libcurl_category_singleton;
    return libcurl_category_singleton;
}

url_info::url_info(const std::string& url)
    : original(url), deferred_until(std::make_unique<std::atomic<tp_type>>(tp_type::min()))
{
    // This re is from rfc3986,
    //                       ^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))?
    //                        12        2 1 3  4       43 5      56  7     76 8 9  98
    // which matches the scheme in $2, and the authority in $4.  But we
    // want to tease apart the hostname and the port from the authority,
    // and we don't care about teasing apart the path ($5), query ($7) or
    // fragment ($9).  So:
    static std::regex urlre("^(([^:/?#]+):)?(//([^/?#:]*))?(.*)");
    //                        12        2 1 3  4        43 5  5
    //  scheme-with-trailing-colon: $1
    //  hostname: $4
    //  optional-port-plus-rest-of-url: $5  

    // regex for matching numeric IP addresses.  Note that this also
    // matches 999.888.777.666.  I don't think we care...
    static std::regex dotted_quadre("\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}");

    std::smatch mr;
    if(!std::regex_search(url, mr, urlre) || mr.str(1).empty() || mr.str(4).empty()){
        complain(LOG_WARNING, "Can't find a hostname in url:" + url + ".  It will not be added to the addrinfo_cache.  Maybe libcurl can make sense of it...");
        do_not_lookup = true; // maybe libcurl can make sense of this url.  We can't...
        return;
    }

    before_hostname = mr.str(1) + "//";
    hostname = mr.str(4);
    after_hostname = mr.str(5);
    do_not_lookup = std::regex_match(hostname, dotted_quadre);
}

struct backend123_http::curl_handler{
    // Callbacks *CAN NOT* be non-static class members.  So we provide
    // static class members and arrange that 'this' is passed through
    // the userdata argument.
    static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata){
        curl_handler* ch = (curl_handler *)userdata;
        ch->bep->stats.backend_header_callbacks++;
        atomic_scoped_nanotimer _t(&ch->bep->stats.backend_header_callback_sec);
        try{
            ch->recv_hdr(buffer, size, nitems);
            ch->bep->stats.backend_header_bytes_rcvd += size * nitems;
            return size*nitems;
        }catch(...){
            // We don't want exceptions thrown "over" the libcurl C 'perform' function.
            ch->exptr = std::current_exception();
            return 0;
        }
    }

    static size_t write_callback(char *buffer, size_t size, size_t nitems, void *userdata){
        curl_handler* ch = (curl_handler *)userdata;
        atomic_scoped_nanotimer _t(&ch->bep->stats.backend_write_callback_sec);
        ch->bep->stats.backend_write_callbacks++;
        try{
            ch->recv_data(buffer, size, nitems);
            ch->bep->stats.backend_body_bytes_rcvd += size * nitems;
            return size * nitems;
        }catch(...){
            // We don't want exceptions thrown "over" the libcurl C 'perform' function.
            ch->exptr = std::current_exception();
            return 0;
        }
    }

    curl_handler(backend123_http* bep_) :
        bep(bep_), exptr{}, content{}, hdrmap{}
    {
        if(bep->content_reserve_size>0)
            content.reserve(bep->content_reserve_size);
    }

    backend123_http* bep;
    std::exception_ptr exptr;
    std::string content;
    // Note that the keys in hdrmap are all lower-case, e.g.,
    // "cache-control", "age", "fs123-errno".  Regardless
    // of how they were spelled by the origin server or proxies.
    std::map<std::string, std::string> hdrmap;
    long http_code;
    char curl_errbuf[CURL_ERROR_SIZE]; // 256
    std::vector<std::string> headers;
    wrapped_curl_slist connect_to_sl;
    wrapped_curl_slist headers_sl;

    void reset(){
        content.clear();
        hdrmap.clear();
        exptr = nullptr;
    }

    bool perform_without_fallback(CURL *curl, const url_info& baseurli, const std::string& urlstem, reply123* replyp, int recursion_depth = 0){
        // The curl_slist API doesn't support deletion or replacement.
        // Since we can't replace the Host header, we re-initialize
        // the headers_sl curl_slist with the common headers and
        // conditionally append a Host header to it every time through.
        //
        // N.B. The docs for both CURLOPT_HTTPHEADER and
        // CURLOPT_CONNECT_TO say this about the slist: "When this
        // option is passed to curl_easy_setopt, libcurl will not copy
        // the entire list, so you *must* keep it around until you no
        // longer use this `handle` for a transfer before you call
        // curl_slist_free_all on the list".
        //
        // It's not completely clear what the rules really are, or
        // whether we're following them.  We do always call
        //   curl_setoption(curl, CURLOPT_HTTPHEADERS, valid-pointer)
        // immediately before any call to curl_easy_perform().  And
        // the valid pointer remains valid through the call to
        // curl_easy_perform and while we retrieve the results.
        // HOWEVER - between calls to curl_easy_perform, (with the same
        // handle), we might call curl_slist_free_all, and initialize
        // and append to a "new" slist, which is passed to the next
        // call to curl_setoption.  We might also call
        // curl_easy_reset().  This looks safe (from a quick read of the
        // libcurl code), but it's not promised by libcurl's
        // documentation.
        headers_sl.reset();
        headers_sl.append(headers.begin(), headers.end());
        std::string burl = baseurli.original;
        if(bep->vols.namecache && !baseurli.do_not_lookup){
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_socktype = SOCK_STREAM;
            auto ai_result = bep->aicache.lookup(baseurli.hostname, {}, &hints);
            bep->stats.aicache_lookups++;
            if(ai_result->status != 0)
                complain(LOG_WARNING, str("addrinfo_cache.lookup(", baseurli.hostname, ") returned ", ai_result->status));
            unsigned naddrs = 0; // count the addresses in ai_result
            for(auto p = ai_result->aip; p!=nullptr; p = p->ai_next)
                naddrs++;
            if(naddrs>0){
                static std::atomic<uint64_t> ctr;
                // find the idx'th round-robin result
                unsigned idx = ctr.fetch_add(1) % naddrs;
                auto aip = ai_result->aip;
                for(unsigned i=0; i<idx; ++i)
                    aip = aip->ai_next;
                // convert to dotted decimal
                char buf[INET_ADDRSTRLEN];
                sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(aip->ai_addr);
                const char* dotted_decimal = ::inet_ntop(aip->ai_family, &sin->sin_addr, buf, sizeof(buf));
                if(dotted_decimal){
                    bep->stats.aicache_successes++;
                    // N.B.  CURLOPT_xxx are enums, NOT pp-symbols.  We can't check for them at compile-time
#if LIBCURL_VERSION_NUM >= 0x73100 // CURLOPT_CONNECT_TO is in libcurl >= 7.49
                    // This appears to be the "best" way to bypass libcurl's own
                    // resolver and tell it to connect to an address of our choosing:
                    // N.B.  if we somehow get here multiple times (e.g., via fallback),
                    // we just keep appending to the slist.  That seems to be what
                    // curl wants.
                    connect_to_sl.append(baseurli.hostname + "::" + dotted_decimal + ":");
                    wrap_curl_easy_setopt(curl, CURLOPT_CONNECT_TO, connect_to_sl.get());
                    DIAG(_namecache, "CURLOPT_CONNECT_TO: " << connect_to_sl.get());
#elif LIBCURL_VERSION_NUM >= 0x072300 // CURLOPT_RESOLVE is in 7.21.3, but a memory leak was fixed in 7.35
                    // If we don't have CURL_OPT_CONNECT_TO, we can use CURLOPT_RESOLVE
                    // to tell libcurl's resolver to to trust us instead.
                    // N.B.  if we somehow get here multiple times, we
                    // pass a length=1 slist each time.  It's not
                    // clear what the lifetime requirements are, but
                    // keeping the slist around at least until after
                    // the curl_easy_perform returns seems prudent.
                    connect_to_sl.reset();
                    connect_to_sl.append(baseurli.hostname + ":" + dotted_decimal);
                    wrap_curl_easy_setopt(curl, CURLOPT_RESOLVE, connect_to_sl.get());
                    DIAG(_namecache, "CURLOPT_RESOLVE: " << connect_to_sl.get());
#else
                    // Unfortunately, CentOS7 ships with 7.29.0 and CentOS6 ships
                    // with 7.17.7, so we need a workaround when we can't use CURLOPT_RESOLVE.
                    //
                    // Modify the URL and add a Host: header.  This isn't ideal for
                    // a couple of reasons:
                    // - Headers are associated with the CURL* handle,
                    //   and get carried along if libcurl follows redirects.
                    //   (Shouldn't be an issue if curl_handles_redirects is false.)
                    // - If CURLOPT_NETRC is in effect, curl will look up
                    //   the IP address rather than a hostname in the netrc file.
                    // - Curl uses the hostname in other ways, e.g.,
                    //   for TLS verification.
                    if(!bep->using_https){
                        burl = baseurli.before_hostname + dotted_decimal + baseurli.after_hostname;
                        headers_sl.append("Host:"+baseurli.hostname);
                        DIAG(_namecache, "replacing " << baseurli.original << " with " << burl << " and Host:" + baseurli.hostname + " header\n");
                    }else{
                        bep->vols.namecache.store(false);
                        complain(LOG_WARNING, "libcurl older than 7.35.0 cannot use the namecache with https.  Namecache disabled.  Using libcurl's resolver.");
                    }
#endif
                }else{
                    complain(LOG_ERR, "inet_ntop failed.  There's an address in the addrinfo_cache but we can't write it in dotted decimal.  How can that happen?");
                }
            }
        }
        wrap_curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_sl.get());
        wrap_curl_easy_setopt(curl, CURLOPT_URL, (void*)(burl + urlstem).c_str());
        DIAG(_http>=2, "perform_once: CURLOPT_URL: " + (burl + urlstem));
        DIAG(_http>=2, "perform_once: CURLOPT_HTTPHEADER: " << headers_sl.get());
        return perform_once(curl, replyp, recursion_depth);
    }

    // perform_with_fallback is a method of curl_handler so we can
    // keep track of the exception_ptr and so that we can report
    // progress along with any errors.  perform_with_fallback calls
    // perform_without_fallback, which calls perform_once with the
    // first viable 'baseurl', i.e., the first one whose
    // 'deferred_until' time_point is in the past.  It is assumed that
    // retry-looping takes place at a higher level.
    bool perform_with_fallback(CURL* curl, const std::string& urlstem, reply123* replyp){
        // If there's only one baseurl, i.e., no fallbacks, then go
        // straight to perform_without_fallback.  Skip the rigamarole
        // of checking and updating the list of deferrals,
        // complaining, etc.
        if(bep->baseurls.size() < 2)
            return perform_without_fallback(curl, bep->baseurls.at(0), urlstem, replyp);
        
        size_t i = 0; 
        size_t nurls = bep->baseurls.size();
        auto started_at = std::chrono::system_clock::now();
        auto dui = bep->baseurls.at(i).deferred_until->load();
        auto min_deferred = dui;
        auto imin = 0;
        while( started_at <= dui && ++i < nurls){
            dui = bep->baseurls.at(i).deferred_until->load();
            if( dui < min_deferred ){
                imin = i;
                min_deferred = dui;
            }                
        }
        if(i == nurls){
            i = imin;
            complain(LOG_WARNING, "curl_handler::perform:  All fallbacks are deferred.  Use the least deferred: " + bep->baseurls[i].original);
        }
        try{
            return perform_without_fallback(curl, bep->baseurls.at(i), urlstem, replyp);
        }catch(std::exception& e){
            auto now = std::chrono::system_clock::now();
            // There's a lot of scope for different "policy" choices
            // here.  The particular choice is that we defer
            // baseurls[i] for an amount of time equal to how long it
            // took curl to get a failure from baseurls[i].  The idea
            // is to stay away from urls that take a long time to
            // fail, e.g., the ones that fail with EHOSTUNREACH after
            // a long connection timeout.
            //
            auto howlong = now - started_at;
            // Defer for at least 5 seconds, to give the high-level
            // retry logic a chance.  This may be a mistake if the
            // failure is a 500 Internal Error that happens to be
            // path-specific (e.g., because we can't read an
            // estale-cookie), in which case we fail over all
            // connections because of one bad file.  OTOH, even if all
            // servers are 'deferred', we'll still try the least
            // deferred one, so this shouldn't be fatal.
            howlong = clip(decltype(howlong)(std::chrono::seconds(5)),
                           howlong,
                           decltype(howlong)(std::chrono::minutes(10)));
            bep->baseurls.at(i).deferred_until->store(now + howlong);
            throw_with_nested(std::runtime_error(fmt("curl_handler::perform:  url: %s deferred for %.6f",
                                                              bep->baseurls[i].original.c_str(), dur2dbl(howlong))));
        }
    }

    // perform_once calls curl_easy_perform *and* then returns the
    // result of calling getreply on the data.  It returns the value
    // returned by getreply: a bool indicating whether the reply was
    // modified (a requirement of the backend123 api that should be
    // reconsidered)
    bool perform_once(CURL* curl, reply123* replyp, int recursion_depth){
        bep->stats.curl_performs++;
        CURLcode ret;
        {
            atomic_scoped_nanotimer _t(&bep->stats.backend_curl_perform_sec);
            refcounted_scoped_nanotimer _rt(refcountedtimerctrl);
            wrap_curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf);
            curl_errbuf[0] = '\0';
            ret = curl_easy_perform(curl);
        }
        // Retrying is a VERY slippery slope.  There is already retry
        // logic in libcurl (when there are multiple A records).
        // There is also the 'fallback' logic in perform, which calls
        // us directly.  And there is retry-with-delay logic at the
        // top level in mount.fs123p7.  Think VERY hard before adding
        // more logic here.  If something is wrong in the http
        // infrastructure, the exception-handling and error-reporting
        // superstruture can and should handle it gracefully.  System
        // calls will return EIO.  Messages will go to syslog.  The
        // underlying problem will probably be fixed in short order.
        //
        // On the other hand, it's extremely easy to see whether the
        // proxy itself is down, in which case, retrying without a proxy
        // seems like an obvious win.
        //
        // Retrying after CURLE_PARTIAL_FILE is another possibility.  It
        // *may* be due to varnish's bad habit of delivering short reads, in
        // which case a retry might be a win.  But if it's another reason,
        // a retry might just add unnecessary load.  If retrying after
        // CURLE_PARTIAL_FILE, don't forget to call reset().
        if((ret == CURLE_COULDNT_CONNECT || ret == CURLE_OPERATION_TIMEDOUT) && getenv("http_proxy")){
            wrap_curl_easy_setopt(curl, CURLOPT_PROXY, "");
            complain(LOG_WARNING, "CURLE_COULDNT_CONNECT or CURLE_OPERATION_TIMEDOUT.  Proxy down?");
            // curl says it couldn't connect.  But let's check:
            if(content.empty() && hdrmap.empty())
                ret = curl_easy_perform(curl);
            else
                complain(LOG_WARNING, "hdrmap and content not empty with CURLE_COULDNT_CONNECT or CURLE_OPERATION_TIMEDOUT");
        }
        if(ret == CURLE_GOT_NOTHING){
            // We seem to see GOT_NOTHING when upstream (i.e., squid)
            // closes down a keep_alive connection.  curl_perform
            // calls sendto to send the request, which returns without
            // an error indication (should it?).  Then it calls
            // recvfrom(), which returns 0, indicating EOF, resulting
            // in curl_perform returning CURLE_GOT_NOTHING.
            // 
            // Experimentation reveals that we can *still* reuse the
            // CURL*, even though curl_perform just returned
            // CURLE_GOT_NOTHING.  strace shows calls to close,
            // socket, connect, a couple of fcntls, and getsockopt to
            // re-establish the connection, and then the
            // sendto/recvfrom.  But this time (assuming nothing else
            // is wrong), the recvfrom works and everything is fine.
            bep->stats.backend_got_nothing++;
            complain(LOG_NOTICE, "CURLE_GOT_NOTHING.  Keep-alive connection closed by upstream?");
            // curl says it got nothing.  But let's check:
            if(content.empty() && hdrmap.empty())
                ret = curl_easy_perform(curl);
            else
                complain(LOG_WARNING, "hdrmap and content not empty with CURLE_GOT_NOTHING");
        }
        if(ret != CURLE_OK){
            std::ostringstream oss;
            oss << "curl_easy_perform : CURLcode: " << ret << ": " << curl_easy_strerror(ret) << "\n" 
                << "CURLOPT_ERRORBUFFER: " << curl_errbuf << "\n"
                << verbose_complaint(curl);
            long os_errno;
            wrap_curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_errno);
            auto se = libcurl_category_t::make_libcurl_error(ret, os_errno, oss.str());
            try{
                if(exptr)
                    std::rethrow_exception(exptr);
            }catch(...){
                std::throw_with_nested(se);
            }
            throw se;
        }
        wrap_curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        gather_stats(curl);
        if(!bep->vols.curl_handles_redirects.load() &&
           recursion_depth < bep->vols.http_maxredirects.load()){
            char *url = nullptr;
            // N.B.  We could look up hdrmap["location"], but to be
            // fully correct, we'd also have to jump through hoops
            // with relative urls.  libcurl probably got this right,
            // so we don't have to.
            wrap_curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &url);
            if(url){
                DIAG(_http, "Follow redirect: level: " << recursion_depth+1 << "  new url: " << url);
                // The url_info constructor has some tricky regex's to
                // pick apart a URL.  The idea was to precompute those
                // regexs for a small number of never-changing baseurls.
                // But here we're constructing a new url_info every time
                // we follow a redirect.  It's correct, but it doesn't
                // benefit from re-use in any way.
                url_info newurli(url);
                reset(); // N.B.  clears hdrmap content.
                bep->stats.backend_30x_redirected++;
                return perform_without_fallback(curl, newurli, {}, replyp, recursion_depth+1);
            }
        }
        return getreply(replyp);
    }

    bool getreply(reply123* replyp) {
	if (_http >= 2) {
            DIAG(true, "content (size=" << content.size() << ") \"\"\"" << quopri({content.data(), std::min(size_t(512), content.size())}) << "\"\"\"\n");
	}
        if(!(http_code == 200 || http_code == 304)){
            // We're only prepared to deal with 200 and 304.  If we
            // get anything else, we throw.
            //
            // content probably has newlines, and might even contain
            // html, which is annoying if we ship it to syslog,
            // but discarding it seems wrong.  Maybe the http_throw
            // needs a better API?
            httpthrow(http_code, content);
        }
        auto age = get_age();
        auto max_age = get_max_age();
        auto swr = get_stale_while_revalidate();
        if( http_code == 304 ){
            replyp->last_refresh = clk123_t::now() - std::chrono::seconds(age);
            replyp->expires = replyp->last_refresh + std::chrono::seconds(max_age);
            replyp->stale_while_revalidate = std::chrono::seconds(swr);
            bep->stats.backend_304++;
            bep->stats.backend_304_bytes_saved += replyp->content.size();
            return false; 
        }
        // N.B.  http_code is known to be 200 from here on...

        auto ii = hdrmap.find(HHERRNO);
        if(ii == hdrmap.end())
            throw se(EINVAL, "No key matching " HHERRNO " in header, need errno");
        auto eno = svto<int>(ii->second);
        auto et64 = get_etag64();
        std::string content_encoding;
        ii = hdrmap.find("content-encoding");
        if(ii != hdrmap.end())
            content_encoding = ii->second;
        else
            content_encoding = "";
        auto ce = content_codec::encoding_stoi(content_encoding);
        ii = hdrmap.find(HHCOOKIE);
        uint64_t estale_cookie = (ii == hdrmap.end()) ? 0 : svto<uint64_t>(ii->second);
        *replyp = reply123(eno, estale_cookie, std::move(content), ce, age, max_age, et64, swr);
        // CAUTION:  content is no longer usable!!!
        if(eno!=0)
            return true;
        ii = hdrmap.find(HHTRSUM);
        if(ii != hdrmap.end()){
            const std::string& val = ii->second;
            // Look for the 32-bytes in 
            if( ii->second.find(replyp->content_threeroe, 0, 32) == std::string::npos )
                throw se(EINVAL, fmt("threeroe mismatch.  threeroe(received content)=%.32s, " HHTRSUM ": %s",
					     replyp->content_threeroe, val.c_str()));
        }
            
        ii = hdrmap.find(HHNO);
        if(ii != hdrmap.end()){
            // ii->second is one of:
            // 1-  whitespace* NUMBER whitespace*  
            // 2-  whitespace* NUMBER whitespace* "EOF" whitespace*
            // 3-  whitespace* NUMBER <anything else> !ERROR
            // In all cases, the NUMBER is replyp->chunk_next_offset
            // replyp->last_chunk depends on which case.
            size_t pos = svscan<int64_t>(ii->second, &replyp->chunk_next_offset, 0);
            const char *p = ii->second.data() + pos;
            while( ::isspace(*p) )
                ++p;
            if( *p == '\0')
                replyp->chunk_next_meta = reply123::CNO_NOT_EOF;
            else if(::strncmp(p, "EOF", 3)==0)
                replyp->chunk_next_meta = reply123::CNO_EOF;
            else
                throw se(EPROTO, "Unrecognized words in " HHNO " header:" + ii->second);
        }else{
            replyp->chunk_next_meta = reply123::CNO_MISSING;
        }
        return true;
    }
        
    std::string verbose_complaint(CURL *curl) const{
        std::ostringstream oss;
        oss<< "Headers:\n";
        for(const auto& p : hdrmap)
            oss << p.first << ": " << p.second; // p.second ends with crlf
        oss << "Received " << content.size() << " bytes of data\n";
        // What else can we report that might help to diagnose curl
        // errors?  Are we under heavy load??  The number of active
        // handlers is an indicator:
        oss << std::fixed; // default precision of 6 gives microseconds.
        // How long did libcurl spend on the sub-tasks associated with
        // this request??
        double t;
#define curlstat(NAME) \
        wrap_curl_easy_getinfo(curl, CURLINFO_##NAME##_TIME, &t);       \
        oss << #NAME " " << t << "\n"
        curlstat(NAMELOOKUP);
        curlstat(CONNECT);
        curlstat(PRETRANSFER);
        curlstat(STARTTRANSFER);
        curlstat(TOTAL);
#undef curlstat        
        // It has been hypothesized that curl timeouts are correlated
        // with high load average.  Let's see...
        std::ifstream ifs("/proc/loadavg");
        oss << "/proc/loadavg: " << ifs.rdbuf();
        ifs.close();
            
        return oss.str();
    }

protected:
    void recv_hdr(char *buffer, size_t size, size_t nitems){
        str_view sv(buffer, size * nitems);
        if(!endswith(sv, "\r\n"))
            throw se(EPROTO, "Header does not end with CRLF.  Somebody is very confused");
        sv = sv.substr(0, sv.size()-2); // ignore the CRLF from now on
        if( sv.size() == 0 /* nothing but CRLF */ || startswith(sv, "HTTP/") )
            return; // curl tells us about the CRLFCRLF header delimiter and the HTTP/1.1 line.  Ignore them.
        auto firstcolonpos = sv.find(':');
        if(firstcolonpos == std::string::npos)
            throw se(EPROTO, "recv_hdr:  no colon on line: " + std::string(sv));
        std::string key(sv.substr(0, firstcolonpos)); // RFC7230 doesn't allow WS between field-name and ":"
        // down-case the key.  Note that this could have trouble with non-ASCII.  Tough.
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        // skip optional whitespace (OWS).  HTTP RFCs define OWS as SP/HTAB!)
        str_view val = sv.substr(firstcolonpos+1);
        auto firstnonwhitepos = val.find_first_not_of(" \t");
        if(firstnonwhitepos == std::string::npos){
            // There's nothing but OWS and CRLF after the colon.
            // This is pretty sketchy - but we'll allow it.
            val = str_view{};
        }else{
            auto lastnonwhitepos = val.find_last_not_of(" \t");
            val = val.substr(firstnonwhitepos, (lastnonwhitepos - firstnonwhitepos)+1);
        }
        hdrmap[key] = val;
        DIAG(_http>=2, "recv_hdr: hdrmap[" << key << "] = '" <<  hdrmap[key] << "'");
        // N.B.  RFC7230, 3.2.2 says that keys whose values can be
        // treated as a comma-delimited list may appear multiple
        // times.
        //
        // We ignore that because when curl_handles_redirects is true,
        // recv_hdr is called for all the headers in the original
        // reply, and then all the headers in the first redirected
        // reply, and then all the headers ...  There's no way to
        // distinguish between a header, (e.g., Content-Length) that
        // appears in multiple replies and a header that legitimately
        // appears multiple times in the same reply (e.g., Warning).
        // Fortunately, all the headers we care about should only
        // appear once-per-reply, and we definitely want the value
        // from the last, non-redirect reply.
        //
        // Also note that when curl_handles_redirects is false, we
        // clear the hdrmap between redirects, so this is not an
        // issue.
    }

    void recv_data(char *buffer, size_t size, size_t nitems){
        content.append(buffer, size*nitems);
        DIAGf(_http, "recv_data: append %zd bytes to content", size*nitems);
    }

    time_t get_age() const try{
        auto ii = hdrmap.find("age");
        unsigned long age;
        if(ii == hdrmap.end())
            age = 0;
        else{
            age = svto<unsigned long>(ii->second);
        }
        return age;
    }catch(std::exception& e){
        std::throw_with_nested(std::runtime_error(__func__));
    }

    uint64_t get_etag64() const try{
        auto ii = hdrmap.find("etag");
        if(ii == hdrmap.end())
            return 0;
        return parse_quoted_etag(ii->second);
    }catch(std::exception & e){
        // It's not inconceivable that we're talking to a perfectly good
        // server that just happens to use ETags that we don't understand.
        // Complain once and then ignore.
        static std::atomic_flag complained = ATOMIC_FLAG_INIT;
        if(!complained.test_and_set())
            complain(LOG_WARNING, e, "get_etag64:  ETags header is unparseable as a uint64_t.  Returning 0.  This message will not be repeated");
        return 0;
    }

    time_t get_max_age() const try{
        auto ii = hdrmap.find("cache-control");
        if(ii == hdrmap.end())
            throw se(EPROTO, "No Cache-control header.  Something is wrong");
        std::string s = get_key_from_string(ii->second, "max-age=");
        // Should we be more strict here?  There are good reasons for no max-age,
        // e.g., there's a no-cache directive instead.  Just return 0.
        if(s.empty())
            return 0;
        // OTOH, if there is a max-age, throw if we can't parse it as a long.
	auto ret = svto<long>(s);
        return ret;
    }catch(std::exception& e){
        std::throw_with_nested(std::runtime_error(__func__));
    }

    time_t get_stale_while_revalidate() const try{
        auto ii = hdrmap.find("cache-control");
        if(ii == hdrmap.end())
            return 0; //throw se(EPROTO, "No Cache-control header.  Something is wrong");
        auto s = get_key_from_string(ii->second, "stale-while-revalidate=");
        // According to rfc5861, stale-while-revalidate is in replies.
        // Our job is to pass it along.  It's up to the caller what to
        // do with it.
        if(s.empty())
            return 0;
	auto ret = svto<long>(s);
        return ret;
    }catch(std::exception& e){
        std::throw_with_nested(std::runtime_error(__func__));
    }

    static std::string get_key_from_string(const std::string& s, const std::string& key) try {
        std::string::size_type pos=0;
        do{
            pos = s.find(key, pos); // N.B. find is documented to return npos if pos >= s.size()
            if(pos == std::string::npos || pos+key.size() >= s.size())
                return {};
            // take care not to match s-maxage, when we're looking for max-age!
            if( pos==0 || s[pos-1] == ',' || ::isspace(s[pos-1]) )
                break; // SUCCESS!!
            pos += 1;  // += key.size() if key has no repeating sections
        }while(true);
        pos += key.size();
        // find_first_of might return npos, which is the largest
        // possible size_t.  Subtracting pos from it is still huge, so
        // substr will still give us the whole string.
        return s.substr(pos, s.find_first_of(", ", pos)-pos);
    }catch(std::exception& e){
        std::throw_with_nested(std::runtime_error(__func__));
    }

    void gather_stats(CURL *curl){
        double t;
#define curlstat(NAME) \
        wrap_curl_easy_getinfo(curl, CURLINFO_##NAME##_TIME, &t); \
        bep->stats.curl_##NAME##_sec += 1.e9*t
        curlstat(NAMELOOKUP);
        curlstat(CONNECT);
        curlstat(PRETRANSFER);
        curlstat(STARTTRANSFER);
        curlstat(TOTAL);
#undef curlstat
        if(_transactions){
            // curl can tell us everything we need to know or we can
            // measure and count ourselves and/or retrieve stuff from
            // *this.  Let's ask curl...
            wrap_curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &t);
            double bytes_read;
            wrap_curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &bytes_read);
            const char *url = nullptr;
            wrap_curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
            // Ignore everything after the /fs123 sigil.
            std::string baseurl;
            if(url){
                str_view surl(url);
                auto len = surl.find("/fs123");
                if(len != std::string::npos)
                    len += 1;
                baseurl = surl.substr(0, len);
            }else{
                baseurl = "<unknown>";
            }
            const char *ip = "0.0.0.0";
            wrap_curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
            long code = 599;
            wrap_curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            timespec now;
            ::clock_gettime(CLOCK_REALTIME, &now);
            DIAGsend(fmt("\n%ld.%06ld GET %ld %.0f %ld %s %s",
                         long(now.tv_sec), now.tv_nsec/1000,
                        code,
                        bytes_read,
                        (long)(t*1.e6),
                        ip,
                        baseurl.c_str()));
        }
    }

};

std::ostream& backend123_http::report_stats(std::ostream& os){
    return os << stats
              << "aicache_hits: " << aicache.hit_count() << "\n"
              << "aicache_misses: " << aicache.miss_count() << "\n"
              << "aicache_refreshes: " << aicache.refresh_count() << "\n"
              << "aicache_agains: " << aicache.eai_again_count() << "\n"
              << "aicache_size: " << aicache.size() << "\n"
        ;
}

int sockoptcallback(void */*clientp*/, curl_socket_t curlfd, curlsocktype /*purpose*/){
    // Setting SO_RCVBUF to 24k seems to work well on the DESRES intranet
    // in 2017, largely mitigating the effects of TCP Incast Collapse
    // from 10Gig servers to 1Gig clients, while not doing significant
    // harm to overall throughput.  But in other times at other
    // places, YMMV.
    //
    // Note that setting the option explicitly to "0" means:  don't
    // call setsockopt(..., SO_RCVBUF, ...)  at all, in which case
    // the connection will use system defaults.  On linux, the
    // default is in /proc/sys/net/core/rmem_default.  On one of
    // our Cent7 machines in 2017, it's 212992=208k, which does
    // result in TCP Incast Collapse on our network.
    int len = envto<int>("Fs123SO_RCVBUF", 24*1024); // 0 means leave system default in place.
    if(len)
        sew::setsockopt(curlfd, SOL_SOCKET, SO_RCVBUF, &len, sizeof(len));
    return 0;
}

void backend123_http::regular_maintenance(){
    if(vols.namecache){
        DIAG(_namecache, "aicache.refresh()\n");
        aicache.refresh();
        try{
            // this shouldn't be necessary...  A bug was fixed in
            // core123/0.27.1, but since there's no unit test, we're
            // paranoid that other bugs may be lurking.  In 0.27.2, we
            // added a self-test to the aicache.  It locks the aicache
            // and blocks lookups in other threads while it runs, so
            // it's undesirable in production.  But since production
            // servers typically have only two or three names in their
            // cache, the navel-gazing should only take a couple of
            // microseconds.
            aicache._check_invariant();
        }catch(std::exception& e){
            complain(LOG_ERR, e, "addrinfo_cache::check_invariant detected internal inconsistencies.  Turning off the namecache for now.  Consider attaching a debugger and turning the namecache back on to investigate!");
            vols.namecache.store(false);
        }
    }
}

void backend123_http::setoptions(CURL *curl) const{
    // Forcing HTTP/1.1 tells libcurl not to close connections even though
    // the server (squid-3.1) says it's HTTP/1.0.  It would probably break
    // badly if the server were truly incapable of any 1.1 features (like
    // keepalive).  Let's take our chances for now.  We can make it an
    // option later, if necessary.
    wrap_curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    // If the _http diagnostic level is >= 2, then direct curl's
    // debug stream to our diagnostic stream:
    if(_http >= 2){
        wrap_curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
        wrap_curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_to_diag_stream);
    }

    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, sockoptcallback);
    // libcurl's SIGALRM handler is buggy.  We've definitely seen the
    // *** longjmp causes uninitialized stack frame *** bugs that it
    // causes.  See
    // http://stackoverflow.com/questions/9191668/error-longjmp-causes-uninitialized-stack-frame
    // for possible workarounds, including this one (CURLOPT_NOSIGNAL):
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
    // Tell curl to start by asking for content_reserve_size bytes.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, content_reserve_size);
    if(!netrcfile.empty()){
        // Too slow??  This reparses the netrc file for every request.
        // How much overhead is that??  open/close plus some
        // fgets/strtoks and general string fiddling.  Is it significant
        // compared to an HTTP GET?
        //
        // An alternative would be to parse the netrc file once and
        // for all and then call curl_easysetopt(CURLOPT_USERNAME,
        // ...) and curl_easy_setopt(CURLOPT_PASSWORD, ...).  Maybe
        // call Curl_parsenetrc()?  Note that the user and pwd args
        // are assumed to have been strdup'ed.
        //
        // Another alternative is to invent our own config file
        // with just a username and password, and parse that once and
        // for all.  It doesn't have to be as "fancy" as netrc.  This
        // would also sidestep the issue with the namecache.
        curl_easy_setopt(curl, CURLOPT_NETRC_FILE, netrcfile.c_str());
        curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
    }

    long cto = vols.connect_timeout.load();
    long tto = vols.transfer_timeout.load();
    // If the load_timeout_factor config option is greater than zero
    // (i.e., enabled), and the current load-average per-cpu is
    // greater than the load_timeout_factor, then multiply the connect
    // and transfer timeouts by the ratio (greater than one).  E.g.,
    // if we're running on 6 cores, with load_timeout_factor=3 and a
    // load-average of 30, the load-average per core is 30/6=5, so we
    // increase the timeouts by 5/3.
    float ltf = vols.load_timeout_factor.load();
    if((cto || tto) && ltf > 0.){
        auto la_per_cpu = vols.load_average.load()/vols.hw_concurrency;
        if(la_per_cpu > ltf){
            float load_factor = la_per_cpu/ltf;
            cto *= load_factor;
            tto *= load_factor;
        }
    }

    if(cto)         // see comment in ctor.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cto);
    if(tto)        // see comment in ctor.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, tto);
    if(vols.curl_handles_redirects.load()){
        auto maxredirs = vols.http_maxredirects.load();
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, !!maxredirs);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, maxredirs);
    }

    if(using_https){
        // Command line options to set '--insecure'??
        // N.B.  These comments are verbatim from http://curl.haxx.se/libcurl/c/https.html
        if(!envto<std::string>("Fs123SSLNoVerifyPeer", "").empty()){
            /*
             * If you want to connect to a site who isn't using a certificate that is
             * signed by one of the certs in the CA bundle you have, you can skip the
             * verification of the server's certificate. This makes the connection
             * A LOT LESS SECURE.
             *
             * If you have a CA cert for the server stored someplace else than in the
             * default bundle, then the CURLOPT_CAPATH option might come handy for
             * you.
             */ 
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        }

        if(!envto<std::string>("Fs123SSLNoVerifyHost", "").empty()){
            /*
             * If the site you're connecting to uses a different host name that what
             * they have mentioned in their server certificate's commonName (or
             * subjectAltName) fields, libcurl will refuse to connect. You can skip
             * this check, but this will make the connection less secure.
             */ 
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
    }
}

backend123_http::backend123_http(const std::string& _baseurl, const std::string& _accept_encoding, volatiles_t& _vols)
    : backend123(),
      baseurls{}, content_reserve_size(129 * 1024), // 129k leaves room for the 'validator' in a 128k request
      using_https(startswith(_baseurl, "https://")),
      accept_encoding(_accept_encoding),
      vols(_vols)
{
    baseurls.emplace_back(_baseurl);
    netrcfile = envto<std::string>("Fs123NetrcFile", "");
#ifdef NO_NETRC
    if(!netrcfile.empty()){
        complain(LOG_ERR, "http backend compiled with -DNO_NETRC:  Fs123NetrcFile option ignored!");
        netrcfile = "";
    }
#endif

    // libcurl defaults to a 300 sec connection timeout.  That's
    // extremely painful when the server is down.  Unfortunately,
    // there are no sub-second settings and a zero value is
    // interpreted as a request to restore the default (300).
    // When we had it set to 1s, we saw occasional timeouts
    // even on a LAN.
    if(vols.connect_timeout.load() == 0){
        complain(LOG_WARNING, "Fs123ConnectTimeout was specified as 0.  Timeout set to the shortest allowed value (1 sec) instead.");
        vols.connect_timeout = 1;
    }
    // The transfer_timeout defaults to 40 seconds.  The typical
    // 'Fs123Chunk' is 128kByte or 1Mbit.  So 5 seconds should be
    // plenty of time over wired or wireless connections with enough
    // slack to tolerate a dropped packet or two.  But clients with
    // lossy connections (wireless?) might want a higher value.  FWIW,
    // we saw some timeouts on our 'guest' wireless network when the
    // timeout was 2 seconds.  The timeout can be adjusted with an
    // ioctl.
    if(vols.transfer_timeout.load() ==0 ){
        complain(LOG_WARNING, "libcurl CURLOPT_TIMEOUT will be zero, which can cause libcurl to hang indefinitely.  If too many fuse callbacks hang, all fuse activity (not just this particular fs123) may become unresponsive");
    }
}

bool
backend123_http::refresh(const req123& req, reply123* replyp) try{
    if(!req.no_cache && replyp->fresh()){
        DIAGfkey(_http, "backend123_http::refresh:  short-circuit, no_cache: %d, fresh: %d\n",
                 req.no_cache, replyp->fresh());
        return false;
    }
    // replyp is stale (or invalid).  I.e., it may have content, but
    // if it does, it's older than than max-age, and hence must be
    // revalidated.
    stats.backend_gets++;
    if(vols.disconnected){
        stats.backend_disconnected++;
        throw se(EIO, "backend123_http::refresh:  disconnected");
    }
    atomic_scoped_nanotimer _t(&stats.backend_get_sec);
    auto curl = get_curl();
    // get_curl gives us a CURL* that has been curl_easy_init'ed or curl_easy_reset.
    // We have to call curl_easy_setopt to establish our own policies and defaults.
    setoptions(curl);
    curl_handler ch(this);
    wrap_curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_handler::header_callback);
    wrap_curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)&ch);
    wrap_curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_handler::write_callback);
    wrap_curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&ch);
    DIAGfkey(_http, "backend123_http::refresh: GET %s\n", req.urlstem.c_str());
    if(replyp->valid() && !req.no_cache && replyp->etag64){
        DIAG(_http, "If-None-Match: " << replyp->etag64);
        ch.headers.push_back("If-None-Match: \"" + std::to_string(replyp->etag64) + "\"");
        stats.backend_INM++;
        _t.set_accumulator(&stats.backend_INM_sec);
    }else{
        DIAG(_http, "no If-None-Match: valid=" << replyp->valid() << " req.no_cache=" << req.no_cache << " replyp->etag64 " << replyp->etag64);
    }
    
    if(!accept_encoding.empty())
        ch.headers.push_back("Accept-encoding: " + accept_encoding);

    std::vector<std::string> cache_control;
    if(req.stale_if_error){
        cache_control.push_back("stale-if-error=" + std::to_string(req.stale_if_error));
    }
    if(req.no_cache){
        // no-cache or max-age=0?  Rfc 7234 uses "MUST NOT" in the
        // description of "no-cache", but the slightly milder "not
        // willing to accept" for max-age.  What any given forward or
        // reverse proxy will do is surely subject to the proxy's own
        // configuration options.  Varnish generally has the attitude
        // that it's a bona fide origin server and is not obliged to
        // slavishly follow the demands of mere clients.  At DESRES,
        // we've configured our vcl to respect no-cache, but we're
        // oblivious to max-age=0 (though it wouldn't be hard to fix),
        // so we'll stick  with no-cache for now.
        cache_control.push_back("no-cache");
    }
    if(req.max_stale >= 0){
        cache_control.push_back("max-stale=" + std::to_string(req.max_stale));
    }
    
    if(!cache_control.empty()){
        std::ostringstream oss("Cache-control: ", std::ios::ate);
        int i=0;
        for(const auto& a : cache_control){
            oss << ((i++)?",":"") << a;
        }
        ch.headers.push_back(oss.str());
    }
    
    ch.headers.push_back("User-Agent: fs123p7/" GIT_DESCRIPTION);
    
    bool ret = ch.perform_with_fallback(curl, req.urlstem, replyp);
    if(replyp->content.size() > content_reserve_size){
        // reserve 10% more than the largest reply so far, up to 8M
        content_reserve_size = std::min(size_t(1.1*replyp->content.size()), size_t(8192*1024));
    }
    release_curl(std::move(curl));
    DIAGfkey(_http, "backend123_http::refresh: reply.eno=%d reply.content.size(): %zd\n", replyp->eno, replyp->content.size());
    return ret;
 }catch(std::exception& e){
    std::throw_with_nested( std::runtime_error(fmt("backend123_http::get(\"%s\")", req.urlstem.c_str())));
 }


