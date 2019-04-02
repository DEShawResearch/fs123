#include "backend123.hpp"
#include "volatiles.hpp"
#include <core123/strutils.hpp>
#include <core123/countedobj.hpp>
#include <core123/addrinfo_cache.hpp>
#include <curl/curl.h>
#include <memory>

struct url_info{
    // Extracting the hostname from a url, and remembering the
    // pieces so it can be reassembled later with a different hostname
    // requires tricky regexes that we'd rather apply once and for all.
    url_info(const std::string& url);
    const std::string original;
    std::string before_hostname;
    std::string hostname;
    std::string after_hostname;
    bool do_not_lookup; // the hostname is already dotted-decimal or our code can't find a hostname
    // This is also a convenient place to keep track of whether
    // we're "deferring" use of this url until some future time...
    using tp_type = std::chrono::system_clock::time_point;
    // unique_ptr because a bare std::atomic is not move-constructible and
    // therefore can't be emplace'ed into a vector.
    std::unique_ptr<std::atomic<tp_type>> deferred_until;
};

struct backend123_http : public backend123 {
    backend123_http(const std::string& _baseurl, size_t _content_reserve_size, const std::string& _accept_encodings, bool _disconnected, volatiles_t& volatiles);
    void add_fallback_baseurl(const std::string& s){
        baseurls.emplace_back(s);
    }
    virtual ~backend123_http();

    // refresh MUST provide a "strong" exception guarantee.  I.e., if
    // it throws, it may not corrupt *reply123.
    bool refresh(const req123& req, reply123*) override;

    std::ostream& report_stats(std::ostream&) override;
    struct curl_handler;

    void regular_maintenance();

private:
    std::vector<url_info> baseurls;
    void setoptions(CURL* curl) const;
    std::string stale_if_error;
    std::string netrcfile;
    size_t content_reserve_size;
    bool using_https;
    std::string accept_encoding;
    volatiles_t& vols;
    core123::addrinfo_cache aicache;
};

// libcurl_category(): a singleton std::error_category() for libcurl
// errors.
const std::error_category& libcurl_category() noexcept;

struct libcurl_category_t : public std::error_category{
    virtual const char *name() const noexcept { return "libcurl"; }
    //virtual std::error_condition default_error_condition(int ev) const {}
    //virtual bool equivalent(ocnst std::error_code& code, int condition) const{}

    virtual std::string message(int ev) const{
        static std::mutex err_mtx; // curl_easy_strerror isn't reentrant, so we lock_guard it
        std::lock_guard<std::mutex> lg(err_mtx);
        auto curle = curlcode(ev);
        auto eno = os_errno(ev);
        return core123::str(curl_easy_strerror(curle), " with os_errno:", eno);
    }
    // The system_error API forces us to pack everything we want to
    // communicate about the libcurl_category() into a single integral
    // 'ev'.  We assert that the CURLcode fits into 16 bits, and gamble
    // (there are no guarantees!) that the os_errno fits into the
    // higher bits.
    static_assert(CURL_LAST <= 0x100 && CURL_LAST>0, "CURLE must fit in 8 bits");
    //
    // The following static convenience functions allow callers to
    // manufacture system errors in the libcurl category and to pick
    // apart their se->code().value()s.
    //
    // This may not be the "right" place for such things, but it does
    // nicely localize all the implementation details.
    static CURLcode
    curlcode(int ev){
        return static_cast<CURLcode>(ev&0xff);
    }
    static int
    os_errno(int ev){
        return ((unsigned)ev)>>8;
    }
    static std::system_error
    make_libcurl_error(CURLcode cc, int os_errno, const std::string& what){
        int ev = (os_errno << 8) | (cc&0xff);
        return std::system_error(ev, libcurl_category(), what);
    }
};
