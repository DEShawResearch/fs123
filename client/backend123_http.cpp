#include "backend123_http.hpp"
#include "opensslthreadlock.h"
#include "fs123/httpheaders.hpp"
#include "fs123/content_codec.hpp"
#include "fs123/acfd.hpp"
#include <core123/complaints.hpp>
#include <core123/countedobj.hpp>
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
#include <core123/stats.hpp>
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

static auto _backend = diag_name("backend");
static auto _namecache = diag_name("namecache");

namespace{
#define STATS_STRUCT_TYPENAME backend123_http_statistics_t
#define STATS_INCLUDE_FILENAME "backend123_http_statistic_names"
#include <core123/stats_struct_builder>

backend123_http_statistics_t stats;

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

void wrap_curl_easy_getinfo(CURL* curl, CURLINFO option, long *lp){
    auto ret = curl_easy_getinfo(curl, option, lp);
    if(ret != CURLE_OK)
        libcurl_throw(ret, fmt("curl_easy_getinfo(%p, %d, %p)", curl, option, lp));
}

void wrap_curl_easy_getinfo(CURL* curl, CURLINFO option, double *dp){
    auto ret = curl_easy_getinfo(curl, option, dp);
    if(ret != CURLE_OK)
        libcurl_throw(ret, fmt("curl_easy_getinfo(%p, %d, %p)", curl, option, dp));
}

struct wrapped_curl_slist{
    // construct an empty slist
    wrapped_curl_slist() : chunk{nullptr} {}
    // construct an slist from a range of std::strings:
    template<typename ITER>
    wrapped_curl_slist(ITER b, ITER e) : wrapped_curl_slist() {
        while(b!=e)
            append(*b++);
    }
    // The copy-constructor and assignment operators are asking
    // for trouble.  Delete them until/unless we need them.
    wrapped_curl_slist(const wrapped_curl_slist& rhs) = delete;
    wrapped_curl_slist(wrapped_curl_slist&&) = delete;
    wrapped_curl_slist& operator=(const wrapped_curl_slist&) = delete;
    wrapped_curl_slist& operator=(wrapped_curl_slist&&) = delete;
    
    curl_slist* get(){
        return chunk;
    }

