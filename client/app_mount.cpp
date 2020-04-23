// This is mount.fs123 - see docs/Fs123Overview for more info
//
// A fuse filesystem that allows only the following access operations:
//   lookup
//   forget
//   getattr
//   readlink
//   opendir
//   readdir
//   releasedir
//   open
//   read
//   release
//   listxattr
//   getxattr

#ifndef FUSE_USE_VERSION
#error FUSE_USE_VERSION should have been -Defined in the command line
#endif

#define HAS_FORGET_MULTI  (FUSE_VERSION >= 29)

#include "app_mount.hpp"
#include "backend123.hpp"
#include "backend123_http.hpp"
#include "diskcache.hpp"
#include "distrib_cache_backend.hpp"
#include "inomap.hpp"
#include "special_ino.hpp"
#include "fuseful.hpp"
#include "openfilemap.hpp"
#include "fs123/fs123_ioctl.hpp"
#include "fs123/stat_serializev3.hpp"
#include "fs123/httpheaders.hpp"
#include "fs123/sharedkeydir.hpp"
#include "fs123/content_codec.hpp"
#include "fs123/acfd.hpp"
#include <core123/complaints.hpp>
#include <core123/stats.hpp>
#include <core123/intutils.hpp>
#include <core123/base64.hpp>
#include <core123/diag.hpp>
#include <core123/expiring.hpp>
#include <core123/scoped_nanotimer.hpp>
#include <core123/svto.hpp>
#include <core123/exnest.hpp>
#include <core123/http_error_category.hpp>
#include <core123/syslog_number.hpp>
#include <core123/sew.hpp>
#include <core123/threeroe.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/envto.hpp>
#include <core123/netstring.hpp>
#include <core123/strutils.hpp>
#include <core123/pathutils.hpp>
#include <core123/periodic.hpp>
#include <fuse/fuse_lowlevel.h>

#include <unordered_map>
#include <thread>
#include <exception>
#include <memory>
#include <mutex>
#include <atomic>
#include <array>
#include <fstream>

#include <sys/stat.h>
#include <cstddef>
#include <sys/xattr.h>
#if __has_include(<sys/sysinfo.h>) // Linux
#include <sys/sysinfo.h>
#elif __has_include(<sys/sysctl.h>) // BSD and MacOS
#include <sys/sysctl.h>
#endif
#if __has_include(<mcheck.h>) // GLIBC
#include <mcheck.h>
#endif
#if __has_include(<malloc.h>) // GLIBC
#include <malloc.h>
#endif
#ifndef ENOATTR
# define ENOATTR ENODATA
#endif

// #include <google/heap-profiler.h>

extern char **fs123p7_argv;
extern int fs123p7_argc;

using namespace core123;

namespace {
#define STATS_INCLUDE_FILENAME "fs123_statistic_names"
#define STATS_STRUCT_TYPENAME fs123_stats_t
#include <core123/stats_struct_builder>
fs123_stats_t stats;
atomic_scoped_nanotimer elapsed_asnt; // measures time since program initialization.

auto _init = diag_name("init");
auto _lookup = diag_name("lookup");
auto _getattr = diag_name("getattr");
//auto _readlink = diag_name("readlink");
auto _readdir = diag_name("readdir");
auto _read = diag_name("read");
auto _open = diag_name("open"); // also release
auto _opendir = diag_name("opendir"); // also releasedir
auto _ioctl = diag_name("ioctl");
auto _special = diag_name("special");
auto _estale = diag_name("estale");
auto _err = diag_name("err");
auto _xattr = diag_name("xattr");
auto _secretbox = diag_name("secretbox");
auto _retry = diag_name("retry");
auto _periodic = diag_name("periodic");
auto _shutdown = diag_name("shutdown");

// Setting stat::st_ino to a value that doesn't fit in 32 bits can
// cause trouble for client programs.  Specifically, in a 32-bit
// client progran that was not compiled with __USE_FILE_OFFSET64
// option, the 'stat' in glibc will fail with errno=EOVERFLOW (75).
// Note that many things use stat.  Among (many) others, execvp.
//
// We provide a workaround by which we truncate stat::st_ino to 32
// bits.  By default, st_ino_mask does nothing.  If the environment
// variable Fs123TruncateTo32BitIno is set, then the st_ino_mask is
// set to 0xffffffff.

// When Fs123TruncateTo32BitIno is set, the 'ino' used by the kernel
// and managed with lookup/destroy is still 64 bits, but it differs
// from the st_ino returned to user-space in the stat buffer.  This
// *might* be a very bad idea.  We are on *very* thin ice.
// Remarkably, it doesn't seem to cause problems in practice.  But
// perhaps that means we just haven't looked hard enough.  Bottom
// line: use Fs123TruncateTo32BitIno only as a last resort.  Prefer
// to recompile those crufty old 32-bit programs with 64-bit I/O
// capabilities and keep your inos consistent.
fuse_ino_t st_ino_mask = ~fuse_ino_t(0);

// Fs123Chunk (in KiB) is set by an eponymous -o options or
// environment variable.  See the assignments from envto in main()
// for default values.
size_t Fs123Chunk;

// linkmap and attrcache cannot be staticly constructed because their
// constructors depend on runtime options or environment variables.
// Nevertheless, they are used freely, without checking for validity
// in the callbacks.  Wrapping them in a unique_ptr prevents valgrind
// from complaining about them at program termination.

std::string baseurl;
static_assert(sizeof(fuse_ino_t) == sizeof(uint64_t), "fuse_ino_t must be 64 bits");
std::unique_ptr<expiring_cache<fuse_ino_t, std::string>> linkmap;
struct attrcache_value_t{
    std::string content;
    uint64_t estale_cookie;
    attrcache_value_t() : content{}, estale_cookie{}{}
    attrcache_value_t(const std::string& _content, uint64_t _esc):
        content(_content), estale_cookie(_esc)
    {}
};
std::unique_ptr<expiring_cache<fuse_ino_t, attrcache_value_t, clk123_t>> attrcache;
bool privileged_server;
bool support_xattr;

// Our 'backends' are stacked.  So they can call one another, they may hold *non-owning* pointers
// to one another.  Ownership is managed here, with file-scope unique_ptrs that
// are destroyed in the fs123_destroy callback.
backend123* be;
std::unique_ptr<backend123_http> http_be;
std::unique_ptr<diskcache> diskcache_be;
std::unique_ptr<distrib_cache_backend> distrib_cache_be;

// The maintenance task runs in the background, started in fs123_init and destroyed
// in fs123_destroy
std::unique_ptr<core123::periodic> maintenance_task;

// All the configuration and environmental values that we update and consume
// in multiple places are declared as std::atomic members of volatiles.
std::unique_ptr<volatiles_t> volatiles;

// If we run a subprocess, we assign it to a unique_ptr in fs123_init and join
// it just before returning from main().
std::unique_ptr<std::thread> subprocess;

std::string sharedkeydir_name;
acfd sharedkeydir_fd;
unsigned sharedkeydir_refresh;
std::string encoding_keyid_file;
std::unique_ptr<secret_manager> secret_mgr;
bool encrypt_requests;

// configurable, but can't be changed after startup
bool enhanced_consistency;
std::string signal_filename;
bool no_kernel_data_caching; // DEBUGGING TESTING ONLY.  WILL KILL PERFORMANCE!
bool no_kernel_attr_caching;   // DEBUGGING TESTING ONLY.  WILL KILL PERFORMANCE!
bool no_kernel_dentry_caching;   // DEBUGGING TESTING ONLY.  WILL KILL PERFORMANCE!

std::string executable_path;
std::string cache_dir;
std::string diag_destination;
std::string log_destination;

void massage_attributes(struct stat* sb, fuse_ino_t ino){
    sb->st_ino = ino & st_ino_mask;
    // Adjust permission bits to reflect our ability to actually read,
    // write or execute files.  If we clear the bits here, then system
    // calls will fail with an EACCES (Permission denied) that corresponds
    // to the reported permissions.  See docs/Fs123Permissions.

    // It's a read-only filesystem.
    sb->st_mode &= ~(S_IWUSR|S_IWGRP|S_IWOTH);
    // See the comment below where we set privileged_server.
    if(!privileged_server){
        if( !(sb->st_mode & S_IROTH) ){
            // If 'other' can't read it, then nobody can.
            sb->st_mode &= ~(S_IRUSR|S_IRGRP|S_IROTH);
            // If it's a regular file and we can't read it,
            // then we can't execute it either.
            if(S_ISREG(sb->st_mode))
                sb->st_mode &= ~(S_IXUSR|S_IXGRP|S_IXOTH);
        }
        if(S_ISDIR(sb->st_mode)){
            // The 'x' bit in directories is tricky.  If
            // the server can't execute it, then we can't
            // either, even if it's readable.
            if( !(sb->st_mode & S_IXOTH) )
                sb->st_mode &= ~(S_IXUSR|S_IXGRP|S_IXOTH);
        }
    }
}

fuse_ino_t genino(uint64_t estale_cookie, fuse_ino_t pino, const str_view name){
    return threeroe(name, pino, estale_cookie).hash64();
}

bool cookie_mismatch(fuse_ino_t ino, uint64_t estale_cookie){
    // It's a "mismatch" if we haven't been told to ignore mismatches
    // and if the ino computed from the estale_cookie and the ino is
    // the same as the ino itself.  A zero value for ino is the
    // caller's way of saying "Relax.  We've never seen this
    // estale_cookie before".
    if(ino == 0)
        return false;
    if( volatiles->ignore_estale_mismatch ){
        stats.estale_ignored++;
        return false;
    }
    if( ino == 1 ){
        // The root (ino=1) is weird.  It's ino is *not* equal to
        // the hash of its parents ino, its estale_cookie and its name!
        //
        // Stash the root's estale_cookie the first time we see it,
        // and it's non-zero, and then compare with it ever after.
        // Too clever by half?  Should we just return false??
        //
        // No need for std::atomic here, right?  Static initializers
        // are thread-safe in C++11, right?
        static uint64_t root_estale_cookie{estale_cookie};
        return root_estale_cookie != estale_cookie;
    }else if(ino <= max_special_ino){
        // special inos are similarly weird.  They never mismatch.{
        return false;
    }
    auto pino_name = ino_to_pino_name(ino);
    auto gino = genino(estale_cookie, pino_name.first, pino_name.second);
    if(ino != gino)
        DIAGfkey(_estale, "cookie_mismatch: ec=%ju, name=%s, ino=%ju genino=%ju\n",
                 (uintmax_t)estale_cookie,
                 pino_name.second.c_str(),
                 (uintmax_t)ino, (uintmax_t)gino);
    return ino != gino;
}

// ttl_or_stale is used to:
//   - set the timeout in the attrcache
//   - set the timeout in the linkcache
//   - set the attr_timeout in lookup and getattr
//   - set the entry_timeout in lookup
// All four have the property that a non-zero ttl_or_stale will cause
// us to use this reply until the ttl_or_stale expires.
// I.e., we won't contact the backend for a refresh until the ttl_or_stale
// expires, which is good for latency and bandwidth, but bad for
// freshness.
auto ttl_or_stale(const reply123& r){
    if(!r.valid())
        throw se(EINVAL, "ttl_or_stale called with invalid reply123");
    auto ttl = r.ttl();
    using ttl_t = decltype(ttl);
    if(ttl>= ttl_t::zero())
        return ttl;
    // r is stale.  We might be in the stale_while_revalidate window,
    // or we might be in the (potentially much longer) stale_if_error
    // window.  If we're in the swr window it should be the case that
    // the backend is already revalidating.  It would not be incorrect
    // to continue using the stale data until the window expires.  So
    // any ttl between 0 and the end of the stale-while-revalidate
    // window is "legal".
    ttl_t staleness = -ttl; // positive
    ttl_t tt_too_stale = r.stale_while_revalidate - staleness;
    // On the other hand, we *should* use the refreshed data when it
    // becomes available, which argues for returning a short duration.
    // So let's give ourselves the time till the window closes,
    // clipped from above by 1 second and clipped from below by zero
    // because we don't want to explore the kernel's reaction to a
    // negatve ttl.
    return clip(ttl_t::zero(), tt_too_stale, ttl_t(std::chrono::seconds(1)));
}

// delay_manager - construct one of these before starting a series of
// retry-able backend requests.  Call the dm.delay() method before
// retrying.  If dm.delay() returns false, it does so quickly (without
// delay) and it means that the delay_manager's retry_timeout has been
// reached.  It's time to abandon the retry loop.  Otherwise,
// dm.delay() sleeps for an interval that's initialized to
// initial_delay_millis, grows expoentitally till it reaches
// saturation_seconds, and then stays constant until the total elapsed
// time reaches ttl_seconds, before returning true.
template <class Clk = std::chrono::system_clock>
struct delay_manager{
    typename Clk::time_point end_time;
    typename Clk::duration delay_for;
    typename Clk::duration saturation;
    delay_manager(int ttl_seconds, int initial_delay_millis, int saturation_seconds) :
        end_time(),
        delay_for(std::chrono::milliseconds(initial_delay_millis)),
        saturation(std::chrono::seconds(saturation_seconds))
    {
        // Note that if ttl_seconds is <=0 we avoid any calls to system_clock::now!
        if(ttl_seconds > 0)
            end_time = Clk::now() + std::chrono::seconds(ttl_seconds);
    }        
    
    // delay should return false quickly, or it should sleep for an
    // appropriate interval before returning true.
    bool delay(){
        if(end_time == typename Clk::time_point() || end_time < Clk::now()){
            DIAGfkey(_retry,  "delay_manager::delay time exceeded.  Returning false\n");
            return false;
        }
        DIAGkey(_retry, "delay_manager::delay.  Still some time left.  delay for: " << str(delay_for) << "\n");
        std::this_thread::sleep_for(delay_for);
        stats.toplevel_retries++;
        delay_for *= 2;
        if( delay_for > saturation )
            delay_for = saturation;
        return true;
    }
};

template <class Clk_t>
void rethrow_to_abandon_retry(std::exception& e, const req123& req, const reply123& reply, delay_manager<Clk_t>& dm){
    // Decide between retrying and giving up.
    // 
    // ***MAY ONLY BE CALLED FROM WITHIN AN EXCEPTION HANDLER***
    //
    // To retry, return normally, possibly after tweaking the timeout
    // parameters in dm.  (N.B.  the existing API doesn't really allow
    // for this).  To give up, call throw or throw_with_nested.
    //
    // This decision is inevitably imperfect.  Remember - the price of
    // "hopeless" retries is that applications hang and are probably
    // uninterruptible while we're retrying.  A delayed retry is thus
    // far more costly than just a bit of network resource
    // consumption!  It can be almost as disruptive as an error
    // return, and an error return following a long delay is far more
    // disruptive than an immediate error return.
    DIAGfkey(_retry, "rethrow_to_abandon_retry(%s)\n", req.urlstem.c_str());
    complain(LOG_WARNING, e, "be->refresh(req.urlstem=" + req.urlstem + ") threw an exception.  Contemplating delay/retry");

    unused(reply); unused(dm);
    // Look for any libcurl_category or http_error_category exceptions
    // in the nest.  http errors are probably the innermost error, but
    // because of the trickiness of throwing C++ exceptions from
    // libcurl callbacks, it's possible that libcurl_category
    // exceptions aren't at the innermost nesting level.
    const std::system_error* lcep = nullptr;
    const std::system_error* hep = nullptr;
    for(auto& er : rexnest(e)){
        auto sep = dynamic_cast<const std::system_error*>(&er);
        if(!sep)
            continue;
        auto& category = sep->code().category();
        if(category == libcurl_category()){
            DIAGfkey(_retry, "Found a libcurl_category error\n");
            lcep = sep;
            break;
        }
        if(category == http_error_category()){
            DIAGfkey(_retry, "Found a http_error_category error\n");
            hep = sep;
            break;
        }
    }
    if(lcep){
        // There's a libcurl error here.  Should we retry?
        auto v = lcep->code().value();
        unsigned eno = libcurl_category_t::os_errno(v);
        int curlcode = libcurl_category_t::curlcode(v);
        switch(eno){
#ifdef ECONNRESET
        case ECONNRESET:
#endif
#ifdef ETIMEDOUT
        case ETIMEDOUT:
#endif
#ifdef ENETDOWN
        case ENETDOWN:
#endif
#ifdef ENETUNREACH
        case ENETUNREACH:
#endif
#ifdef EHOSTDOWN
        case EHOSTDOWN:
#endif
#ifdef EHOSTUNREACH
        case EHOSTUNREACH:
#endif
            DIAGfkey(_retry, "curl error CURLcode=%d with errno=%d.  This errno is retryable\n", curlcode, eno);
            return;
        }
        switch(curlcode){
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_OPERATION_TIMEDOUT:
            DIAGfkey(_retry, "curl error CURLcode=%d, errno=%d.  This CURLcode is retryable\n", curlcode, eno);
            return;
        }
        std::throw_with_nested(std::runtime_error(fmt("retrying_berefresh:  curl error CURLcode=%d, errno=%d.  no retry\n", curlcode, eno)));
    }
            
    if(hep){
        auto code = hep->code();
        auto cv = code.value();
        switch(cv){
        case 500:  // Internal  server error
        case 501:  // Not Implemented
        case 502:  // Bad Gateway
        case 504:  // Gateway Timeout
            // FIXME:  THIS IS NOT RIGHT.  These should probably
            // be handled by failing over at the low level rather than
            // retrying (with a delay) at the high level.  But until
            // we fix that, it's better to retry here than to just
            // give up.
        case 503: // Service Unavailable
            // 503 is a temporary server failure.  The server may
            // even have sent a Retry-timeout, but that's not
            // plumbed through yet (if ever).  Nevertheless, we
            // cross our fingers and hope that a retry works...
            DIAGfkey(_retry, "http status 503 is retryable\n");
            return;
        default:
            // These are most likely 500, 404 or 400.  They won't
            // be fixed until somebody fixes a goofed-up
            // configuration, which is unlikely to happen before
            // retry_timeout.  Better to give up promptly.
            std::throw_with_nested(std::runtime_error("retrying_berefresh: no retry on http error with status " + std::to_string(cv)));
        }
        // NOTREACHED
    }
    // It's *neither* an http error nor a libcurl error.  It could be:
    // - something out of our control, e.g., bad_alloc
    // - a program error, e.g., a map::at that
    //   "should have worked"
    // - bad data from the server, e.g., an svto parse error
    // In all these cases, it's hard to imagine that retrying
    // is going to help, so the logic is simple:  rethrow.
    std::throw_with_nested(std::runtime_error("retrying_berefresh: no retry on std::exception from be->refresh(req.urlstem="+req.urlstem+")"));
}

// Retries - The default value of 0 for RetryTimeout makes the other
// default values moot.  But if the caller sets RetryTimeout to a
// non-zero value, then the default values of RetryInitialMillis and
// RetrySaturate mean that failed refreshes (typically due to network
// outages and unreliable servers) will be retried starting at
// 100msec, exponentially backing off to 1sec, and then for one second
// until the RetryTimeout is reached.
bool retrying_berefresh(const req123& req, reply123* reply){
    delay_manager<> dm(volatiles->retry_timeout, volatiles->retry_initial_millis, volatiles->retry_saturate);
    while(1)
        try{
            return be->refresh(req, reply);  
        }catch(std::exception &e){
            rethrow_to_abandon_retry(e, req, *reply, dm);
            if(!dm.delay())
                std::throw_with_nested(std::runtime_error("retrying_berefresh: retry_timeout reached.  no more retries for urlstem=" + req.urlstem));
        }
}

bool berefresh_decode(const req123& req, reply123* reply){
    bool ret = retrying_berefresh(req, reply);
    if(secret_mgr){
        if(reply->content_encoding == content_codec::CE_IDENT)
            throw se(EIO, "server replied in cleartext but client has a secret manager and requires encryption");
        auto sp = content_codec::decode(reply->content_encoding, as_uchar_span(reply->content), *secret_mgr); // might throw, trashes content
        // FIXME: If reply->content were a span, we'd just put it on
        // the lhs of the assignment above and we'd be done.  But
        // since it's a string, we're obliged to make a new one and
        // copy the decoded span into it.
        reply->content = std::string(as_str_view(sp));
        reply->content_encoding = content_codec::CE_IDENT;
    }else{
        if(reply->content_encoding != content_codec::CE_IDENT)
            throw se(EIO, "reply is encoded, but client does not have a sharedkeydir");
    }
    return ret;
}

// fullname - slightly tricky because pino might be the ino of the
// *parent* of the mount-point itself, in which case, we can't call
// ino_to_fullname on it...
std::string fullname(fuse_ino_t pino, str_view lastcomponent){
    // assert( !( lastcomponent.empty() ^ (pino==g_mount_dotdot_ino) ) )
    return lastcomponent.empty() ? "" : ino_to_fullname(pino) + "/" + std::string(lastcomponent);
}

// The attrcache_key can't be the same as genino because when we look
// things up in the attrcache, we don't have the estale_cookie.  Any
// hash function would be fine - the fact that it's the same as
// genino(estale_cookie=0) is "mere coincidence".
uint64_t attrcache_key(fuse_ino_t pino, str_view lastcomponent){
    return threeroe(lastcomponent, pino).hash64();
}

// The content of an /a/ reply  (protocol 7.1) is:
//    <struct stat>  '\n', <validator>

// This is a horrible API!  It's also not cheap to parse struct stats
// over and over.  But rather than optimize prematurely we'll assess
// the cost with some counters.

// sometimes we just want the struct stat
struct stat
stat_from_a_reply(const std::string& sv) try{
    stats.stat_scans++;
    struct stat ret;
    svscan(sv, &ret); // don't use svto because there's probably trailing non-whitespace, i.e., the estale_cookie.
    return ret;
}catch(std::exception&){
    std::throw_with_nested(std::runtime_error("stat_from_a_reply(" + std::string(sv) + "\n"));
 }

// sometimes we just want the validator (see below, outside the anonymous namespace)

// and sometimes we want both
auto
pair_from_a_reply(const std::string& sv) try {
    std::pair<struct stat, uint64_t> ret;
    stats.stat_scans++;
    auto off = svscan(sv, &ret.first, 0);
    if(proto_minor >= 1){
        stats.validator_scans++;
        svscan(sv, &ret.second, off);
    }else{
        // The 7.0 client effectively used st_mtim as the validator,
        // so we use that here.
        ret.second = ret.first.st_mtim.tv_sec * 1000000000  + ret.first.st_mtim.tv_nsec;
    }
    return ret;
}catch(std::exception&){
    std::throw_with_nested(std::runtime_error("pair_from_a_reply(" + std::string(sv) + "\n"));
 }

void encrypt_request(req123& req){
    if(secret_mgr){
        auto esid = secret_mgr->get_encode_sid();
        size_t sz = req.urlstem.size();
        const size_t leader = sizeof(fs123_secretbox_header) + crypto_secretbox_MACBYTES; // crypto_secretbox_MACBYTES == 16
        const size_t padding = 8;
        uchar_blob ub(sz + leader + padding); // enough space for zerobytes and padding.
        padded_uchar_span ps(ub, leader, sz);
        ::memcpy(ps.data(), req.urlstem.data(), sz);
        secret_sp secret = secret_mgr->get_sharedkey(esid);
        padded_uchar_span encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX,
                                                          esid, secret,
                                                          ps,
                                                          padding, true/*derived_nonce*/);
        req.urlstem = "/e/" + macaron::Base64::Encode(std::string(as_str_view(encoded)));
    }
}

// beflush - called when an estale mismatch tells us that we *might*
// have old data in the attrcache or in web caches.  Erases the attrcache
// and tries to force refresh in web caches.
void beflush(fuse_ino_t pino, str_view lastcomponent){
    auto key = attrcache_key(pino, lastcomponent);
    attrcache->erase(key);
    std::string name = fullname(pino, lastcomponent);
    req123 req = req123::attrreq(name, req123::MAX_STALE_UNSPECIFIED);
    req.no_cache = true;
    if(encrypt_requests)
        encrypt_request(req);
    reply123 unused;
    berefresh_decode(req, &unused);
}

// berefresh - common code for begetXXX.  Calls berefresh_decode, and
// optionally checks for staleness mismatch.  If there's a mismatch,
// tries again with req.no_cache = true, and if there's still a
// mismatch, invalidate the entry in the kernel, call beflush to
// flush/replace the attributes in the attrcache and web caches, and
// throw an ESTALE.
void berefresh(fuse_ino_t ino, req123& req, reply123* reply, bool check_cookie){
    if(encrypt_requests)
        encrypt_request(req);
    berefresh_decode(req, reply);
    if(!(reply->eno==0 && check_cookie && cookie_mismatch(ino, reply->estale_cookie)))
        // We return from here the vast majority of  the time!
        return;

    // Either we weren't caching to begin with, or we got a cookie
    // mismatch with the cache in play.  Try again without caching:
    DIAGfkey(_estale, "ESTALE mismatch retry %s\n", req.urlstem.c_str());
    stats.estale_retries++;
    req123 ncreq = req;
    ncreq.no_cache = true;
    berefresh_decode(ncreq, reply);
    if(reply->eno==0 && check_cookie && cookie_mismatch(ino, reply->estale_cookie)){
        // The estale cookie has changed, making the ino itself bogus.
        // Let's tell the kernel:
        //
        // No need to invalidate the ino itself: no promises are
        // broken if an open descriptor gets a page from buffer cache.
        // lowlevel_notify_inval_inode_detached(ino, 0, 0);
        //
        // But we do want to invalidate the entry so that lookups
        // don't get the old ino.  The kernel will (eventually) tell
        // us to 'forget' it.
        auto pino_name = ino_to_pino_name(ino);
        lowlevel_notify_inval_entry_detached(pino_name.first, pino_name.second);
        // We've already refreshed any stale copies of *this* URL in
        // caches by calling berefresh_decode with
        // req.no_cache=true.  But it's possible (likely) that *this*
        // URL is up to date, and the mismatch is because the
        // /a/ 'getattr' URL is stale.  For the benefit of future
        // accesses, we want to flush that too.
        beflush(pino_name.first, pino_name.second);
        stats.estale_thrown++;
        throw se(ESTALE, "estale detected in befresh:  req.urlstem: " + req.urlstem
                + " reply->estale_cookie: " + std::to_string(reply->estale_cookie));
    }
}

// The different variants of begetchunk are "necessary" because of the need
// to accomodate unpredictable (possibly negative) values of
// chunkstart and the begin flag necessary for readdir.
reply123 begetchunk_dir(fuse_ino_t ino, bool begin, int64_t start){
    reply123 ret;
    std::string name = ino_to_fullname(ino);
    req123 req = req123::dirreq(name, Fs123Chunk, begin, start);
    berefresh(ino, req, &ret, true);
    return ret;
}    

reply123 begetchunk_file(fuse_ino_t ino, int64_t startkib, bool no_cache = false){
    reply123 ret;
    std::string name = ino_to_fullname(ino);
    req123 req = req123::filereq(name, Fs123Chunk, startkib, req123::MAX_STALE_UNSPECIFIED);
    req.no_cache = no_cache;
    berefresh(ino, req, &ret, true);
    return ret;
}    

reply123 begetattr(fuse_ino_t pino, str_view lc, fuse_ino_t ino, int max_stale){
    auto key = attrcache_key(pino, lc);
    auto cached_reply = attrcache->lookup(key);
    if( !cached_reply.expired() ){
        DIAGkey(_getattr, "attrcache hit: " << cached_reply.content << " ec: " << cached_reply.estale_cookie << " good_till: " << ins(cached_reply.good_till) << " (" << ins(until(cached_reply.good_till)) << ")\n");
        if( !cookie_mismatch(ino, cached_reply.estale_cookie) )
            return {std::move(cached_reply.content), content_codec::CE_IDENT, cached_reply.estale_cookie, cached_reply.ttl()};
        // It's not clear how we get here.  But if we're here
        // the reply stored in the attrcache is for the same name, but
        // a different 'ino' than the one we're being asked about.
        // Delete the attrcache entry and fall through to refresh.
        complain(LOG_NOTICE, "attrcache erased:  cookie mismatch in " + strfunargs("begetattr", pino, lc,  ino) + " cached.sb: " + cached_reply.content + " cached.estale_cookie: " + str(cached_reply.estale_cookie));
        attrcache->erase(key);
    }
    DIAGkey(_getattr, str("attrcache miss: pino:", pino, "lastcomponent:", lc, "max_stale:", max_stale));
    // If we get here, the key is no longer in the attrcache.  Either
    // it was expired, in which case it was erased as part of the lookup,
    // or it was unexpired but had a cookie_mismatch, in which case
    // we erased it on the line above.  In any case, there's no need
    // to erase it again.
    std::string name = fullname(pino,  lc);
    req123 req = req123::attrreq(name, max_stale);
    reply123 ret;
    berefresh(ino, req, &ret, true);
    if(ret.eno == 0){
        DIAGfkey(_getattr, "/a reply with content: %s\n", ret.content.c_str());
        // A subsequent request might be for max-stale=0.  Since the
        // attrcache doesn't understand swr or max-stale, we can't put
        // anything in the cache that's expired but within the swr
        // window.  Therefore, use ret.ttl(), not ttl_or_stale(ret)
        // for the insertion ttl!
        //
        // FIXME: the attrcache should "understand"
        // stale-while-revalidate and max-stale.
        auto ttl = ret.ttl();
        if( ttl > decltype(ttl)::zero() ){
            bool inserted = attrcache->insert(key, attrcache_value_t(ret.content, ret.estale_cookie), ttl);
            DIAGfkey(_getattr, "attrcache->insert(pino=%ju, lastcomponent=%s, key=%ju, name=%s, estale_cookie=%ju ttl=%.6f):  %s\n",
                     (uintmax_t)pino, std::string(lc).c_str(),
                     (uintmax_t)key, name.c_str(), (uintmax_t)ret.estale_cookie, dur2dbl(ttl),
                     inserted? "replaced" : "did not replace");
        }else{
            DIAGfkey(_getattr, "attrcache: did not insert stale attributes:  ttl: %.6f", dur2dbl(ttl));
        }
    }
    return ret;
}