    void append(const std::string& s){
        append(s.c_str());
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
        if(chunk)
            curl_slist_free_all(chunk);
    }
private:
    curl_slist* chunk;
};

// Let's build some RAII autoclosing magic for CURL*.
struct CURLcloser{
    void operator()(CURL *c){ ::curl_easy_cleanup(c); stats.curl_cleanups++; }
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
        stats.curl_reuses++;
        return std::move(tl_curl);
    }
    stats.curl_inits++;
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
        stats.curl_inits++;
        ret.reset(curl_easy_init());
    }else{
        stats.curl_reuses++;
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
refcounted_scoped_nanotimer_ctrl refcountedtimerctrl(stats.backend_curl_perform_inuse_sec);

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

struct backend123_http::curl_handler : private countedobj<backend123_http::curl_handler, decltype(stats.backend_active_handlers)>{
    // Callbacks *CAN NOT* be non-static class members.  So we provide
    // static class members and arrange that 'this' is passed through
    // the userdata argument.
    static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata){
        curl_handler* ch = (curl_handler *)userdata;
        atomic_scoped_nanotimer _t(&stats.backend_header_callback_sec);
        try{
            ch->recv_hdr(buffer, size, nitems);
            stats.backend_header_bytes_rcvd += size * nitems;
            return size*nitems;
        }catch(...){
            // We don't want exceptions thrown "over" the libcurl C 'perform' function.
            ch->exptr = std::current_exception();
            return 0;
        }
    }

    static size_t write_callback(char *buffer, size_t size, size_t nitems, void *userdata){
        atomic_scoped_nanotimer _t(&stats.backend_write_callback_sec);
        curl_handler* ch = (curl_handler *)userdata;
        try{
            ch->recv_data(buffer, size, nitems);
            stats.backend_body_bytes_rcvd += size * nitems;
            return size * nitems;
        }catch(...){
            // We don't want exceptions thrown "over" the libcurl C 'perform' function.
            ch->exptr = std::current_exception();
            return 0;
        }
    }

    curl_handler(backend123_http* bep_, const std::string& urlstem_) :
        bep(bep_), urlstem(urlstem_), exptr{}, content{}, hdrmap{}
    {
        if(bep->content_reserve_size>0)
            content.reserve(bep->content_reserve_size);
    }

    backend123_http* bep;
    std::string urlstem;
    std::exception_ptr exptr;
    std::string content;
    // Note that the keys in hdrmap are all lower-case, e.g.,
    // "cache-control", "age", "fs123-errno".  Regardless
    // of how they were spelled by the origin server or proxies.
    std::map<std::string, std::string> hdrmap;
    long http_code;
    char curl_errbuf[CURL_ERROR_SIZE]; // 256
    std::vector<std::string> headers;

    void reset(){
        content.clear();
        hdrmap.clear();
        exptr = nullptr;
    }

    bool perform_without_fallback(CURL *curl, const url_info& urli, reply123* replyp){
        // The curl_slist API doesn't support deletion or replacement.
        // Since we can't replace the Host header, we create a new
        // single-use curl_slist with the common headers and
        // conditionally append a Host header to it.  It must stay
        // in-scope until curl_easy_perform is done.
        wrapped_curl_slist headers_sl(headers.begin(), headers.end());

        // N.B. we wouldn't have to fiddle with the Host header with a
        // newer libcurl: Since 7.49.0 libcurl has had
        // CURLOPT_CONNECT_TO, which looks like the "right" way to
        // tell libcurl to connect to an IP address of our choosing.
        // CURLOPT_RESOLVE, introduced in 7.21.3 (memory leak fixed in
        // 7.35.0), would be almost as good.  Unfortunately, CentOS7
        // ships with libcurl-7.29.0, and CentOS6 ships with
        // libcurl-7.19.7.  So for backward compatibility, we modify
        // the URL and add a Host: header.  This isn't ideal for
        // a couple of reasons:
        // - Headers are associated with the CURL* handle, and hence
        //   are re-used when following redirects (c.f.
        //   CURL_FOLLOW_REDIRECTION).  Sending the original Host:
        //   header to the redirected-to server is definitely
        //   surprising, but might be ok, depending on how the
        //   redirected-to server is configured.
        // - If CURLOPT_NETRC is in effect, curl will look up
        //   the IP address rather than a hostname in the netrc file.
        // - Curl might use the hostname in other ways.
        std::string burl = urli.original;
        if(bep->vols.namecache && !urli.do_not_lookup){
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_socktype = SOCK_STREAM;
            auto ai_result = bep->aicache.lookup(urli.hostname, {}, &hints);
            if(ai_result->status != 0)
                complain(LOG_WARNING, str("addrinfo_cache.lookup(", urli.hostname, ") returned ", ai_result->status));
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
                    burl = urli.before_hostname + dotted_decimal + urli.after_hostname;
                    headers_sl.append("Host:"+urli.hostname);
                    DIAG(_namecache, "replacing " << urli.original << " with " << burl << " and Host:" + urli.hostname + " header\n");
                }else{
                    complain(LOG_ERR, "inet_ntop failed.  There's an address in the addrinfo_cache but we can't write it in dotted decimal.  How can that happen?");
                }
            }
        }
        wrap_curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_sl.get());
        wrap_curl_easy_setopt(curl, CURLOPT_URL, (void*)(burl + urlstem).c_str());
        return perform_once(curl, replyp);
    }

    // perform_with_fallback is a method of curl_handler so we can
    // keep track of the exception_ptr and so that we can report
    // progress along with any errors.  perform_with_fallback calls
    // perform_without_fallback, which calls perform_once with the
    // first viable 'baseurl', i.e., the first one whose
    // 'deferred_until' time_point is in the past.  It is assumed that
    // retry-looping takes place at a higher level.
    bool perform_with_fallback(CURL* curl, reply123* replyp){
        // If there's only one baseurl, i.e., no fallbacks, then go
        // straight to perform_without_fallback.  Skip the rigamarole
        // of checking and updating the list of deferrals,
        // complaining, etc.
        if(bep->baseurls.size() < 2)
            return perform_without_fallback(curl, bep->baseurls.at(0), replyp);
        
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
            return perform_without_fallback(curl, bep->baseurls.at(i), replyp);
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
    bool perform_once(CURL* curl, reply123* replyp){
        stats.curl_performs++;
        atomic_scoped_nanotimer _t(&stats.backend_curl_perform_sec);
        refcounted_scoped_nanotimer _rt(refcountedtimerctrl);
        wrap_curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf);
        curl_errbuf[0] = '\0';
        auto ret = curl_easy_perform(curl);
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
            stats.backend_got_nothing++;
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
        return getreply(replyp);
    }

    bool getreply(reply123* replyp) {
	DIAGkey(_backend, "got status " << http_code << "\n");
	if (_backend >= 2) {
	    for (const auto h : hdrmap)
		DIAG(true, "header \"" << h.first << "\" : \"" << h.second << "\"" << (endswith(h.second, "\n")? ""  : "\n"));
            DIAG(true, "content (size=" << content.size() << ") \"\"\"" << quopri({content.data(), content.size()}) << "\"\"\"\n");
	}
        auto age = get_age();
        auto max_age = get_max_age();
        auto swr = get_stale_while_revalidate();
        if( http_code == 304 ){
            if(max_age == 0){
                // See comment in get_max_age().
                max_age = std::chrono::duration_cast<std::chrono::seconds>(replyp->expires - replyp->last_refresh).count();
		if( age > max_age ){
		    // the max_age probably changed.  But we'll never
		    // learn what it is if we keep making INM
		    // requests.  So we have to zero out the
		    // etag64 so that the next time we
		    // request it, we get a 200.
		    complain(LOG_WARNING,  "age > max_age - set etag64=0  and hope it clears"); // don't know the url here!?
		    replyp->etag64 = 0;
		}
            }
            replyp->last_refresh = clk123_t::now() - std::chrono::seconds(age);
            replyp->expires = replyp->last_refresh + std::chrono::seconds(max_age);
            replyp->stale_while_revalidate = std::chrono::seconds(swr);
            stats.backend_304++;
            stats.backend_304_bytes_saved += replyp->content.size();
            DIAGkey(_backend, "getreply 304 setting age: " <<  age
                    << ", max_age=" << max_age
                    << ", last_refresh=" << tp2dbl(replyp->last_refresh)
                    << ", expires=" << tp2dbl(replyp->expires)
                    << ", swr=" <<  str(replyp->stale_while_revalidate)
                    << "\n");
            return false; 
        }
        if( http_code != 200 ) {
            // content probably has newlines, and might even contain html, which might be annoying if we
            // ship it to syslog, but discarding it seems wrong.  Maybe the http_error object should
            // have a 'content' separate from a 'message'?
            httpthrow(http_code, content);
	}

        auto ii = hdrmap.find(HHERRNO);
        if(ii == hdrmap.end())
            throw se(EINVAL, "No key matching " HHERRNO " in header, need errno");
        auto eno = svto<int>(ii->second);
        DIAGkey(_backend, "errno " HHERRNO ": " << eno << "\n");
        auto et64 = get_etag64();
        std::string content_encoding;
        ii = hdrmap.find("content-encoding");
        if(ii != hdrmap.end())
            content_encoding = ii->second;
        else
            content_encoding = "";
        auto ce = content_codec::encoding_stoi(content_encoding);
        DIAGkey(_backend,  "content_encoding: " << content_encoding << "\n");
        ii = hdrmap.find(HHCOOKIE);
        uint64_t estale_cookie = (ii == hdrmap.end()) ? 0 : svto<uint64_t>(ii->second);
        DIAGkey(_backend, "estale_cookie: " << estale_cookie << "\n");
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
            DIAGkey(_backend, HHNO ": " + ii->second);
            // ii->second is one of:
            // 1-  whitespace* NUMBER whitespace*  
            // 2-  whitespace* NUMBER whitespace* "EOF" whitespace*
            // 3-  whitespace* NUMBER <anything else> !ERROR
            // In all cases, the NUMBER is replyp->chunk_next_offset
            // replyp->last_chunk depends on which case.
            size_t pos = svscan<int64_t>(ii->second, &replyp->chunk_next_offset, 0);
            const char *p = ii->second.data() + pos;
            DIAGfkey(_backend, HHNO ": chunk_next_offset=%jd, ii->second=%s, pos=%zd\n",
                     (intmax_t)replyp->chunk_next_offset, ii->second.c_str(), pos);
            while( ::isspace(*p) )
                ++p;
            if( *p == '\0')
                replyp->chunk_next_meta = reply123::CNO_NOT_EOF;
            else if(::strncmp(p, "EOF", 3)==0)
                replyp->chunk_next_meta = reply123::CNO_EOF;
            else
                throw se(EPROTO, "Unrecognized words in " HHNO " header:" + ii->second);
        }else{
            DIAGfkey(_backend, "No " HHNO "\n");
            replyp->chunk_next_meta = reply123::CNO_MISSING;
        }
        return true;
    }
        
    std::string verbose_complaint(CURL *curl) const{
        std::ostringstream oss;
        oss<< "Headers:\n";
        for(const auto p : hdrmap)
            oss << p.first << ": " << p.second; // p.second ends with crlf
        oss << "Received " << content.size() << " bytes of data\n";
        // What else can we report that might help to diagnose curl
        // errors?  Are we under heavy load??  The number of active
        // handlers is an indicator:
        oss << "active handlers: " << stats.backend_active_handlers<< "\n";
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
        std::string s(buffer, size * nitems);
        std::string ignore;
        if( s[0] == '\n' || s[0] == '\r' || startswith(s, "HTTP/") )
            return; // curl tells us about the CRLFCRLF header delimiter and the HTTP/1.1 line!
        auto firstcolonpos = s.find(':');
        if(firstcolonpos == std::string::npos)
            throw se(EPROTO, fmt("recv_hdr:  no colon on line: %s", s.c_str()));
        auto key = s.substr(0, firstcolonpos);
        // down-case the key.  Note that this could have trouble with non-ASCII.  Tough.
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        auto val = s.substr(firstcolonpos+1);
        // += here means that headers that appear multiple times,
        // e.g., "Warning", are recorded as multiline values in hdrmap
        // with CRLF separators.
        hdrmap[key] += val;
    }

    void recv_data(char *buffer, size_t size, size_t nitems){
        content.append(buffer, size*nitems);
    }

    time_t get_age() const try{
        auto ii = hdrmap.find("age");
        unsigned long age;
        if(ii == hdrmap.end())
            age = 0;
        else{
            age = svto<unsigned long>(ii->second);
        }
	DIAGkey(_backend, "get_age returning " << age << "\n");
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
        // First, find the Cache-control header:
        // RFC7232 says that 304s MUST contain cache-control
        // if the 200 would have contained it.  But squid 3.3
        // doesn't play by those rules.  So we return 0 rather
        // than throwing an error, and have logic at the call
        // site to try to work around the defeciency.
        auto ii = hdrmap.find("cache-control");
        if(ii == hdrmap.end())
            return 0; //throw se(EPROTO, "No Cache-control header.  Something is wrong");
        DIAGfkey(_backend,  "get_max_age:  cache-control: %s\n", ii->second.c_str());
        std::string s = get_key_from_string(ii->second, "max-age=");
        // Should we be more strict here?  There are good reasons for no max-age,
        // e.g., there's a no-cache directive instead.  Just return 0.
        if(s.empty())
            return 0;
        // OTOH, if there is a max-age, throw if we can't parse it as a long.
	auto ret = svto<long>(s);
	DIAGkey(_backend, "get_max_age ret=" << ret << " from \"" << s << "\"\n");
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
	DIAGkey(_backend, "get_swr ret=" << ret << " from \"" << rstrip(s) << "\"\n");
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
        stats.curl_##NAME##_sec += 1.e9*t
        curlstat(NAMELOOKUP);
        curlstat(CONNECT);
        curlstat(PRETRANSFER);
        curlstat(STARTTRANSFER);
        curlstat(TOTAL);
#undef curlstat
    }

};
countedobjDECLARATION(backend123_http::curl_handler, stats.backend_active_handlers);