reply123 begetattr(fuse_ino_t ino, int max_stale){
    auto pino_name = ino_to_pino_name(ino);
    return begetattr(pino_name.first, pino_name.second, ino, max_stale);
}

reply123 begetstatfs(fuse_ino_t ino) {
    reply123 ret;
    std::string name = ino_to_fullname(ino);
    req123 req = req123::statfsreq(name);
    berefresh(ino, req, &ret, false);
    return ret;
}    

reply123 begetlink(fuse_ino_t ino) {
    reply123 ret;
    std::string name = ino_to_fullname(ino);
    req123 req = req123::linkreq(name);
    berefresh(ino, req, &ret, false);
    return ret;
}    

reply123 begetxattr(fuse_ino_t ino, const char *attrname, bool nosize){
    reply123 ret;
    std::string name = ino_to_fullname(ino);
    // TODO should we have an Fs123AttrMax rather than using Fs123Chunk?
    req123 req = req123::xattrreq(name, nosize ? 0 : Fs123Chunk, attrname);
    berefresh(ino, req, &ret, false);
    return ret;
}

// We much prefer to take external commands via the ioctl mechanism,
// or 'special' readable inos, but for some things those mechanisms
// aren't practical.  So we provide a *VERY* limited command pipe,
// under the control of:
//   -oFs123CommandPipe=/path/to/named_pipe
// The actual named pipe created in the filesystem will be:
//      /path/to/named_pipe.PID
// If the option is not provided, then the pipe isn't created,
// the thread is not launched, and there is no command inteprereter.
std::atomic<bool> named_pipe_done;
int named_pipe_fd;
std::thread named_pipe_thread;
std::string named_pipe_name;

// named_pipe_func: read a single byte from named_pipe_fd, and if that
// byte is 's', then statistics are written to the complaint channel.
// We envision the possibility of adding a few more cases should the
// need arise.  BUT THIS WILL NEVER EVOLVE INTO A COMMAND LINE
// INTERPRETER!
void named_pipe_func(){
    char cmd;
    complain(LOG_NOTICE, "start named pipe command loop");
    while(!named_pipe_done && read(named_pipe_fd, &cmd, 1)==1){
        switch(cmd){
        case 's': // Report statistics to the complaint channel.
            // Normally, statistics are available in
            // .fs123_statistics, but when a mount.fs123 process has
            // been lazily unmounted, we can no longer get to that
            // file.
            try{
                std::stringstream ss;
                report_stats(ss);
                std::string line;
                // one complaint per line: ugly, but better than a
                // single complaint that's too long for syslog.  We
                // don't expect to use this much, so it's not worth
                // the trouble to make it pretty.
                complain(LOG_NOTICE, "Statistics report from CommandPipe handler:");
                while(std::getline(ss, line))
                    complain(LOG_NOTICE, line);
            }catch(...){}
            break;
        }
    }
    complain(LOG_NOTICE, "end named pipe command loop");
}

// Boilerplate catch-blocks for all "ops".  The "expected exceptions"
// (these are like known unknowns), are of type std::system_error.  We
// extract the errno from anything that looks like a std::system_error
// in the std::system_category(), and propagate the errno back to
// kernel with reply_err.  If it's another kind of std::exception, or
// if its category is not equal to std::system_category() we call it
// an EIO.  If something is thrown that's not even a std::exception,
// we let it go and plan to look at the resulting core dump :-(.
void caught(std::system_error& se, fuse_ino_t ino, fuse_req_t req, const char* func) try{
    int eno;
    auto& code = se.code();
    if(code.category() == std::system_category())               
        eno = code.value();                                     
    else                                                        
        eno = EIO;                                                      
    auto ctx = fuse_req_ctx(req);
    complain(se, "std::system_error::what: %s(req=%p, ino=%ju (%s)) caught std::system_error: returning errno=%d to pid=%d uid=%d gid=%d",
             func, req, (uintmax_t)ino, ino_to_fullname_nothrow(ino).c_str(), int(eno), int(ctx->pid), int(ctx->uid), int(ctx->gid)); 
    stats.caught_system_errors++;
    return reply_err(req, eno);                                           
 }catch(...){
    complain(LOG_CRIT, "caught handler threw an exception.  Something is very wrong");
 }

void caught(std::exception& e, fuse_ino_t ino, fuse_req_t req, const char *func) try{
    auto ctx = fuse_req_ctx(req);
    complain(e, "std::exception::what: %s(req=%p, ino=%ju (%s)) caught std::exception: returning EIO to pid=%d uid=%d gid=%d",
             func, req, (uintmax_t)ino, ino_to_fullname_nothrow(ino).c_str(), int(ctx->pid), int(ctx->uid), int(ctx->gid));
    stats.caught_std_exceptions++;
    return reply_err(req, EIO);                                                
 }catch(...){
    complain(LOG_CRIT, "caught handler threw an exception.  Something is very wrong");
 }

#define CATCH_ERRS                                              \
 catch(std::system_error& se){                                    \
     caught(se, ino, req, __func__);                                 \
 }catch(std::exception &e){                                         \
     caught(e, ino, req, __func__);                                  \
 }                                                                  

void regular_maintenance(){
    // Load average:
#if __has_include(<sys/sysinfo.h>)
    // sysinfo is linux-specific.
    struct sysinfo si;
    static const float si_load_inv = 1.f/(1 << SI_LOAD_SHIFT); // convert si.load to a float
    if(::sysinfo(&si) == 0){ // no sew::, but we might not want it anyway.
        volatiles->load_average.store(si.loads[0]*si_load_inv);
    }
#elif __has_include(<sys/sysctl.h>)
    struct loadavg la;
    size_t len;
    if(sysctlbyname("vm.loadavg", &la, &len, NULL, 0) != 0)
	volatiles->load_average.store(float(la.ldavg[0])/la.fscale);
#else
#error "Don't know how to get load averages on this platform"
    // popen("sysctl -n vm.loadavg") should work on OS X and FreeBSD
    // popen("uptime") may be even more generic.  
    // google for getloadavg.c for a multiplatform monstrosity.
    // 
    // At the very least, issue a warning if volatiles->load_timeout_factor is non-zero.
#endif
    DIAG(_periodic, "updated volatiles->load_average to " << volatiles->load_average);

    // secret_manager:
    if(secret_mgr)
        secret_mgr->regular_maintenance();

    // the backend_http regular maintenance calls getaddrinfo to keep
    // the name cache up to date.  It can take a while if the network
    // is flakey or if the load average is very high.  Nevertheless,
    // it's better to take the hit in the maintenance thread than to
    // take it in a content-serving thread.
    http_be->regular_maintenance();

    if(distrib_cache_be)
        distrib_cache_be->regular_maintenance();

#if __has_include(<mcheck.h>)
    if(getenv("MALLOC_CHECK_")){
        mcheck_check_all();
    }
#endif
}

auto once_per_minute_maintenance() try {
    regular_maintenance();
    return std::chrono::minutes(1);
 }catch(std::exception& e){
    complain(e, "once_per_minute_maintenance:  caught and ignored exception.");
    return std::chrono::minutes(1);
 }

void fs123_init(void *, struct fuse_conn_info *conn_info) try {
    complain(LOG_NOTICE, "fs123_init.  Starting up at epoch: %s: conn_info={.major=%u, .minor=%u, .async_read=%u, .max_write=%u, .max_readahead=%u, .capable=%#x, .want=%#x"
#if FUSE_VERSION > 28
             ", .max_background=%u, .congestion_threshold=%u"
#endif
             "}\n",
             str(std::chrono::system_clock::now()).c_str(),
             conn_info->proto_major, conn_info->proto_minor,
             conn_info->async_read, conn_info->max_write, conn_info->max_readahead,
             conn_info->capable, conn_info->want
#if FUSE_VERSION > 28
             ,conn_info->max_background, conn_info->congestion_threshold
#endif
             );
    // In general, it's probably best to wait till here before
    // initializing things...
    //
    Fs123Chunk = envto<size_t>("Fs123Chunk", 128);
    if(Fs123Chunk < 128)
        complain(LOG_WARNING, "Fs123Chunk *may* be too small.  It must be large enough that the largest kernel-read request can be satisfied by no more than two chunks.  Don't be surprised by read errors");

    req123::cachetag = envto<unsigned long>("Fs123CacheTag", 0);
    st_ino_mask = ~fuse_ino_t(0);
    if( getenv("Fs123TruncateTo32BitIno") )
        st_ino_mask = 0xffffffff;

    // StaleIfError - the http backend puts this in cache-control headers.
    //  In addition, the diskcache backend uses it to determine when it's
    //  permissible to return a cached object even if it's stale.
    req123::default_stale_if_error = envto<int>("Fs123StaleIfError", 864000); // 10 days

    // PastStaleWhileRevalidate - added to the value of stale-while-revalidate
    // provided by the server.  This is the one to use if you want to trade
    // consistency for latency.
    req123::default_past_stale_while_revalidate = envto<int>("Fs123PastStaleWhileRevalidate", 0);
    
    // A 'privileged' server is one that can read all the files in its
    // tree.  Arguably, all properly configured servers should be
    // privileged.  But in practice, we sometimes want to export
    // directory trees that might have a few unreadable files or
    // directories.  If the server is 'unprivileged' fs123 will
    // promote 'other' permissions up to 'group' and 'user' in
    // massage_permissions, in which case the client won't ask for
    // things the server can't read, avoiding lots of confusing error
    // messages (50x http error codes and complaints in the server's
    // syslog), but it shouldn't lead to incorrect behavior.
    privileged_server = envto<bool>("Fs123PrivilegedServer", true);

    // Codecs and secrets:
    sharedkeydir_name = envto<std::string>("Fs123Sharedkeydir", "");
    // Unless you're doing something very fancy, you can't "rotate" keys much faster than
    // max_age_long, so there's no point in refreshing the sharedkeydir much more often.
    // The default max_age_long in the server is 86400 (1 day), so:
    sharedkeydir_refresh = envto<unsigned>("Fs123SharedkeydirRefresh", 43200);
    std::string accepted_encodings;
    
    if(!sharedkeydir_name.empty()){
        // *only* accept fs123-secretbox.  The *;q=0 sub-clause says that every other
        // encoding is explicitly forbidden.
        accepted_encodings = "fs123-secretbox,*;q=0";
        encoding_keyid_file = envto<std::string>("Fs123EncodingKeyidFile", "encoding");
        sharedkeydir_fd = sew::open(sharedkeydir_name.c_str(), O_DIRECTORY|O_RDONLY);
        secret_mgr = std::make_unique<sharedkeydir>(sharedkeydir_fd, encoding_keyid_file, sharedkeydir_refresh);
        encrypt_requests = envto<bool>("Fs123EncryptRequests", false);
    }

    proto_minor = envto<int>("Fs123ProtoMinor", fs123_protocol_minor_default);
    if(proto_minor < fs123_protocol_minor_max)
        complain(LOG_NOTICE, "Using backward-compatible protocol_minor=%d", proto_minor);
    else if(proto_minor > fs123_protocol_minor_max)
        throw se(EINVAL, fmt("Fs123ProtoMinor too large.  Maximum value: %d", fs123_protocol_minor_max));

    // One-stop shopping for all our dynamic configuration:
    volatiles = std::make_unique<volatiles_t>();
    
    // The first non-option argument was extracted as 'fuse_device_option'.
    // It's the url we pass to backend123.
    baseurl = backend123::add_sigil_version(fuse_device_option);
    http_be = std::make_unique<backend123_http>(baseurl, accepted_encodings, *volatiles);
    if(!http_be)
        throw se(ENOMEM, fmt("new backend123_http(%s) returned nullptr.  Something is terribly wrong.", fuse_device_option.c_str()));
    std::string fallbacks = envto<std::string>("Fs123FallbackUrls", "");
    // Treat fallbacks as a pipe(|) or whitespace-separated list of
    // strings.  Don't use comma because it confuses fuse/mount
    // arg-parsing.  Pipe is a lot easier to pass through automount
    // executable maps than whitespace.
    if(!fallbacks.empty()){
        size_t b, e=0;
        const std::string sep("| \t\n\f");
        while((b = fallbacks.find_first_not_of(sep, e)) != std::string::npos){
            e = fallbacks.find_first_of(sep, b);
            auto with_sigil = backend123::add_sigil_version(fallbacks.substr(b, e-b));
            http_be->add_fallback_baseurl(with_sigil);
            complain(LOG_NOTICE, "Added fallback url: %s",  with_sigil.c_str());
        }
    }
    be = http_be.get();
    cache_dir = envto<std::string>("Fs123CacheDir", "");
    if(!cache_dir.empty() && volatiles->dc_maxmbytes){
        // By specifying a seed that depends on the baseurl, we make
        // it possible for multiple clients to share the same cache.
        // Requests made to different baseurls will (with high probability)
        // not collide.
        diskcache_be = std::make_unique<diskcache>(be, cache_dir, threeroe(baseurl).hash64(),
                                                   envto<bool>("Fs123CacheFancySharing", false), *volatiles);
        be = diskcache_be.get();
    }

    // FIXME - Options!!
    auto distrib_cache_style = envto<std::string>("Fs123DistribCacheExperimental", "");
    if(!distrib_cache_style.empty()){
        if(!diskcache_be)
            throw se(EINVAL, "Can't have a Distributed Cache without a Diskcache");
        // See the comment at the top of distrib_cache_backend.hpp for a description
        // of these 'styles'
        if(distrib_cache_style == "diskcache-in-front"){
            distrib_cache_be = std::make_unique<distrib_cache_backend>(http_be.get(), diskcache_be.get(), baseurl, *volatiles);
            diskcache_be->set_upstream(distrib_cache_be.get());
        }else if(distrib_cache_style == "diskcache-behind"){
            distrib_cache_be = std::make_unique<distrib_cache_backend>(diskcache_be.get(), diskcache_be.get(), baseurl, *volatiles);
            be = distrib_cache_be.get();
        }else{
            throw se(EINVAL, "Unrecognized value of Fs123DistribCacheExperimental: " + distrib_cache_style + ".  Expected either 'diskcache-in-front' or 'diskcache-behind'");
        }
    }

    auto attrcachesz = envto<size_t>("Fs123AttrCacheSize", 100000);
    attrcache = std::make_unique<decltype(attrcache)::element_type>(attrcachesz);

    auto linkmapsz = envto<size_t>("Fs123LinkCacheSize", 10000);
    linkmap = std::make_unique<decltype(linkmap)::element_type>(linkmapsz);
    ino_remember(g_mount_dotdot_ino, "", 1, ~0);

    enhanced_consistency = envto<bool>("Fs123EnhancedConsistency", true);
    if(enhanced_consistency)
        openfile_startscan();
    no_kernel_data_caching = envto<bool>("Fs123NoKernelDataCaching", false);
    no_kernel_attr_caching = envto<bool>("Fs123NoKernelAttrCaching", false);
    no_kernel_dentry_caching = envto<bool>("Fs123NoKernelDentryCaching", false);

    named_pipe_name = envto<std::string>("Fs123CommandPipe", "");
    if( !named_pipe_name.empty() ){
        named_pipe_name += "." + std::to_string(getpid());
        sew::mkfifo(named_pipe_name.c_str(), 0622); // & ~umask
        named_pipe_done = false;
        named_pipe_fd = sew::open(named_pipe_name.c_str(), O_RDWR);
        named_pipe_thread = std::thread(named_pipe_func);
    }

    auto subprocesscmd = envto<std::string>("Fs123Subprocess", "");
    // N.B.  it's tempting to just 'detach' the thread and let it run,
    // but if we return to our caller while something like system("bash")
    // is running, we find ourselves with two processes reading
    // from stdin.  To avoid that, we do subprocess->join() just before
    // returning to our caller at the end of main.
    if(!subprocesscmd.empty())
        subprocess = std::make_unique<std::thread>([](std::string cmd){
                        complain(LOG_NOTICE, "subprocess thread:  system(\"%s\") starting.  Will call fuseful_teardown when it finishes", cmd.c_str());
                        int ret = ::system(cmd.c_str()); // not sew!
                        complain(LOG_NOTICE, "subprocess thread:  system(\"%s\") returned %d.  Calling fuseful_teardown", cmd.c_str(), ret);
                        fuseful_teardown();
                                                   }, subprocesscmd);
    
    // Start the maintenance_task last and destroy it first (in
    // fs123_destroy) so that the maintenance function can safely
    // assume that all subsystems (volatiles, backends, secret
    // managers, DNS caches, etc.) are live whenever it runs.
    maintenance_task = std::make_unique<core123::periodic>(once_per_minute_maintenance);
}catch(std::exception& e){
    // Something is wrong.  Get out.  If we return the fuse layer will
    // start invoking callbacks, which might not be properly
    // initialized.  If we just exit(), we leave the mount-point
    // borked in an ENOTCONN state.  So what to do?
    // 
    // https://sourceforge.net/fuse/mailman/messages/11634250 suggests
    // that we should call fuse_exit(), but that's in the high-level
    // API.  Looking at the library code, fuse_exit() just calls
    // fuse_session_exit(f->se).  So let's try that.  See the long
    // comment in fuse_main_ll in fuseful.cpp for a blow-by-blow of
    // what happens after fuse_session_exit.
    complain(e, "fs123_init caught exception.  Are we misconfigured?  Exception: ");
    if(g_session){
        complain("fs123_init:  calling fuse_session_exit");
        fuse_session_exit(g_session);
    }else{
        complain(LOG_CRIT, "fs123_init:  g_session is null.  How did we get into the fuse_init callback without a g_session?");
    }
 }

// N.B.  Unlke the other lowlevel ops, the destroy op is *not* called
// by a libfuse thread.  In libfuse it would normally be called by
// fuse_session_destroy, but (see comments in fuseful_main_ll) we call
// it directly from fuseful_teardown.  fuseful_teardown gets called in
// a variety of normal and abnormal contexts.  It is protected by an
// atomic_flag that guarantees that it will return immediately without
// side-effects (such as calling the destroy op) after the first
// invocation.  Bottom line - it's safe to assume that fs123_destroy
// will be called exactly once, but it's not safe to assume anything
// about which thread (or signal handler) might call it.
void fs123_destroy(void*){
    complain(LOG_NOTICE, "top of fs123_destroy");
    DIAG(_shutdown, "top of fs123_destroy");
    // In some cases, (unclear when), the kernel doesn't tell us
    // to 'forget', ino=1.  We could ino_forget() it here, but if
    // the kernel has told us to forget it, a second forget() generates
    // an error message.  Let's just ignore it.
    // ino_forget(1, 1);
    //
    // Various subsystems were initialized in fs123_init.  Shut them
    // down in LIFO order so that any inter-dependencies are respected.
    maintenance_task.reset();
    // N.B.  Don't join the subprocess here!  This_thread *might* be the
    // subprocess thread, and a thread can't join itself.
    if(!named_pipe_name.empty()){
        unlink(named_pipe_name.c_str());
        named_pipe_done = true;
        // poke the named_pipe_func so it notices named_pipe_done
        char noop = ' ';
        // N.B. write() shouldn't raise SIGPIPE because the only place
        // the pipe is closed is a few lines below!  So the reading
        // side "can't possibly" be closed.
        try{
            sew::write(named_pipe_fd, &noop, 1);
        }catch(std::exception& e){
            complain(LOG_ERR, e, "error writing wakeup-byte to named_pipe.  The join might hang...");
        }
        // It should rejoin pretty soon.
        if(named_pipe_thread.joinable()){
            complain(LOG_NOTICE, "joining named pipe");
            named_pipe_thread.join();           DIAG(_shutdown, "named_pipe_thread.join() done");
        }else
            complain(LOG_ERR, "named pipe is not joinable?  Have we called destroy twice?");
        close(named_pipe_fd);
        named_pipe_fd = -1;
    }
    if(enhanced_consistency)
        openfile_stopscan();      DIAG(_shutdown, "openfile_stopscan() done");
    linkmap.reset();              DIAG(_shutdown, "linkmap.reset() done");
    attrcache.reset();            DIAG(_shutdown, "attrcache.reset() done");
    distrib_cache_be.reset();     DIAG(_shutdown, "distrb_cache_be.reset() done");
    diskcache_be.reset();         DIAG(_shutdown, "diskcache_be.reset() done");
    http_be.reset();              DIAG(_shutdown, "http_be.reset() done");
    volatiles.reset();            DIAG(_shutdown, "volatiles_be.reset() done");
    secret_mgr.reset();           DIAG(_shutdown, "secret_mgr.reset() done");
    complain(LOG_NOTICE, "return from fs123_destroy at epoch: " + str(std::chrono::system_clock::now()));
}

void fs123_crash(){
    // Called by fuseful's signal handlers *only* for "Program
    // Termination Signals", e.g., SIGSEGV, SIGILL - the signals that
    // we can't return from.  The very limited goal here is to make a
    // best-effort to clean up anything that would have persistent
    // consequences, e.g., filesystem litter.  Since we're called by a
    // signal handler, only async-signal-safe functions should be
    // used.  Assume as little as possible about the integrity of data
    // structures, threads, etc., and do no more than necessary to
    // achieve the limited goal.  Also note that shutting fuse itself
    // down is handled by our caller, so there's no need to call
    // fuse_unmount, fuse_session_exit, etc.
    if(!named_pipe_name.empty())
        unlink(named_pipe_name.c_str());
}

void fs123_lookup(fuse_req_t req, fuse_ino_t ino, const char *name) try
{
    const auto pino = ino;  // it's really the parent ino.  It's called ino for CATCH_ERRS
    stats.lookups++;
    atomic_scoped_nanotimer _t(&stats.lookup_sec);
    DIAGfkey(_lookup, "lookup(%p, %ju, %s)\n", req, (uintmax_t)pino, name);
    if(lookup_special_ino(req, pino, name))
        return;
    auto reply = begetattr(pino, name, 0, req123::MAX_STALE_UNSPECIFIED);
    struct fuse_entry_param e = {};
    double ttl = dur2dbl(ttl_or_stale(reply));
    e.attr_timeout = no_kernel_attr_caching ? 0. : ttl;
    if( reply.eno == ENOENT ){
        // setting e.ino = 0 tells the kernel to cache the negative
        // result.  Use the attribute's ttl as the entry_timeout.
        // That *might* still be very long, e.g., if we're in a 'long
        // timeout' directory, but that's the backend's
        // responsibility, not ours.
        e.ino = 0;
        e.entry_timeout = no_kernel_dentry_caching? 0 : ttl;
        stats.lookup_enoents++;
        return reply_entry(req, &e);
    }
    if( reply.eno ){
        stats.lookup_other_errno++;
        return reply_err(req, reply.eno);
    }
    e.ino = genino(reply.estale_cookie, pino, name);
    e.entry_timeout = no_kernel_dentry_caching ? 0. : ttl;
    uint64_t validator;
    std::tie(e.attr, validator) = pair_from_a_reply(reply.content);
    if( !privileged_server && S_ISDIR(e.attr.st_mode) && !(e.attr.st_mode&S_IXOTH) ){
        complain(LOG_WARNING, "pino=%ju name=%s is a directory with the 'other' execute bit unset.  Attempts to look inside this directory will fail with EACCES (Permission denied)", (uintmax_t)pino, name);
    }
    massage_attributes(&e.attr, e.ino);
    ino_remember(pino, name, e.ino, validator);
    DIAGkey(_lookup, "lookup(" << req << ") -> stat: " << e.attr << " entry_timeout: " << e.entry_timeout << " attr_timeout: " << e.attr_timeout  <<"\n");
    reply_entry(req, &e);
 } CATCH_ERRS

struct fh_state{
    // Only used by opendir/readdir!
    mutable std::mutex mtx;
    std::string contents;
    int64_t chunk_next_offset;
    bool eof;

    fh_state() : contents{}, chunk_next_offset{0}, eof{false} {}
 };
// We use a shared_ptr<fh_state> to avoid any chance that the fh_state
// (or anything within it, e.g., the mtx) goes out-of-scope while we're
// still using it.  For example, in readdir, we hold a unique_lock 
// on the fh_state's mutex, so the mutex's lifetime must be strictly
// longer than the unqique_lock's.
using fh_state_sp = std::shared_ptr<fh_state>;

void fs123_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) try
{    
    stats.getattrs++;
    atomic_scoped_nanotimer _t(&stats.getattr_sec);
    if(fi)
        stats.getattrs_with_fi++;
    DIAGfkey(_getattr, "getattr(%p, %ju, fi=%p, fi->fh=%p)\n", req, (uintmax_t)ino, fi, fi?(void*)fi->fh:nullptr);
    if( ino>1 && ino <= max_special_ino )
        return getattr_special_ino(req, ino, fi);

    auto reply = begetattr(ino, req123::MAX_STALE_UNSPECIFIED);
    if(reply.eno){
        if(reply.eno == ENOENT)
            stats.getattr_enoents++;
        else
            stats.getattr_other_errno++;
        DIAGfkey(_getattr, "getattr reply_err req=%p r.eno=%d\n", req, reply.eno);
        return reply_err(req, reply.eno);
    }
    struct stat sb = stat_from_a_reply(reply.content);
    massage_attributes(&sb, ino);
    double tosd = no_kernel_attr_caching? 0.0 : dur2dbl(ttl_or_stale(reply));
    DIAGkey(_getattr,  "getattr(" << req <<  ", " << ino << ") -> ttl: " << tosd << " attrs: " << sb << "\n");
    reply_attr(req, &sb, tosd);
 } CATCH_ERRS

void fs123_opendir(fuse_req_t req, fuse_ino_t  ino, struct fuse_file_info *fi) try {
    stats.opendirs++;
    atomic_scoped_nanotimer _t(&stats.opendir_sec);
    fi->fh = reinterpret_cast<decltype(fi->fh)>(new fh_state_sp(std::make_shared<fh_state>()));
    DIAGfkey(_opendir, "opendir(%ju, fi=%p, fi->fh = %p)\n", (uintmax_t)ino, fi, (void*)fi->fh);
    // Documentation is silent on whether keep_cache is honored for
    // directories.  We want the kernel *not* to use cached entries --
    // if it did, then clients would never see new entries because
    // "kernel caches are forever".  I think that keep_cache and
    // direct_io are initialized to zero, but let's not assume:
    fi->keep_cache = false;
    fi->direct_io = false;
    reply_open(req, fi);
} CATCH_ERRS

size_t fuse_add_direntry_sv(fuse_req_t req, char *buf, size_t bufsz, str_view namesv, struct stat* stp, size_t nextoff){
    char *endp = const_cast<char*>(namesv.data()) + namesv.size();
    char comma = *endp;
    *endp = '\0';
    auto ret = fuse_add_direntry(req, buf, bufsz, namesv.data(), stp, nextoff);
    *endp = comma;
    return ret;
}