std::ostream& backend123_http::report_stats(std::ostream& os){
    return os << stats
              << "getaddrinfo_again: " << aicache.eai_again_count() << "\n";
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
    // FIXME - curl's verbosity can be very informative.  Can  we figure out
    // how to divert it somewhere useful to us other than uncommenting this
    // line, recompiling and running -f (oreground).
    //wrap_curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, sockoptcallback);
    // libcurl's SIGALRM handler is buggy.  We've definitely seen the
    // *** longjmp causes uninitialized stack frame *** bugs that it
    // causes.  See
    // http://stackoverflow.com/questions/9191668/error-longjmp-causes-uninitialized-stack-frame
    // for possible workarounds, including this one (CURLOPT_NOSIGNAL):
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
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
    auto maxredirs = vols.curl_maxredirs.load();
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, !!maxredirs);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, maxredirs);

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

backend123_http::backend123_http(const std::string& _baseurl,  size_t _content_reserve_size, const std::string& _accept_encoding, bool _disconnected, volatiles_t& _vols)
    : backend123(_disconnected),
      baseurls{}, content_reserve_size(_content_reserve_size),
      using_https(startswith(_baseurl, "https://")),
      accept_encoding(_accept_encoding),
      vols(_vols)
{
    baseurls.emplace_back(_baseurl);
    int flags = 0;
    if(using_https)
        flags |= CURL_GLOBAL_SSL;
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
    curl_global_init(flags); // would a static initializer be a better location?
    thread_setup();
}