void fs123_readdir(fuse_req_t req, fuse_ino_t ino, 
                      size_t size, off_t off, struct fuse_file_info *fi) try
{
    stats.readdirs++;
    DIAGfkey(_opendir, "readdir(ino=%ju, size=%zu, off=%jd, fi=%p, fi->fh=%p)\n",
             (uintmax_t)ino, size, (intmax_t)off, fi, fi?(void*)fi->fh:nullptr);
    atomic_scoped_nanotimer _t(&stats.readdir_sec);
    if(fi==nullptr || fi->fh == 0)
        throw se(EIO, "fs123_readdir called with fi=nullptr or fi->fh==0.  This is totally unexpected!");
    fh_state_sp fhstate = *reinterpret_cast<fh_state_sp*>(fi->fh);

    // All readdirs on the same open fd are serialized.
    std::unique_lock<std::mutex> lk{fhstate->mtx};
#if 0
    // See docs/Notes.readdir.  You can't win.
    // #if 1 is the "rewinddir syncs the directory, seekdir(d, 0) might invalidate previous telldirs" solution
    // #if 0 is the "rewinddir doesn't sync the directory, seekdir(d,0) doesn't invalidate previous telldirs" solution.
    if(off == 0){
        fhstate->contents.resize(0);
        fhstate->chunk_next_offset = 0;
        fhstate->eof = false;
    }
#endif

    size_t nextoff = size_t(off);
    if(nextoff == fhstate->contents.size() && !fhstate->eof){
	auto reply = begetchunk_dir(ino, fhstate->contents.empty(), fhstate->chunk_next_offset);
        if( reply.eno )
            return reply_err(req, reply.eno);
        // anti-Postel's law ... be strict in what we accept too
        if( reply.chunk_next_meta == reply123::CNO_MISSING )
            throw se(EIO, fmt("fs123_readdir(%s) reply missing " HHNO " with chunk-next-offset", ino_to_fullname(ino).c_str()));
        fhstate->contents += reply.content;
        fhstate->chunk_next_offset = reply.chunk_next_offset;
	// since we do not assume anything about what offset means, we
	// do  not use it to setsize, so getattr or header stat info
	// must have meaningful st_size
        fhstate->eof = (reply.chunk_next_meta == reply123::CNO_EOF);
    }
    std::array<char, 4096> buf;
    size_t bufsz = std::min(size, buf.size());
    size_t used = 0;
    std::string dir = ino_to_fullname(ino);

    if(nextoff > fhstate->contents.size() + ((ino==1)?(max_special_ino-1):0))
        throw se(EINVAL, fmt("readdir offset invalid.  It points past end of directory.  contents.size()=%zu, ino=%ju, max_special_ino=%zu, off=%zu",
                                     fhstate->contents.size(), (uintmax_t)ino, max_special_ino, nextoff));
    str_view svin(fhstate->contents);
    while(nextoff < svin.size()){
        str_view name;
        uint64_t estale_cookie;
        int d_type; // it fits in a char, but who's counting
        struct stat stbuf;
        // DANGER!  This block of code has security implications.  The
        // value of 'off' is under user control via lseek.  It
        // *should* be a value previously returned as a d_off in an
        // earlier readdir.  But it *could* be anything.  We MUST NOT
        // try to read or write illegal addresses, DoS ourselves with
        // a huge malloc, etc., even if 'off' is bogus.  While it's
        // also true that 'contents' came from an external source (the
        // http server), we're not going to worry (much) about that.
        // If the http server is malicious, getting confused here is
        // the least of our worries.

        // We rely heavily on features of svscan:
        //  - it throws if the input doesn't scan cleanly
        //  - it's safe (i.e., it throws but it avoids making any
        //    illegal memory accesses) if nextoff is too big.
        //  - the svscan<netstring> overload throws before allocating
        //    storage if it's given a string whose length exceeds
        //    maxlen (255, in the call here).
        try{
            nextoff = svscan_netstring(svin, &name, nextoff);
            if(name.size() > 255)
                throw se(EINVAL, "name too long in get_dir_contents_and_attributes");
            if(name.size() == 0)
                throw se(EINVAL, "zero-length name in get_dir_contents_and_attributes");
            // At this point, we're safe... A bogus offset will almost
            // always cause the svscan above to throw.  The only way to
            // get here with a bogus offset is if the offset points *into*
            // a name, and that name (or part of the name) looks like a
            // bona fide entry.  E.g., a perfectly valid entry like this,
            // describing a file called "5:hello,1 1234" with dtype=2 and
            // estale_cookie=65432:
            //
            //      14:5:hello,1 1234,2 65432
            //         ^offset      
            //                        ^nextoff

            // If a malicious (or unlucky) user manages to lseek to just
            // right (wrong) offset, they can persuade us to say that
            // there's an entry called 'hello' here, even though there
            // isn't.  But remember, after a bogus lseek, the caller
            // should expect "undefined behavior".  Our mandate is to
            // protect ourselves, not to return a specific error to the
            // user.
            //
            // In fact, if we haven't filled the outbuf, we'll loop back
            // around with nextoff as illustrated above, pointing to '1 ',
            // and the next svscan will throw.  So actual users are even
            // less likely to see the bogus "hello" entry.
            nextoff = svscan(svin, &d_type, nextoff);
            stbuf.st_mode = dtype_to_mode(d_type);
            nextoff = svscan(svin, &estale_cookie, nextoff);
            nextoff = svscan(svin, nullptr, nextoff); // skip whitespace
        }catch(std::exception&){
            // the caller sees EINVAL, regardless of exactly what was
            // thrown by svscan et al.
            std::throw_with_nested(std::system_error(EINVAL, std::system_category(),
                                                     "directory parsing failed:  likely: invalid user-space lseek on directory fd.  unlikely:  garbled directory contents"));
        }
        stbuf.st_ino = 0;
        if(name[0] == '.'){
            if(name.size() == 1){
                stbuf.st_ino = ino;
                stbuf.st_mode = S_IFDIR;
            }else if(name[1] == '.' && name.size()==2){
                stbuf.st_ino = ino_to_pino(ino);
                stbuf.st_mode = S_IFDIR;
            }
        }
	DIAGfkey(_readdir, "readdir parsed name \"%s\", mode %o, estale_cookie %" PRIu64 "\n",
		 std::string(name).c_str(), stbuf.st_mode, estale_cookie);
        if(stbuf.st_ino == 0){ // i.e., neither "." nor ".."
            if(estale_cookie == 0 && ( S_ISREG(stbuf.st_mode) || S_ISDIR(stbuf.st_mode) ))
                stats.fake_ino_dirents++; // should only see these if the server is configured with --fake_ino_dirents.
            stbuf.st_ino = genino(estale_cookie, ino, name) & st_ino_mask;
        }
        auto needed = fuse_add_direntry_sv(req, buf.data()+used, bufsz-used, name, &stbuf, nextoff);
        // N.B.  If needed > bufsz-used, then fuse_add_direntry did nothing.
        if( needed > bufsz-used )
            break;
        DIAGfkey(_readdir, "fuse_add_direntry(%ju) -> %s, nextoff = %ld used=%zu\n",
                 (uintmax_t)ino, (dir + "/" + std::string(name)).c_str(), nextoff, used);
        used += needed;
    }
    
    // deal with 'special' inos in the root.
    if(ino==1 && fhstate->eof){
        for(fuse_ino_t sino = 2+nextoff - svin.size(); sino <= max_special_ino; ++sino){
            struct stat stbuf = shared_getattr_special_ino(sino, nullptr);
            nextoff += 1;
            auto needed = fuse_add_direntry(req, buf.data()+used, bufsz-used, special_ino_to_fullname(sino), &stbuf, nextoff);
            if( needed > bufsz-used )
                break;
            used += needed;
            DIAGfkey(_readdir, "fuse_add_direntry(%ju) -> %s, nextoff = %ld used=%zu\n",  (uintmax_t)ino, (dir + "/" + special_ino_to_fullname(sino)).c_str(), nextoff, used);
        }
    }
    // We could release the lock sooner but it would require thinking
    // carefully about how contents are stored - and it's not clear
    // anyone would ever benefit (who does multiple concurrent
    // readdirs on the same fd??)
    DIAGfkey(_readdir, "fuse_reply_buf(req, %p, %ld)\n", buf.data(), used);
    reply_buf(req, buf.data(), used);
} CATCH_ERRS

void fs123_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) try
{
    DIAGfkey(_opendir, "releasedir(%ju, fi=%p, fi->fh=%p)\n", (uintmax_t)ino, fi, fi?(void*)fi->fh:nullptr);
    stats.releasedirs++;
    if(fi == nullptr || fi->fh == 0)
        throw se(EINVAL, "fs123_releasedir called with fi==nullptr or fi->fh=0.  This is totally unexpected!");
    delete reinterpret_cast<fh_state_sp*>(fi->fh);
    return reply_err(req, 0);
}CATCH_ERRS


void do_xattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size) {
    if( ino>1 && ino <= max_special_ino )
	return reply_err(req, ENOATTR);
    bool nosize = (size == 0);
    auto reply = begetxattr(ino, name, nosize);
    if (reply.eno)
	return reply_err(req, reply.eno);
    DIAGfkey(_xattr, "getxattr content: %s\n", reply.content.c_str());
    if (nosize) {
	return reply_xattr(req, svto<size_t>(reply.content));
    } else if (size < reply.content.size()) {
	return reply_err(req, ERANGE);
    }
    return reply_buf(req, reply.content.c_str(), reply.content.size());
}

#ifndef __APPLE__
void fs123_getxattr(fuse_req_t req, fuse_ino_t ino,
		    const char *name, size_t size) try
#else
void fs123_getxattr(fuse_req_t req, fuse_ino_t ino,
		    const char *name, size_t size, uint32_t /*position*/) try
#endif
{
    stats.getxattrs++;
    atomic_scoped_nanotimer _t(&stats.getxattr_sec);
    return do_xattr(req, ino, name, size);
} CATCH_ERRS

void fs123_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) try
{
    stats.listxattrs++;
    atomic_scoped_nanotimer _t(&stats.listxattr_sec);
    return do_xattr(req, ino, nullptr, size);
} CATCH_ERRS

void fs123_readlink(fuse_req_t req, fuse_ino_t ino) try
{
    stats.readlinks++;
    atomic_scoped_nanotimer _t(&stats.readlink_sec);
    auto lip = linkmap->lookup(ino);
    if(!lip.expired()){
        stats.shortcircuit_readlinks++;
        return reply_readlink(req, lip.c_str());
    }
        
    auto reply = begetlink(ino);
    if(reply.eno)
        return reply_err(req, reply.eno);
    linkmap->insert(ino, reply.content, ttl_or_stale(reply));
    return reply_readlink(req, reply.content.c_str());
 } CATCH_ERRS

void fs123_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) try {
    stats.opens++;
    atomic_scoped_nanotimer _t(&stats.open_sec);
    DIAGfkey(_open, "open(%p, %ju, flags=%#o)\n", req, (uintmax_t)ino, fi->flags);
    // According to the fuse docs: "Open flags (with the exception of O_CREAT,
    // O_EXCL, O_NOCTTY and O_TRUNC are available in fi->flags".  So
    // we don't have to worry about them.  What about the others.  These
    // are listed in man open(2):
    //
    // APPEND only applies to writing.  Warn.
    //
    // ASYNC will raise SIGIO.  Only available for "terminals,
    // pseudo-terminals, sockets and (since Linux 2.6) pipes and
    // FIFOS".  Reject.
    //
    // DIRECT seems like it's asking for trouble to combine it with fuse.
    // However, it can be useful for debugging and diagnostics, so if
    // it's set, we set fi->direct_io in our reply.
    // 
    // DIRECTORY says only open if it's a directory.  We shouldn't
    // be opening any directories.  Directory reads should go through
    // readdir.  Reject (just in case).
    //
    // LARGEFILE shouldn't be a problem
    //
    // NOATIME shouldn't be a problem
    //
    // NOFOLLOW succeeds only if name is not a symlink.  Shouldn't be
    // a problem.
    //
    // NONBLOCK or NDELAY - POSIX, i.e.,
    // http://pubs.opengroup.org/onlinepubs/9699919799/functions/open.html
    // says: "if FIFO or device ... Otherwise, the O_NONBLOCK flag
    // shall not cause an error, but it is unspecified whether the
    // file status flags will include the O_NONBLOCK flag."  Ignore.
    //
    // SYNC only affects writes.  Warn.
    // 
    // A careful reading of /usr/include/asm-generic/fcntl.h reveals
    // that the above list of macros leaves four bits undocumented.
    // The mystery bits are 0000074, bits 4,8,16 and 32, which fall
    // between O_ACCMODE and O_CREAT.  Some kernel spelunking
    // reveals that when the kernel execs a file, it also OR's
    // in FMODE_EXEC = 32 = 040.  
    //
    // I can't find anything about the other three bits: 000034.
    // There are some other FMODE_ bits, but I don't think they're
    // ever included in open flags.
    if( fi->flags & (O_WRONLY|O_RDWR) )
        return reply_err(req, EROFS);

    if( fi->flags & (O_APPEND|O_SYNC) )
        complain(LOG_WARNING, "fs123_open(ino=%ju, flags=%#o) contains one or more of O_APPEND|O_SYNC=%#o, which only apply to writing.  This is a read-only fs", (uintmax_t)ino, fi->flags, O_APPEND|O_SYNC);        
    if( fi->flags & (O_ASYNC|O_DIRECTORY) ){
        complain("fs123_open(ino=%ju, flags=%#o) contains one or more unsupported flags in O_ASYNC|O_DIRECTORY=%#o",
                 (uintmax_t)ino, fi->flags, O_ASYNC|O_DIRECTORY);
        return reply_err(req, ENOSYS);
    }
    if( fi->flags & 034 )
        complain(LOG_WARNING, "fs123_open(ino=%ju, flags=%#o) has undocumented/unrecognized bits in 034 set in flags",
                 (uintmax_t)ino, fi->flags);

    if(ino > 1 && ino <= max_special_ino)
        return open_special_ino(req, ino, fi);
    reply123 r = begetattr(ino, req123::MAX_STALE_UNSPECIFIED);
    if(r.eno != 0)
        return reply_err(req, r.eno);
#ifdef O_DIRECT
    fi->direct_io = fi->flags&O_DIRECT;
#endif
    if(no_kernel_data_caching)
        fi->direct_io = true;

    // Update the inomap with the just-retrieved validator.  We can
    // keep the kernel-cache if the just-retrieved validator is the
    // same as the validator that had been recorded in the inomap
    // (returned by ino_update_validator).  The inomap is initialized
    // with a zero validator, which compares unequal to just-retrieved
    // mtim the very first time we open an ino.  This would result in
    // an "unnecessary" keep_cache=0 the very first time we open an
    // ino, but since there's nothing to keep, it's moot.  We could
    // avoid that by also comparing old_validator with zero.
    auto new_validator = validator_from_a_reply(r);
    uint64_t old_validator;
    try{
        old_validator = ino_update_validator(ino, new_validator);
    }catch(std::exception& e){
        // We *may* have seen this once in March 2019, and we don't
        // really know what caused it.  Notifying the kernel seems
        // prudent because if the kernel caches are goofed up, we
        // might not get another chance.  We might also want to flush
        // the attrcache, diskcache and http proxy caches, but let's
        // understand the root-cause first.
        lowlevel_notify_inval_inode_detached(ino, 0, 0);
        std::throw_with_nested(std::runtime_error("non-monotonic validator.  Probable server misconfiguration.  lowlevel_notify_inval_inode_detached(ino) called"));
    }

    // If the reply has max-age==0 turn on direct_io to completely
    // avoid the openfilemap logic.
    if(r.max_age().count() == 0)
        fi->direct_io = true;
    fi->keep_cache = (old_validator == new_validator);
    if(enhanced_consistency && !fi->direct_io){
        fi->fh = openfile_register(ino, r);
    }else{
        fi->fh = 0;
    }
    if(!fi->keep_cache)
        stats.no_keep_cache_opens++;
    if(fi->direct_io)
        stats.direct_io_opens++;
    
    DIAGfkey(_open, "open(req=%p, ino=%ju) -> fi->fh: %p, fi->keep_cache: %d fi->direct_io: %d\n",
             req, (uintmax_t)ino, (void*)fi->fh, fi->keep_cache, fi->direct_io);
    reply_open(req, fi);
 } CATCH_ERRS

std::pair<uint64_t, str_view>
content_parse_7_2(str_view whole){
     str_view monotonic_validator;
     if(proto_minor < 2)
         throw std::logic_error("You shouldn't call content_parse_7_2 with proto_minor<2");
     auto next = svscan_netstring(whole, &monotonic_validator, 0);
     return {svto<uint64_t>(monotonic_validator), whole.substr(next)};
}

void fs123_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) try {
    stats.reads++;
    atomic_scoped_nanotimer _t(&stats.read_sec);
    if(ino > 1 && ino <= max_special_ino)
        return read_special_ino(req, ino, size, off, fi);
    // FIXME - if fi->fh==0 && enhanced_consistency, it means we were
    // opened with direct_io.  In that case, it might make more sense
    // to skip the chunking and request (almost) exactly the bytes we
    // want.  Otherwise, if somebody does a lot of short reads, we
    // might DoS ourselves by pulling Fs123Chunk*KiB over the network
    // for every read(2).  Maybe something like:
    // if(enhanced_consistency && fi->fh == 0)
    //     return fs123_read_nochunk(req, ino, size, off, fi);

    static constexpr int KiB = 1024;
    auto chunkbytes = Fs123Chunk*KiB;	// most math below is in bytes
    if( size > chunkbytes ){
        // we expect size to be <= FUSE_MAX_PAGES_PER_REQ*PAGE_SIZE
        // which is 32*4096 in modern (2016) kernels.  If wouldn't be
        // hard to write a loop - but before we do, we should probably
        // understand why we're getting such unexpected requests and
        // whether we'd be better off with another strategy.
        throw se(EINVAL, "fs123_read is limited to size<=" + std::to_string(chunkbytes) );
    }
        
    DIAGfkey(_read, "read(req=%p, ino=%ju, size=%zu, off=%jd)\n", req, (uintmax_t)ino, size, (intmax_t)off);
    std::string name;
    uint64_t ino_validator;
    std::tie(name, ino_validator) = ino_to_fullname_validator(ino);
    auto chunknum = off / chunkbytes;
    decltype(chunkbytes) off0 = off%chunkbytes;
    auto len0 = std::min(size, chunkbytes-off0);
    DIAGfkey(_read, "readchunk0(%s, %jd, %zu, %zu, %zu)\n", name.c_str(), (intmax_t)chunknum, chunkbytes, off0, len0);
    auto start0kib = chunknum*Fs123Chunk;
    auto reply0 = begetchunk_file(ino, start0kib);
    if(reply0.eno)
        return reply_err(req, reply0.eno);
    str_view content;
    if(proto_minor >= 2){
        uint64_t rvalidator;
        std::tie(rvalidator, content) = content_parse_7_2(reply0.content);
        if(rvalidator < ino_validator){
            stats.reread_no_cache++;
            // More decisions...
            //
            // If r is not in the swr-window, then there's no choice
            // but to retry with no-cache.
            //
            // If r is in the swr-window (which can only happen if
            // max-stale was unspecified in the original begetchunk),
            // then there's probably already a background refresh
            // "in-flight".  It's probably new enough (but not
            // guaranteed), so if we could wait for that, we'd
            // probably be good.  But unfortunately our diskcache
            // doesn't attach new requests to in-flight refreshes, so
            // we'd have to pause for an indeterminate length of time
            // and hope that the background refresh completes.  It
            // gets very complicated, for a fairly modest bandwidth
            // reduction.
            //
            // The simplest thing to do is to go straight to no-cache.
            // That might waste a little bandwidth, but it's simple
            // and correct.
            reply0 = begetchunk_file(ino, start0kib, true/*no_cache*/);
            if(reply0.eno)
                return reply_err(req, reply0.eno);
            std::tie(rvalidator, content) = content_parse_7_2(reply0.content);
            if(rvalidator < ino_validator){
                stats.non_monotonic_validators++;
                throw se(ESTALE, "fs123_read:  monotonic_validator in the past even after no_cache retrieval fullname: " + name + " r_validator: " + std::to_string(rvalidator) + " ino_validator: " + std::to_string(ino_validator));
            }
        }
        // Wake up the openfile machinery if rvalidator implies that kernel caches are stale.
        if(rvalidator > ino_validator && fi->fh && enhanced_consistency)
            openfile_expire_now(ino, fi->fh);
    }else{
        content = reply0.content;
    }
    
    DIAGfkey(_read, "reply0: size=%zu\n", content.size());
    struct iovec iovecs[2];
    bool shortread = (content.size() < chunkbytes);
    if(off0 > content.size()){
        // We're probably reading more than one chunk past the end of
        // the file.  It's perfectly legal, so a warning may be
        // a bit panicy, but in practice, we've only seen it when
        // something was misbehaving.
        complain(LOG_WARNING, "fs123_read(ino=%ju (%s), size=%zu, off=%jd) got only content.size()=%zu bytes in reply for chunk at start0kib=%ju.  Return 0 bytes (EOF)\n",
                 (uintmax_t)ino, name.c_str(), size, (intmax_t)off, content.size(), (uintmax_t)start0kib);
        return reply_buf(req, nullptr, 0);
    }
    iovecs[0].iov_base = const_cast<char *>(content.data()) + off0;
    iovecs[0].iov_len = std::min(len0, content.size()-off0);
    DIAGfkey(_read, "iov[0]: %lu@%p\n", iovecs[0].iov_len,  iovecs[0].iov_base);
    if( shortread || iovecs[0].iov_len == size ){
        // We're done.  Either we got a short read, indicating EOF,
        // or we've satisfied the request for 'size' bytes.
        stats.bytes_read += iovecs[0].iov_len;
        DIAGfkey(_read, "read(%p, %ju) -> iovecs[1]{len=%zd}\n", req, (uintmax_t)ino, iovecs[0].iov_len);
        return reply_iov(req, iovecs, 1);
    }
    auto nleft = size - len0;
    //  Not done yet.  Request the next chunk:
    DIAGfkey(_read, "readchunk1(%s, %jd, %zu, %zu, %zu)\n", name.c_str(), (intmax_t)chunknum+1, chunkbytes, size_t(0), nleft);
    // FIXME - can we somehow do this read in parallel
    // with the other one?  We've got a threadpool for
    // use in dirent.  Can we reuse that??
    auto start1kib = (chunknum+1)*Fs123Chunk;
    auto reply1 = begetchunk_file(ino, start1kib);
    if(reply1.eno)
        return  reply_err(req, reply1.eno);
    if(proto_minor >= 2){
        uint64_t rvalidator;
        std::tie(rvalidator, content) = content_parse_7_2(reply1.content);
        if(rvalidator < ino_validator){
            stats.reread_no_cache++;
            reply1 = begetchunk_file(ino, start0kib, true/*no_cache*/);
            if(reply1.eno)
                return reply_err(req, reply1.eno);
            std::tie(rvalidator, content) = content_parse_7_2(reply1.content);
            if(rvalidator < ino_validator){
                stats.non_monotonic_validators++;
                throw se(ESTALE, "fs123_read:  monotonic_validator in the past even after no_cache retrieval fullname: " + name + " r_validator: " + std::to_string(rvalidator) + " ino_validator: " + std::to_string(ino_validator));
            }
        }
        // Wake up the openfile machinery if rvalidator implies that kernel caches are stale.
        if(rvalidator > ino_validator && fi->fh && enhanced_consistency)
            openfile_expire_now(ino, fi->fh);
    }else{
        content = reply1.content;
    }
    auto len1 = std::min(nleft, content.size());
    iovecs[1].iov_base = const_cast<char*>(content.data());
    iovecs[1].iov_len = len1;
    DIAGfkey(_read, "iov[1]: %lu@%p\n", iovecs[1].iov_len,  iovecs[1].iov_base);
    stats.bytes_read += len0+len1;
    DIAGfkey(_read, "read(%p, %ju) -> iovecs[2]{len0=%zd, len1=%zd} \n", req, (uintmax_t)ino, len0, len1);
    return reply_iov(req, iovecs, 2);
 } CATCH_ERRS

void fs123_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) try {
    DIAGfkey(_open, "release(%ju, %p)\n", (uintmax_t)ino, fi);
    stats.releases++;
    if(ino > 1 && ino <= max_special_ino)
        return release_special_ino(req, ino, fi);
    if(enhanced_consistency && fi->fh)
        openfile_release(ino, fi->fh);
    reply_err(req, 0);
 } CATCH_ERRS

void fs123_statfs(fuse_req_t req, fuse_ino_t ino) try {
    auto reply = begetstatfs(ino);
    if( reply.eno ){
         // somebody/something is confused.  Report
         // an error and hope somebody notices the complaints.
         return reply_err(req, reply.eno);
    }
    DIAGfkey(_special, "statvfs content: %s\n", reply.content.c_str());
    auto svb = svto<struct statvfs>(reply.content);
    return reply_statfs(req, &svb);
}CATCH_ERRS

void do_forget(fuse_ino_t ino, uint64_t nlookup){
    DIAGfkey(_lookup, "forget(%ju, %ju)\n", (uintmax_t)ino, (uintmax_t)nlookup);
    if(ino > 1 && ino <= max_special_ino)
	return forget_special_ino(ino, nlookup);
    ino_forget(ino, nlookup);
}     

#if !HAS_FORGET_MULTI
// it wasn't defined in fuse_lowelevel.h.  Define it now.
struct fuse_forget_data{
    uint64_t ino;
    uint64_t nlookup;
};
#endif

void fs123_forget_multi(fuse_req_t req, size_t count, struct fuse_forget_data *forgets){
    stats.forget_inos += count;
    stats.forget_calls++;
    while(count--){
        do_forget(forgets->ino, forgets->nlookup);
        forgets++;
    }
    reply_none(req);
}

void fs123_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup){
    fuse_forget_data ffd;
    ffd.ino = ino;
    ffd.nlookup = nlookup;
    fs123_forget_multi(req, 1, &ffd); // will call reply_none
}

void fs123_ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void *arg, struct fuse_file_info */*fi*/, unsigned /*flags*/, 
                 const void *in_buf, size_t in_bufsz, size_t /*out_bufsz*/) try {
    // Maybe we should designate a 'special' name and ino for fiddling
    // with diag_names, and move the code over to special_ino.cpp??

    // The only ioctls we handle have up-to-128-byte NUL-terminated
    // character strings as arguments.  We replace the 128'th byte
    // with a NUL (whether it needs it or not) and pass the string to
    // downstream code.
    
    stats.ioctls++;
    DIAGfkey(_ioctl, "ioctl(ino=%ju, cmd=%d, arg=%p)\n", (uintmax_t)ino, cmd, arg);

    // N.B.  It's tempting to try to implement an FS_IOC_GETVERSION
    // which could simply return the ino.  It's not particularly
    // helpful because fuse (at least the version on CentOS7) won't
    // let us do ioctl's on directories.  (See FUSE_CAP_IOCTL_DIR in
    // the libfuse tree).  Also, it's not particularly useful because
    // we can just say fs123p7exportd --estale-cookie-src=st_ino with
    // the same effect, and no system call.

    // Even with -odefault_permissions, users with only
    // read-permission are permitted to invoke ioctl's.  The ioctl's
    // here change the "state" of the filsystem, so we restrict them
    // to a specific 'special' ino.  The permissions on that ino
    // (e.g., 0400 with st_uid = the geteuid() of the daemon process)
    // limit who can perform fs-wide ioctls.
    if(ino != SPECIAL_INO_IOCTL)
        return reply_err(req, EPERM);
    
    // Boilerplate for changing the value of something in volatiles
    // from an ioctl:
#define VOLATILE_IOCTL(NAME)  \
        if( in_bufsz != sizeof(fs123_ioctl_data) ) \
            throw se(EINVAL, "Wrong size for ioctl"); \
        rdo = (fs123_ioctl_data*)in_buf; \
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0'; \
        volatiles->NAME.store(svto<decltype(volatiles->NAME.load())>(rdo->buf)); \
        complain(LOG_NOTICE, str("changed " #NAME " to:", volatiles->NAME.load())); \
        fuse_reply_ioctl(req, 0, nullptr, 0); \

    fs123_ioctl_data *rdo;
    switch(unsigned(cmd)){
        // FIXME - there's A LOT of repeated boilerplate here!
#ifdef TCGETS
    case TCGETS:
        // Ok - we've learned that we see an ioctl(TCGETS) whenever
        // the garden 'read's a file.  No need to alert syslog every time...
        return reply_err(req, ENOSYS);
#endif
    case DIAG_NAMES_IOC:
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL,  "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        set_diag_names(rdo->buf, /*clear_before_set*/ false);
        complain(LOG_NOTICE, "turning on diagnostic key: %s", rdo->buf);
        fuse_reply_ioctl(req, 0, nullptr, 0);
#if defined(INTENTIONAL_HEAP_CORRUPTION)
        if(startswith(rdo->buf, "CAUSE_FATAL_buffer_overrun")){
            complain(LOG_ERR, "Intentionally writing past the end of a malloced block!");
            volatile char *p = (volatile char*)::malloc(9999);
            p[9999] ^= 'x';
            complain(LOG_ERR, "The heap is now dangerously corrupted by a buffer overrun near address %p!", p+9999);
        }
        if(startswith(rdo->buf, "CAUSE_FATAL_buffer_underrun")){
            complain(LOG_ERR, "Intentionally writing before the beginning of a malloced block!");
            volatile char *p = (volatile char*)::malloc(9999);
            p[-1] ^= 'x';
            complain(LOG_ERR, "The heap is now dangerously corrupted by a buffer underrun near address %p!", p);
        }
        if(startswith(rdo->buf, "CAUSE_FATAL_double_free")){
            complain(LOG_ERR, "Intentionally double-freeing a pointer!");
            volatile char *p = (volatile char*)::malloc(9999);
            ::free((void *)p);
            ::free((void *)p);
            complain(LOG_ERR, "The heap is now dangerously corrupted by a double-free of %p!", p);
        }
        if(startswith(rdo->buf, "CAUSE_FATAL_undefined_behavior")){
            complain(LOG_ERR, "Intentionally cause a signed integer overflow!");
            int i = std::numeric_limits<int>::min();
            int j = -i;
            complain(LOG_ERR, "v = %d (as a result of integer overflow)!", j);
        }
#endif
        return;
    case DIAG_OFF_IOC:
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL,  "Wrong size for ioctl");
        set_diag_names("");
        complain(LOG_NOTICE, "turned off all diagnostics");
        fuse_reply_ioctl(req, 0, nullptr, 0);
        return;
    case CONNECT_TIMEOUT_IOC:
        VOLATILE_IOCTL(connect_timeout);
        return;
    case TRANSFER_TIMEOUT_IOC:
        VOLATILE_IOCTL(transfer_timeout);
        return;
    case LOAD_TIMEOUT_FACTOR_IOC:
        VOLATILE_IOCTL(load_timeout_factor);
        return;
    case NAMECACHE_IOC:
        VOLATILE_IOCTL(namecache);
        return;
    case STALE_IF_ERROR_IOC:
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL, "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        req123::default_stale_if_error.store(svto<int>(rdo->buf));
        complain(LOG_NOTICE, "changed Fs123StaleIfError=%s", rdo->buf);
        fuse_reply_ioctl(req, 0, nullptr, 0);
        return;
    case PAST_STALE_WHILE_REVALIDATE_IOC:
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL, "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        req123::default_past_stale_while_revalidate.store(svto<int>(rdo->buf));
        complain(LOG_NOTICE, "changed Fs123PastStaleWhileRevalidate=%s", rdo->buf);
        fuse_reply_ioctl(req, 0, nullptr, 0);
        return;
    case DISCONNECTED_IOC:
        VOLATILE_IOCTL(disconnected);
        return;
    case CACHE_TAG_IOC:
        {
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL, "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        req123::cachetag.store(svto<unsigned long>(rdo->buf));
        complain(LOG_NOTICE, "changed CacheTag=%lu", req123::cachetag.load());
        fuse_reply_ioctl(req, 0, nullptr, 0);
        }
        return;
    case RETRY_TIMEOUT_IOC:
        VOLATILE_IOCTL(retry_timeout);
        return;
    case RETRY_INITIAL_MILLIS_IOC:
        VOLATILE_IOCTL(retry_initial_millis);
        return;
    case RETRY_SATURATE_IOC:
        VOLATILE_IOCTL(retry_saturate);
        return;
    case IGNORE_ESTALE_MISMATCH_IOC:
        VOLATILE_IOCTL(ignore_estale_mismatch);
        return;
    case EVICT_LWM_IOC:
        VOLATILE_IOCTL(evict_lwm);
        return;
    case EVICT_TARGET_FRACTION_IOC:
        VOLATILE_IOCTL(evict_target_fraction);
        return;
    case EVICT_THROTTLE_LWM_IOC:
        VOLATILE_IOCTL(evict_throttle_lwm);
        return;
    case EVICT_PERIOD_MINUTES_IOC:
        VOLATILE_IOCTL(evict_period_minutes);
        return;
    case DC_MAXMBYTES_IOC:
        VOLATILE_IOCTL(dc_maxmbytes);
        return;
    case DC_MAXFILES_IOC:
        VOLATILE_IOCTL(dc_maxfiles);
        return;
    case DIAG_DESTINATION_IOC:
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL, "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        diag_destination = rdo->buf;
        set_diag_destination(diag_destination);
        the_diag().opt_tstamp = !startswith(diag_destination, "%syslog");
        complain(LOG_NOTICE, "changed Fs123DiagDestination=" + diag_destination);
        fuse_reply_ioctl(req, 0, nullptr, 0);
        return;
    case LOG_DESTINATION_IOC:
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL, "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        log_destination = rdo->buf;
        set_complaint_destination(log_destination, 0666);
        if(!startswith(log_destination, "%syslog"))
            start_complaint_delta_timestamps();
        else
            stop_complaint_delta_timestamps();   
        complain(LOG_NOTICE, "changed Fs123LogDestination=" + log_destination);
        fuse_reply_ioctl(req, 0, nullptr, 0);
        return;
    case CURL_MAXREDIRS_IOC:
        VOLATILE_IOCTL(curl_maxredirs);
        return;
    case LOG_MAX_HOURLY_RATE_IOC:
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL, "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        set_complaint_max_hourly_rate(svto<float>(rdo->buf));
        complain(LOG_NOTICE, "changed Fs123LogMaxHourlyRate=%s", rdo->buf);
        fuse_reply_ioctl(req, 0, nullptr, 0);
        return;
    case ADD_PEER_IOC:
        if(!distrib_cache_be)
            throw se(EINVAL, "ioctl(ADD_PEER_IOC, ...): No distributed cache");
        {
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL, "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        // Expect input of the form:
        //  UUID baseurl
        // where there is exactly one space between the UUID and the
        // baseurl.
        distrib_cache_be->suggest_peer(rdo->buf);
        fuse_reply_ioctl(req, 0, nullptr, 0);
        }
        return;
    case REMOVE_PEER_IOC:
        if(!distrib_cache_be)
            throw se(EINVAL, "ioctl(REMOVE_PEER_IOC, ...): No distributed cache");
        {
        if( in_bufsz != sizeof(fs123_ioctl_data) )
            throw se(EINVAL, "Wrong size for ioctl");
        rdo = (fs123_ioctl_data*)in_buf;
        rdo->buf[ sizeof(rdo->buf)-1 ] = '\0';
        distrib_cache_be->discourage_peer(rdo->buf);
        fuse_reply_ioctl(req, 0, nullptr, 0);
        }
        return;
    default:
        // We could just ignore these, but it can be
        // instructive/surprising to see ioctls getting called on
        // remote files.  One common one is cmd=5401 = TCGETS, which
        // the shell apparently uses when redirecting input into
        // 'read', e.g., as in the 'garden load'
        return reply_err(req, ENOSYS);
    }
} CATCH_ERRS