backend123_http::~backend123_http(){
    thread_cleanup();
    curl_global_cleanup();
}

bool
backend123_http::refresh(const req123& req, reply123* replyp) try{
    if(!req.no_cache && replyp->fresh()){
        DIAGfkey(_backend, "backend123_http::refresh:  short-circuit, no_cache: %d, fresh: %d\n",
                 req.no_cache, replyp->fresh());
        return false;
    }
    // replyp is stale (or invalid).  I.e., it may have content, but
    // if it does, it's older than than max-age, and hence must be
    // revalidated.
    stats.backend_gets++;
    if(disconnected_){
        stats.backend_disconnected++;
        throw se(EIO, "backend123_http::refresh:  disconnected");
    }
    atomic_scoped_nanotimer _t(&stats.backend_get_sec);
    auto curl = get_curl();
    // get_curl gives us a CURL* that has been curl_easy_init'ed or curl_easy_reset.
    // We have to call curl_easy_setopt to establish our own policies and defaults.
    setoptions(curl);
    curl_handler ch(this, req.urlstem);
    wrap_curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_handler::header_callback);
    wrap_curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)&ch);
    wrap_curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_handler::write_callback);
    wrap_curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&ch);
    DIAGfkey(_backend, "GET %s\n", req.urlstem.c_str());
    if(replyp->valid() && !req.no_cache && replyp->etag64){
        ch.headers.push_back("If-None-Match: \"" + std::to_string(replyp->etag64) + "\"");
        DIAGkey(_backend, "INM: " <<  replyp->etag64 << "\n");
        stats.backend_INM++;
        _t.set_accumulator(&stats.backend_INM_sec);
    }
    
    DIAGkey(_backend, "accept_encoding: " << accept_encoding << "\n");
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
        DIAGfkey(_backend, "%s\n", oss.str().c_str());
        ch.headers.push_back(oss.str());
    }
    bool ret = ch.perform_with_fallback(curl, replyp);
    release_curl(std::move(curl));
    DIAGfkey(_backend, "elapsed: %llu\n", _t.elapsed());
    return ret;
 }catch(std::exception& e){
    std::throw_with_nested( std::runtime_error(fmt("backend123_http::get(\"%s\")", req.urlstem.c_str())));
 }