// Locks - NO! NO! NO!  But if we don't implement the callbacks,
// the kernel assumes that it can handle locks locally, which is
// highly misleading.  I.e., in the absence of these callbacks,
//   flock(open("/some/file/in/fs123", O_RDONLY), LOCK_EX)
// returns 0, and even "works" with respect to other processes
// on the same machine that might try to flock the same file.
// But it silently doesn't work with respect to flock calls
// on other hosts.
//
// We could plausibly set errno to ENOSYS, ENOLCK, EROFS, or EIO here.
// ENOSYS is the same as not having callbacks at all.  Our primary
// "customer" for locks is sqlite.  It recognizes ENOLCK, and
// complains that the 'database is locked'.  It doesn't distinguish
// between EROFS and EIO - both result in 'disk I/O error'.  These are
// reasonable, but if we want sqlite to "work", we either have to use
// ENOSYS, or, equivalently, not implement the callbacks at all.  See
// Fs123LocalLocks for the mechanism for not setting the callbacks at
// all.
void fs123_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info*,  struct flock*, int /*sleep*/) try {
    return reply_err(req, ENOLCK);
}CATCH_ERRS

void fs123_getlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info*, struct flock*) try{
    return reply_err(req, ENOLCK);
 }CATCH_ERRS;

} // namespace <anon>

// A couple of symbols that  we use elsewhere in the code:
// declared in mount.fs123p7.hpp
//

/*static*/ const unsigned
volatiles_t::hw_concurrency = std::thread::hardware_concurrency();

reply123 begetattr_fresh(fuse_ino_t ino){
    return begetattr(ino, 0);
}

uint64_t
validator_from_a_reply(const reply123& r) try {
    if(r.eno)
        return 0;
    if(r.content_encoding != content_codec::CE_IDENT)
        throw se(EINVAL, "reply should have been decoded before calling validator_from_a_reply");
    if(proto_minor >= 1){
        // Use the fact that there's a newline between the sb and the
        // validator to avoid fully parsing the sb.
        auto newlineidx = r.content.find('\n');
        stats.validator_scans++;
        return svto<uint64_t>(r.content, newlineidx);
    }else{
        // with 7.0, parse the whole thing and return the .second
        return pair_from_a_reply(r.content).second;
    }
 }catch(std::exception&){
    std::throw_with_nested(std::runtime_error("validator_from_a_reply"));
 }

// begetserver_stats is in the global namespace so we can call it from special_ino.cpp
reply123 begetserver_stats(fuse_ino_t ino){
    reply123 ret;
    req123 req = req123::statsreq();
    berefresh(ino, req, &ret, false);
    return ret;
}

std::ostream& vm_report(std::ostream& os){
    std::string ret;
    std::ostringstream procpidstatus;
    procpidstatus << "/proc/" << getpid() << "/status";
    std::ifstream ifs(procpidstatus.str().c_str());
    std::string line;
    while( getline(ifs, line) ){
        if( core123::startswith(line, "Vm") )
            os << line << "\n";
    }
    return os;
}

std::ostream& report_stats(std::ostream& os){
    stats.elapsed_sec = elapsed_asnt.elapsed();
    os << stats;
    if(diskcache_be)
        diskcache_be->report_stats(os);
    if(http_be)
        http_be->report_stats(os);
    if(distrib_cache_be)
        distrib_cache_be->report_stats(os);
    if(enhanced_consistency)
        os << openfile_report();
    if(secret_mgr)
        secret_mgr->report_stats(os);
    content_codec::report_stats(os);
    os << "syslogs_per_hour: " << get_complaint_hourly_rate() << "\n";
    os << "inomap_size: " << ino_count() << "\n";
    os << "attrcache_size: " << attrcache->size() << "\n"
       << "attrcache_evictions: " << attrcache->evictions() << "\n"
       << "attrcache_hits: " << attrcache->hits() << "\n"
       << "attrcache_expirations: " << attrcache->expirations() << "\n"
       << "attrcache_misses: " << attrcache->misses() << "\n";
    os << "linkmap_size: " << linkmap->size() << "\n"
       << "linkmap_evictions: " << linkmap->evictions() << "\n"
       << "linkmap_hits: " << linkmap->hits() << "\n"
       << "linkmap_expirations: " << linkmap->expirations() << "\n"
       << "linkmap_misses: " << linkmap->misses() << "\n"
       << fuseful_report()
       << vm_report;
    return os;
}

std::ostream& report_config(std::ostream& os){
    // FIXME: The option-parsing/environment-parsing/ioctl-handling
    // code grew organically and haphazardly.  It's conveninent during
    // early development to be able to put an envto<...>(...) pretty
    // much anywhere in the code and get some runtime configurability,
    // but now that we're putting more and more of the configuration
    // under ioctl control, reporting is a headache, and names, values
    // and defaults are dispersed in too many places.  It needs an
    // overhaul.
    os << "pid: " << getpid() << "\n"
       << "baseurl: " << baseurl << "\n"
       << "mountpoint: " << g_mountpoint << "\n"
       << "executable_path: " << executable_path << "\n"
       << "hardware_concurrency: " << volatiles->hw_concurrency << "\n"
       << "load_average: " << volatiles->load_average << "\n"
       << "Fs123Rundir: " << apath() << "\n"
       << "Fs123LogDestination: " << log_destination << "\n"
       << "Fs123DiagNames: " << get_diag_names(true) << "\n"
       << "Fs123DiagDestination: " << diag_destination << "\n"
       << "Fs123TruncateTo32BitIno: " << (st_ino_mask != ~fuse_ino_t(0)) << "\n"
       << "Fs123Chunk: " << Fs123Chunk << "\n"
       << "Fs123CacheDir: " << cache_dir << "\n"
       << "Fs123CacheMaxMBytes: " << volatiles->dc_maxmbytes << "\n"
       << "Fs123CacheMaxFiles: " << volatiles->dc_maxfiles << "\n"
       << "Fs123EvictLwm: " << volatiles->evict_lwm << "\n"
       << "Fs123EictTargetFraction: " << volatiles->evict_target_fraction << "\n"
       << "Fs123EvictThrottleLWM: " << volatiles->evict_throttle_lwm << "\n"
       << "Fs123EvictPeriodMinutes" << volatiles->evict_period_minutes << "\n"
       << "Fs123StaleIfError: " << req123::default_stale_if_error << "\n"
       << "Fs123PastStaleWhileRevalidate: " << req123::default_past_stale_while_revalidate << "\n"
       << "Fs123PrivilegedServer: " << privileged_server << "\n"
       << "Fs123Sharedkeydir: " << sharedkeydir_name << "\n"
       << "Fs123SharedkeydirRefresh: " << sharedkeydir_refresh << "\n"
       << "Fs123EncodingKeyidFile: " << encoding_keyid_file << "\n"
       << "Fs123EncryptRequests: " << encrypt_requests << "\n"
       << "Fs123RetryTimeout: " << volatiles->retry_timeout << "\n"
       << "Fs123RetryInitialMillis: " << volatiles->retry_initial_millis << "\n"
       << "Fs123RetrySaturate: " << volatiles->retry_saturate << "\n"
       << "Fs123IgnoreEstaleMismatch: " << volatiles->ignore_estale_mismatch << "\n"
       << "Fs123SupportXattr: " << support_xattr << "\n"
       << "Fs123ConnectTimeout: " << volatiles->connect_timeout << "\n"
       << "Fs123TransferTimeout: " << volatiles->transfer_timeout << "\n"
       << "Fs123LoadTimeoutFactor " << volatiles->load_timeout_factor << "\n"
       << "Fs123NameCache: " << volatiles->namecache << "\n"
       << "Fs123Disconnected: " << volatiles->disconnected << "\n"
       << "Fs123NoKernelDataCaching: " << no_kernel_data_caching << "\n"
       << "Fs123NoKernelAttrCaching: " << no_kernel_attr_caching << "\n"
       << "Fs123NoKernelDentryCaching: " << no_kernel_dentry_caching << "\n"
       << "Fs123CacheTag: " << req123::cachetag << "\n"
       << "Fs123CurlMaxRedirs: " << volatiles->curl_maxredirs << "\n"
       << "Fs123LogMaxHourlyRate: " << get_complaint_max_hourly_rate() << "\n"
       << "Fs123LogRateWindow: " << get_complaint_averaging_window() << "\n"
        ;
    
    // These names match the list passed to fuse_option_to_envvars.
    // The default values are copied from the point in the code at which
    // the envto is called.  CHANGING DEFAULTS REQUIRES UPDATING BOTH
    // LOCATIONS!!
    // Also note - toString formats bools as "0" and "1", and knows
    // nothing about whether oss has 'boolalpha' set.
#define Prt(Name, dflt) << #Name << ": " << (getenv(#Name) ? getenv(#Name) : str(dflt))  << "\n"
    os
        //Prt(Fs123DiagNames)
        //Prt(Fs123DiagDestination)
        //Prt(Fs123TruncateTo32BitIno)
        Prt(Fs123FallbackUrls, "<unset>")
        Prt(Fs123ProtoMinor, fs123_protocol_minor_max)
        //Prt(Fs123LogDestination, "%stderr")
        Prt(Fs123CommandPipe, "<unset>")
        Prt(Fs123Subprocess, "<unset>")
        Prt(Fs123LogMinLevel, "LOG_INFO")
        //Prt(Fs123Chunk)
        Prt(Fs123LocalLocks, "false")
        //Prt(Fs123Rundir)
        Prt(Fs123BuggyAutomountWorkaround, "false")
        Prt(Fs123Nice, 0)
        Prt(Fs123AttrCacheSize, 100000)
        Prt(Fs123LinkCacheSize, 10000)
        //Prt(Fs123StaleIfError)
        //Prt(Fs123PrivilegedServer)
        //Prt(Fs123IgnoreEstaleMismatch)
        // Retry configuration
        //Prt(Fs123RetryTimeout)
        //Prt(Fs123RetryInitialMillis)
        //Prt(Fs123RetrySaturate)
        Prt(Fs123EnhancedConsistency, "true")
        Prt(Fs123SignalFile, "fs123signal")
#ifdef M_ARENA_MAX
        Prt(Fs123MallocArenaMax, 0)
#endif
        // In backend123
        Prt(Fs123SO_RCVBUF, 1024 * 24)
        Prt(Fs123SSLNoVerifyPeer, "<unset>") // default in backend123_http.cpp
        Prt(Fs123SSLNoVerifyHost, "<unset>") // default in backend123_http.cpp
        //Prt(Fs123TransferTimeout)
        //Prt(Fs123ConnectTimeout)
        //Prt(Fs123LoadTimeoutFactor)
        //Prt(Fs123NameCache)
        //Prt(Fs123Disconnected)
        Prt(Fs123NetrcFile, "<unset>")       // default in backend123_http.cpp
        //Prt(Fs123CacheTag)
        //Prt(Fs123CurlMaxRedirs)
        // In diskcache:
        //Prt(Fs123CacheDir)
        Prt(Fs123DistribCacheExperimental, "false")// default in distrib_cache_backend.cpp
        Prt(Fs123DistribCacheReflector, "<unset>") // default in distrib_cache_backend.cpp
        Prt(Fs123DistribCacheMulticastLoop, "false")// default in distrib_cache_backend.cpp
        //Prt(Fs123PastStaleWhileRevalidate)
        //Prt(Fs123CacheMaxMBytes)
        //Prt(Fs123CacheMaxFiles)
        Prt(Fs123CacheFancySharing, "false")
        //Prt(Fs123EvictLwm, "0.7")        // default in diskcache.cpp
        //Prt(Fs123EvictTargetFraction, "0.8")  // default in diskcache.cpp
        //Prt(Fs123EvictThrottleLWM, "0.9") // default in diskcache.cpp
        //Prt(Fs123EvictPeriodMinutes, 60)// default in diskcache.cpp
        Prt(Fs123RefreshThreads, 10)    // default in diskcache.cpp
        Prt(Fs123RefreshBacklog, 10000)    // default in diskcache.cpp
        Prt(Fs123ForegroundSerialize, "true") // default in diskcache.cpp
        // env-vars with conventional meaning to libcurl
        // can be set on the command line.
        // Note that http{s}_proxy distinguish between being
        // unset and being set to the empty string.  Beware!
        Prt(http_proxy, "<unset>")
        Prt(https_proxy, "<unset>")
        // Hacks to run under valgrind
        Prt(Fs123Trampoline, "")
        ;
    if(distrib_cache_be){
        os << "distrib_cache_uuid: " << diskcache_be->get_uuid() <<  "\n"
           << "distrib_cache_server: " << distrib_cache_be->get_url() << "\n";
    }
    return os;
}

int app_mount(int argc, char *argv[])
try {
    // The initializations left in main are the ones that must
    // (or benefit from) being done before starting fuse,
    // e.g., diagnositics.
    const char *syslog_name = "fs123";
    fuse_args args = FUSE_ARGS_INIT(argc, argv);
    fuse_options_to_envvars(&args, 
        "fs123p7 - a read-only caching fuse filesystem"
        "\n"
        "Positional arguments are:\n"
        "    http[s]://hostname[:port]/SEL/EC/TOR\n"
        "    /path/to/mountpoint\n"
        "\n"
        "You can configure filesystems of this type with a line like this in fstab:\n"
        "\n"
        "http[s]://hostname[:port]/SEL/EC/TOR /path/to/mountpoint fs123p7 allow_other,OptionName=value 0 0\n",
                            {
                                "Fs123DiagNames=",
                                    "Fs123DiagDestination=",
                                    "Fs123TruncateTo32BitIno=",
                                    "Fs123FallbackUrls=",
                                    "Fs123ProtoMinor=",
                                    "Fs123LogDestination=",
                                    "Fs123LogMinLevel=",
                                    "Fs123LogMaxHourlyRate=",
                                    "Fs123LogRateWindow=",
                                    "Fs123CommandPipe=",
                                    "Fs123Subprocess=",
                                    "Fs123Chunk=",
                                    "Fs123LocalLocks=",
                                    "Fs123Rundir=",
                                    "Fs123BuggyAutomountWorkaround=",
                                    "Fs123Nice=",
                                    "Fs123AttrCacheSize=",
                                    "Fs123LinkCacheSize=",
                                    "Fs123StaleIfError=",
                                    "Fs123PrivilegedServer=",
                                    "Fs123Sharedkeydir=",
                                    "Fs123SharedkeydirRefresh=",
                                    "Fs123AllowUnencryptedReplies",
                                    "Fs123EncodingKeyidFile=",
                                    "Fs123EncryptRequests=",
                                    "Fs123IgnoreEstaleMismatch=",
				    "Fs123SupportXattr=",
                                    "Fs123EnhancedConsistency=",
                                    "Fs123SignalFile=",
#ifdef M_ARENA_MAX
                                    "Fs123MallocArenaMax=",
#endif
                                    // Retry configuration
                                    "Fs123RetryTimeout=",
                                    "Fs123RetryInitialMillis=",
                                    "Fs123RetrySaturate=",
                                    // In backend123
                                    "Fs123SO_RCVBUF=",
                                    "Fs123SSLNoVerifyPeer=",
                                    "Fs123SSLNoVerifyHost=",
                                    "Fs123TransferTimeout=",
                                    "Fs123ConnectTimeout=",
                                    "Fs123LoadTimeoutFactor=",
                                    "Fs123NameCache=",
                                    "Fs123Disconnected=",
                                    "Fs123NoKernelDataCaching=",    // Debug/diagnostic only.  Will kill performance.
                                    "Fs123NoKernelAttrCaching=",// Debug/diagnostic only.  Will kill performance.
                                    "Fs123NoKernelDentryCaching=",// Debug/diagnostic only.  Will kill performance.
                                    "Fs123NetrcFile=",
                                    "Fs123CacheTag=",
                                    // In diskcache:
                                    "Fs123CacheDir=",
                                    "Fs123PastStaleWhileRevalidate=",
                                    "Fs123CacheMaxMBytes=",
                                    "Fs123CacheMaxFiles=",
                                    "Fs123CacheFancySharing=",
                                    "Fs123EvictLwm=",
                                    "Fs123EvictTargetFraction=",
                                    "Fs123EvictThrottleLWM=",
                                    "Fs123EvictPeriodMinutes=",
                                    "Fs123RefreshThreads=",
                                    "Fs123RefreshBacklog=",
                                    "Fs123ForegroundSerialize=",
                                    // In distrib_cache_backend:
                                    "Fs123DistribCacheExperimental=",
                                    "Fs123DistribCacheReflector=",
                                    "Fs123DistribCacheMulticastLoop=",
                                    // env-vars with conventional meaning to libcurl
                                    // can be set on the command line.
                                    "http_proxy=",
                                    "https_proxy=",
                                    // Hacks to re-exec with env-vars, instrumentation, etc.
                                    "Fs123Trampoline=",
                                    }
                            );

    // The 'trampoline' is the path to a command that will be exece'ed
    // as if you had written in a shell:
    //    exec "$Fs123Trampoline" "$0" "$@"
    //
    // It allows an administrator to create a script that re-execs
    // fs123p7 in a modified environment and/or under a debugger.  This
    // is particularly useful when it's difficult to directly control
    // fs123p7's environment, e.g., when it is started via fstab or
    // autofs.
    std::string trampoline = envto<std::string>("Fs123Trampoline", "");
    if(!trampoline.empty() && !getenv("Fs123AlreadyReExeced")){
        std::vector<const char*> vargs;
        vargs.push_back(trampoline.c_str());
        vargs.insert(vargs.end(), fs123p7_argv, fs123p7_argv+fs123p7_argc);
        vargs.push_back(nullptr);
        ::setenv("Fs123AlreadyReExeced", "1", 1);
        // N.B.  It would be *very bad* if the trampoline were to
        // remove Fs123p7AlreadyExeced from the environment before
        // re-exec-ing.  GARDEN_OVERRIDE_ENV_KEEP_ALWAYS defends
        // against one way that might happen accidentally if the
        // trampoline uses 'env-keep-only' from the 'desres garden'.
        ::setenv("GARDEN_OVERRIDE_ENV_KEEP_ALWAYS", "Fs123AlreadyReExeced", 1);
        execvp(vargs[0], const_cast<char * const *>(vargs.data()));
        std::cerr << "Uh oh.  execvp of trampoline returned:   errno=" << errno << std::endl;
        return 99;
    }

#ifdef M_ARENA_MAX
    // See docs/Notes.Vm
    int arena_max = envto<int>("Fs123MallocArenaMax", 0);
    if(arena_max)
        mallopt(M_ARENA_MAX, arena_max);
#endif
    // It's tempting to do something like:
    //
    // if(getenv("MALLOC_CHECK_")) mcheck(&complain_and_abort);
    //
    // But the heap is corrupted when mcheck's argument is called!
    // Calling complain() might just make it worse.  I think we're
    // best served by letting the default mcheck() handler do its
    // thing: write a message to stderr and abort()).  Even if stderr
    // has been closed, we'll still see the core dump, and, if
    // Fs123SignalFile is enabled, we'll see a backtrace.

    // Work around for buggy automount.  There's a race condition
    // in the Linux automount code that sometimes leaves us with
    // spurious open file descriptors and can result in locking up
    // the whole automount infrastructure.  See:
    //  https://bugzilla.redhat.com/show_bug.cgi?id=1509088
    // Closing these file descriptors is a public service - like
    // picking up somebody else's litter.  They should never have
    // been open in the first place, but by closing them, we make
    // the world a better place...  N.B.  We have to do this
    // before opening any file descriptors "of our own", e.g,
    // before calling openlog.
    if(envto<bool>("Fs123BuggyAutomountWorkaround", false)){
        struct rlimit rl;
        sew::getrlimit(RLIMIT_NOFILE, &rl);
        int rlopen = rl.rlim_cur;
        if(rlopen > 16384){
            fprintf(stderr, "Enough is enough... We're only closing 16k files for Fs123BuggyAutomountWorkaround even though rlim_cur is %d\n",
                    rlopen);
            rlopen = 16384;
        }
        for(int fd=3; fd<rlopen; ++fd){
            if( close(fd) == 0 ) // N.B.  These are expected to fail.  They *should* fail!
                fprintf(stderr, "Closed fd=%d.  Automount probably dodged a bullet.", fd);
        }
    }

    std::string logdestination_default = "%syslog%LOG_USER";
    log_destination = envto<std::string>("Fs123LogDestination", logdestination_default);
    set_complaint_destination(log_destination, 0666);
    if(!startswith(log_destination, "%syslog"))
        start_complaint_delta_timestamps();
    set_complaint_level(syslog_number( envto<std::string>("Fs123LogMinLevel", "LOG_INFO")));
    set_complaint_max_hourly_rate(envto<double>("Fs123LogMaxHourlyRate", 3600.));
    // We want a non-hourly rate_window for debugging, so we can push it over
    // and let it fall back without waiting for a whole hour.  Under normal
    // circumstances, it will always be the default 3600 sec = 1hr.
    set_complaint_averaging_window(envto<double>("Fs123LogRateWindow", 3600.));
    auto nice=envto<int>("Fs123Nice", 0);
    if(nice != 0){
        // N.B.  requires root or CAP_SYS_NICE to lower the priority.
        sew::setpriority(PRIO_PROCESS, 0, nice);
    }

    // N.B.  core123's log_channel doesn't call openlog.  But it
    // *does* explicitly provide a facility every time it calls
    // syslog.  So the third arg to openlog shouldn't matter.  But
    // just in case, glibc's openlog(...,0) leaves the default
    // facility alone if it was previously set, and sets it to
    // LOG_USER if it wasn't.
    openlog(syslog_name, LOG_PID, 0);

    signal_filename = envto<std::string>("Fs123SignalFile", "fs123signal");
    if(!signal_filename.empty())
        signal_filename += "." + str(getpid());

#ifdef __linux__
    executable_path = sew::str_readlink("/proc/self/exe");
#else
    executable_path = fs123p7_argv[0];
#endif
    complain(LOG_NOTICE, "Parent process starting at %.9f",
           std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
    complain(LOG_NOTICE, "Startup: " + executable_path + " " + strbe(argv+1,  argv+argc));
    auto diags = envto<std::string>("Fs123DiagNames", "");
    if(!diags.empty()){
        set_diag_names(diags);
        std::cerr << get_diag_names() << "\n";
    }
    diag_destination = envto<std::string>("Fs123DiagDestination", "%syslog");
    set_diag_destination(diag_destination);
    if(!startswith(diag_destination, "%syslog"))
        the_diag().opt_tstamp = true;
    the_diag().opt_tid = true;

    // If Fs123LocalLocks is zero, then all attempts to lock a file
    // fail with errno=ENOLCK.  This is safe and "correct", but it
    // prevents some applications, e.g., sqlite3, from working at all
    // - even in a "read only" mode.
    //
    // If non-zero, Fs123LocalLocks allows the kernel to handle
    // locking.  The kernel "knows" about locks made on this machine,
    // and does the right thing when local processes use flock or
    // fcntl locks.  But it's oblivious to anything happening on
    // other clients or on the server.  Applications that rely on
    // locks may be badly fooled if the file is locked elsewhere.  On
    // the other hand, the fs123 filesystem is read-only, so if
    // locking is being used to prevent multiple writers, a false "ok,
    // you have the lock" won't do any harm because even if several
    // clients try to write the file, none of them will succeed
    // anyway.  In such cases, Fs123LocalLocks=1 might be a reasonable
    // compromise that allows applications (e.g., sqlite) to operate
    // in read-only mode. 
    //
    // We have to do initialize Fs123LocalLocks and Fs123SupportXattr
    // in main, and not in fs123_init because they affect
    // how we initialize the callbacks.
    auto Fs123LocalLocks = envto<bool>("Fs123LocalLocks",  false);

    // If true, Fs123SupportXattr handles listxattr and getxattr
    // operations, generating "x" requests to the server
    // (protocol version 7 addition)
    support_xattr = envto<bool>("Fs123SupportXattr", false);

    struct fuse_lowlevel_ops fs123_oper = {};
    fs123_oper.init = fs123_init;
    fs123_oper.destroy = fs123_destroy;

    fs123_oper.lookup = fs123_lookup;
    fs123_oper.forget = fs123_forget;
#if HAS_FORGET_MULTI
    fs123_oper.forget_multi = fs123_forget_multi;
#endif

    fs123_oper.getattr   = fs123_getattr;


    if (support_xattr) {
	fs123_oper.getxattr   = fs123_getxattr;
	fs123_oper.listxattr   = fs123_listxattr;
    }

    fs123_oper.readlink = fs123_readlink;

    fs123_oper.open = fs123_open;
    fs123_oper.read = fs123_read;
    fs123_oper.release = fs123_release;

    fs123_oper.readdir = fs123_readdir;
    fs123_oper.opendir = fs123_opendir;
    fs123_oper.releasedir = fs123_releasedir;

    // locks?  No, not really.  See the comments
    // near the implementations.
    if( !Fs123LocalLocks ){
        fs123_oper.getlk = fs123_getlk;
        fs123_oper.setlk = fs123_setlk;
    }

    fs123_oper.ioctl = fs123_ioctl;
    fs123_oper.statfs = fs123_statfs;

    // Unconditionally add some mount-options.  If we ever need to
    // reverse these (unlikely), see the code in that handles subtype=
    // in fuse_options_to_envvars in useful.c
    fuse_opt_add_arg(&args, "-odefault_permissions");
    fuse_opt_add_arg(&args, "-oro");
    fuse_opt_add_arg(&args, "-onoatime");
    fuse_opt_add_arg(&args, ("-ofsname=" + fuse_device_option ).c_str());
    // N.B.  nosuid and nodev are already the defaults for fuse mounts.

    // N.B.  signal_filename has global scope.  It is never
    // modified.  It's safe to give it to fuseful_main_ll, which will
    // use it only if it catches any signals.
    const char *sig_report = (signal_filename.empty()) ? nullptr : signal_filename.c_str();
    int ret = fuseful_main_ll(&args, fs123_oper, sig_report, fs123_crash);
    complain(LOG_NOTICE, str("app_mount:  fuse_main_ll returned", ret, " return from app_mount() at", std::chrono::system_clock::now()));
    if(subprocess){
        complain(LOG_NOTICE, "app_mount:  join-ing subprocess thread.  wait for subprocess to finish");
        subprocess->join();      DIAG(_shutdown, "subprocess joined");
        subprocess.reset();
    }
    return ret;
 }catch(std::exception& e){
    complain(LOG_CRIT, e, "%s:  nested exception caught in main:", argv[0]);
    fuseful_teardown();
    if(subprocess){
        complain(LOG_NOTICE, "app_mount:  join-ing subprocess thread.  wait for subprocess to finish");
        subprocess->join();      DIAG(_shutdown, "subprocess joined");
        subprocess.reset();
    }
    return 1;
 }
