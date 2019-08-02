#include "valgrindhacks.hpp"
#include "diskcache.hpp"
#include "fuseful.hpp"
#include "fs123/stat_serializev3.hpp"
#include "fs123/acfd.hpp"
#include <core123/complaints.hpp>
#include <core123/scoped_nanotimer.hpp>
#include <core123/diag.hpp>
#include <core123/threeroe.hpp>
#include <core123/sew.hpp>
#include <core123/exnest.hpp>
#include <core123/throwutils.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/envto.hpp>
#include <core123/pathutils.hpp>
#include <core123/stats.hpp>
#include <core123/fdstream.hpp>
#include <random>
#include <vector>
#include <utility>
#include <thread>
#include <cmath>

using namespace core123;

static auto _diskcache = diag_name("diskcache");
static auto _evict = diag_name("evict");

namespace{
#define STATS_INCLUDE_FILENAME "diskcache_statistic_names"
#define STATS_STRUCT_TYPENAME diskcache_stats_t
#include <core123/stats_struct_builder>
diskcache_stats_t stats;

inline double log_16(double x){
    return ::log2(x)/4.;
}

 refcounted_scoped_nanotimer_ctrl serialize_nanotimer_ctrl(stats.diskcache_serialize_inuse_sec);
 refcounted_scoped_nanotimer_ctrl deserialize_nanotimer_ctrl(stats.diskcache_deserialize_inuse_sec);
 refcounted_scoped_nanotimer_ctrl update_nanotimer_ctrl(stats.diskcache_update_inuse_sec);
 refcounted_scoped_nanotimer_ctrl serdes_nanotimer_ctrl(stats.diskcache_serdes_inuse_sec);

bool should_serialize(const reply123& r){
    switch(r.eno){
    case 0:
    case ENOENT:
#ifndef __APPLE__
        // FIXME!  DANGER!! - ENODATA is numerically different on Linux
        // and OSX.
        // Linux:  ENODATA=61    EPFNOSUPPORT=96
        //   OSX:  ENODATA=96    ECONNREFUSED=61
        // ECONNREFUSED should definitely *not* be serialized, and
        // on OSX, we can't distinguish between a server-generated
        // ENODATA and a client-generated ECONNREFUSED, so we punt.
        // It may require a protocol change to fix this.
    case ENODATA:
#endif
        // should serialization if there's no
        // error, or if the errno is non-transient and is something
        // that we'd like to negatively cache (e.g., there is no-such
        // entry, there are no such xattrs, etc).
        return true;
    }
    // don't serialize anything else.
    return false;
}

} // end namespace <anon>

void 
diskcache::check_root(size_t Ndirs){
    // FIXME - there should be more checks here!

    // make sure that the directories we need either
    // already exist, or that we successfully create them.
    for(unsigned i=0; i<Ndirs; ++i){
        auto d = reldirname(i);
        struct stat sb;
        auto ret = ::fstatat(rootfd_, d.c_str(), &sb, 0);
        if( ret==0 ){
            if(!S_ISDIR(sb.st_mode))
                throw se(EINVAL, "diskcache::check_root: " + d + " exists but is not a directory");
        }else{
            if(errno == ENOENT){
                if(::mkdirat(rootfd_, d.c_str(), 0700) != 0){
                    if(errno == EEXIST)
                        complain(LOG_WARNING, "Now-you-see-it-now-you-dont cache directory.  This is normal if there are two mount.fs123 processes concurrently creating a cache directory.  If not, your diskcache is probably corrupted.");
                    else
                        throw se("mkdirat(rootfd_," + d + ")");
                }
            }else
                throw se("fstatat(rootfd_, " + d + ")");
        }
    }
}

std::string 
diskcache::reldirname(unsigned i) const{
    char s[32];
    sprintf(s, "%0*x", int(hexdigits_), i);
    return s;
}

scan_result
diskcache::do_scan(unsigned dirnum) const{
    scan_result ret;
    acDIR dp = sew::opendirat(rootfd_, reldirname(dirnum).c_str());
    auto len = offsetof(struct dirent, d_name) + 
        fpathconf(rootfd_, _PC_NAME_MAX) + 1;
    char space[len];
    // Yikes.  readdir_r really is a pain...
    struct dirent* entryp = (struct dirent*)&space[0];
    struct dirent* result;
    while( sew::readdir_r(dp, entryp, &result), result ){
        std::string fname = entryp->d_name;
        struct stat sb;
        DIAGfkey(_evict, "readdir -> %x/%s\n", dirnum, fname.c_str());
        try{
            sew::fstatat(dirfd(dp), fname.c_str(), &sb, 0);
        }catch(std::system_error& se){
            // rethrow everything but ENOENT
            DIAGfkey(_evict, "fstatat threw se.code().value() = %d\n", se.code().value());
            if(se.code().category() != std::system_category() || se.code().value() != ENOENT)
                throw;
            continue;
        }
        if( !S_ISREG(sb.st_mode) ) // skips . and .. along with any other dirs
            continue;
        // count st_blocks*512 rather than st_size.  It's more
        // representative of our actual resource-consumption.  Add
        // 4k because that's what df seems to indicate
        // when we create small files on ext4.  Do NOT add
        // sb.st_blksize, because that has nothing to do with
        // the underlying block size.  st_blksize is 32k on
        // some NFS mounts and 4M(!) on some docker bind mounts.
        ret.nbytes += sb.st_blocks* 512 + 4096;
        ret.names.push_back(fname);

        // Should we gather more info?  E.g., open the file and
        // extract its expiration time??  What about st_atime?
    }
    return ret;
}

void
diskcache::evict(size_t Nevict, size_t dir_to_evict, scan_result& sr, std::default_random_engine& eng){
    // try to evict Nevict files from the current dir_to_evict_.
    // The names (and perhaps other metadata) are in sr.
    std::string pfx = reldirname(dir_to_evict) + "/";
    for(size_t i=0; i<Nevict; ++i){
        // Randomly pick one of the names in the inclusive range [i, size()-1]
        size_t j = std::uniform_int_distribution<size_t>(i, sr.names.size()-1)(eng);
        std::swap( sr.names[i], sr.names[j] );
        auto ret = ::unlinkat(rootfd_, (pfx + sr.names[i]).c_str(), 0);
        if(ret && errno != ENOENT)
            throw se("unlinkat(" + pfx+sr.names[i] + ")");
        // ENOENT is ok.  It means another thread unlinked the file
        // before we got to it.  Rare, but not an error.
        stats.dc_eviction_evicted++;
    }
}

// The diskcache code is intentionally oblivious to external processes
// removing files from (or adding properly named and formatted files
// to) the cache "behind its back".  This makes it possible for
// multiple mount.fs123 processes to share the same active diskcache.
// If they have different baseurls they *must* have different
// hash_seeds (specified in the constructor).  Conversely, if they
// have the same baseurl, they should have the same seed, which will
// allow them to share.
//
// If 'fancy_sharing' was requested at construction, then if more than
// one process is sharing a diskcache, only one of them will be the
// 'custodian' at any one time.  The 'custodian' is responsible for
// actually evicting files *and* for writing the 'status' file in the
// root directory.  The non-custodian caches poll the statusfd_ every
// 10 seconds and adjust their 'injection probability' accordingly.
//
// WARNING: the fancy-sharing code is untested (and off by default).
// WARNING: the fancy-sharing code will misbehave if the diskcache is
//        NFS.  There are at least two problems:
//  1 - flock will return EBADF.  Arguably, this is a "feature"
//      that tells people to stop what they're doing before they
//      hurt themselves.  Don't "fix" this without fixing #2.
//  2 - NFS doesn't promise write-to-read consistency, so there's no
//      telling when one process' read_status will "see" another
//      process' write_status.  This *may* not be a problem if both
//      processes are on the same machine, but who knows...
//  3 - Multiple machines sharing a single diskcache over NFS is
//      crazy-talk.  Don't even think about it.
bool
diskcache::custodian_check(){
    // Once the custodian - always the custodian...
    if(custodian_)
        return true;
    // Try to acquire an exclusive flock on the rootfd_.
    // If it succeeds, we take responsibility for
    // evictions, and hold the flock until rootfd_ is
    // closed by the diskcache destructor.
    if( flock(statusfd_, LOCK_EX|LOCK_NB) == 0 ){
        complain(LOG_NOTICE, "diskcache::custodian_check:  Acquired flock on statusfd.  This process is now in charge of diskcache evictions");
        custodian_ = true;
        return true;
    }
    if(errno != EWOULDBLOCK)
        throw se(errno, "diskcache::custodian_check flock(rootfd_, LOCK_EX|LOCK_NB) failed unexpectedly");
    return false;
}

static constexpr int BUFSZ = 32;  // more than enough for a %.9g
void
diskcache::write_status(float prob) const {
    // N.B.  This is only called in one thread in the process that's
    // the 'custodian' of the statusfd, i.e., when it has an exclusive
    // flock on the statusfd_.  We must defend against torn reads and
    // writes, but we don't have to defend against other processes
    // racing with us to write the file.  The "solution" is to read
    // and write exactly BUFSIZ (32) bytes with single atomic calls to
    // pread and pwrite on statusfd_.  (Note - the diskcache is safe
    // against external processes *removing* files from the rootfd,
    // but it's definitely not safe against random scribbling).
    char buf[BUFSZ] = {}; // fill with NUL
    // FIXME - It's 2019.  Shouldn't we have something more structured
    // than a single %g? Json? INI?
    auto nprt = ::snprintf(buf, BUFSZ, "%g", prob);
    if(nprt < 0 || nprt >= BUFSZ)
        throw se(EINVAL, "diskcache::write_status:  Unexpected snprintf overflow.  This can't happen");
    auto wrote = sew::pwrite(statusfd_, buf, BUFSZ, 0);
    if(wrote != BUFSZ){
        complain(LOG_ERR, "diskcache::write_status:  short write");
        wrote = sew::pwrite(statusfd_, buf, BUFSZ, 0);
        if(wrote != BUFSZ)
            throw se(EINVAL, "diskcache::write_status:  short write");
    }
}

float
diskcache::read_status() {
    char buf[BUFSZ];
    auto nread = sew::pread(statusfd_, &buf[0], BUFSZ, 0);
    if(nread == 0){
        // see comment in ctor where we open statusfd_.
        complain(LOG_WARNING, "Oops.  You have found the tiny window during which the 'custodian' has taken ownership, but hasn't written anything to the statusfile.  This is normal if it happens once but is probably a misconfiguration if it recurs.");
        return injection_probability_.load();
    }
    if(nread != BUFSZ)
        throw se(EINVAL, "diskcache::read_status:  short read");
    return svto<float>({buf, BUFSZ});
}

// evict_loop is the entry-point for the evict_thread.  It loops until
// the 'evict_thread_done_' bool is set and the evict_thread_done_cv_
// is notifiy()'ed, which is done by the diskcache destructor.  It
// sleeps in between loops by an amount that depends on the number of
// directories and whether it's feeling "pressure".
void
diskcache::evict_loop(size_t Ndirs){
    std::unique_lock<std::mutex> lk(evict_done_mtx_);
    std::chrono::duration<double> sleepfor(0);
    size_t dir_to_evict = 0;
    size_t files_evicted = 0;
    size_t files_scanned = 0;
    size_t bytes_scanned = 0;
    std::default_random_engine urng; // not seeded.  Should we care...
    float inj_prob;
    do{
        try{
            if(custodian_check()){
                auto scan = do_scan(dir_to_evict);
                auto Nfiles = scan.names.size();
                float maxfiles_per_dir = float(vols_.dc_maxfiles) / Ndirs;
                float maxbytes_per_dir = float(vols_.dc_maxmbytes)*1000000. / Ndirs;
                double filefraction = Nfiles / maxfiles_per_dir;
                double bytefraction = scan.nbytes / maxbytes_per_dir;
                double usage_fraction = std::max( filefraction, bytefraction );
                size_t Nevict = 0;
                if(usage_fraction > 1.0)
                    complain(LOG_WARNING, "Usage fraction %g > 1.0 in directory: %zx.  Disk cache may be dangerously close to max-size",
                             usage_fraction, dir_to_evict);
                // We don't evict anything until we're above evict_target_fraction.
                if( usage_fraction > vols_.evict_target_fraction ){
                    // We're above evict_target_fraction.  Try to get down to 'evict_lwm'
                    auto evict_fraction = (usage_fraction - vols_.evict_lwm)/usage_fraction;
                    Nevict = clip(0, int(ceil(Nfiles*evict_fraction)), int(scan.names.size()));
                    complain(LOG_NOTICE, "evict %zd files from %zx. In this directory: files: %zu (%g) bytes: %zu (%g)",
                             Nevict, dir_to_evict, Nfiles, filefraction, scan.nbytes, bytefraction);
                }
                evict(Nevict, dir_to_evict, scan, urng);
                files_evicted += Nevict;
                files_scanned += Nfiles;
                bytes_scanned += scan.nbytes;
                // If usage_fraction is above evict_throttle_lwm, we assume we're "under attack".
                // There are two responses:
                //   lower the injection_probability to randomly reject insertion of new objects
                //   lower the interval between directory scans.
                inj_prob = clip(0., (1. - usage_fraction)/(1. - vols_.evict_throttle_lwm), 1.);
                DIAGfkey(_evict, "Injection probability set to %g after scanning directory %zx.  filefraction=%g, bytefraction=%g, Nevict=%zd",
                         inj_prob, dir_to_evict, filefraction, bytefraction, Nevict);
                if(statusfd_) // i.e., we really are fancy_sharing.
                    write_status(inj_prob);

                sleepfor = std::chrono::minutes(vols_.evict_period_minutes);
                sleepfor *= inj_prob / Ndirs;
                
                if(++dir_to_evict >= Ndirs)
                    dir_to_evict = 0;
                if(dir_to_evict == 0){
                    complain((files_evicted == 0)? LOG_INFO : LOG_NOTICE, "evict_loop:  completed full scan.  Found %zd files of size %zd.  Evicted %zd files", 
                             files_scanned, bytes_scanned, files_evicted);
                    files_evicted = 0;
                    files_scanned = 0;
                    bytes_scanned = 0;
                }
                stats.dc_eviction_dirscans++;
            }else{
                inj_prob = read_status();
                sleepfor = std::chrono::seconds(10);
            }
            if(inj_prob < 1.0)
                complain(LOG_NOTICE, "Injection probability set to %g", inj_prob);
            else if(injection_probability_ < 1.0)
                complain(LOG_NOTICE, "Injection probability restored to 1.0");
            injection_probability_.store(inj_prob);
        }catch(std::exception& e){
            // We're the thread's entry point, so the buck stops here.
            // If we rethrow, we terminate the program, which seems "harsh".
            // Our only choice is to hope that somebody notices the complain(LOG_ERR,...)
            // OTOH, let's avoid thrashing by waiting at least 5 minutes before
            // trying again.
            sleepfor = clip(std::chrono::minutes(5), sleepfor, sleepfor);
            injection_probability_.store(0.);
            complain(e, "exception thrown in evict_loop.  diskcache injection disabled.  Will try again in %.0f seconds", sleepfor.count());
        }
    }while(!evict_thread_done_cv_.wait_for(lk, sleepfor, [this](){return evict_thread_done_;}));
    complain(LOG_NOTICE, "evict_loop thread exiting.");
}
    
diskcache::diskcache(std::unique_ptr<backend123> upstream, const std::string& root,
                     uint64_t hash_seed_first, bool fancy_sharing, volatiles_t& vols) :
    backend123(upstream?upstream->get_disconnected():false),
    upstream_(std::move(upstream)),
    injection_probability_(1.0),
    evict_thread_done_(false),
    hashseed_(hash_seed_first, 0),
    vols_(vols)
{
    // We made injection_probability_ std:atomic<float> so we wouldn't
    // have to worry about it getting ripped or torn.  But helgrind
    // worries anyway.  Tell it to stop.
    VALGRIND_HG_DISABLE_CHECKING(&injection_probability_, sizeof(injection_probability_));
    size_t nthreads = envto<size_t>("Fs123RefreshThreads", 10);
    size_t backlog = envto<size_t>("Fs123RefreshBacklog", 10000);
    tp = std::make_unique<threadpool<void>>(nthreads, backlog);
    rootfd_ = ::open(root.c_str(), O_DIRECTORY);
    rootpath_ =  root;
    // If there's no root, we'll make one...
    if(!rootfd_){
        makedirs(root, 0755);
        rootfd_ = sew::open(root.c_str(), O_DIRECTORY);
    }

    if(fancy_sharing){
        statusfd_ = sew::openat(rootfd_, status_filename_, O_RDWR|O_CREAT, 0600);
        // Consider the case where two processes start up concurrently
        // and there's no pre-existing statusfile.  The status file is
        // O_CREAT'ed with zero length.  One of them takes the lock.
        // But after taking the lock, the other one can race ahead and
        // get to read_status before the custodian gets to
        // write_status.  The reader will see a zero-length file,
        // which we are careful to handle (with a warning) in
        // read_status.
        //
        // Try to narrow the window by trying the lock now, and if
        // successful, immediately writing the statusfile.  There's still
        // a window, but it's pretty narrow.
        if(custodian_check())
            write_status(1.0f);
    }else
        custodian_ = true; // without fancy_sharing there are many custodians

    // Look for directories:  <root>/0, <root>/00, <root>/000, <root>/0000
    // If any of them exist, then assume we're looking at a pre-exisiting
    // cache directory with the obvious number of hexdigits.
    hexdigits_ = 0;
    for( auto zeros : {"0", "00", "000", "0000"} ){
        acfd fd = ::openat(rootfd_, zeros, O_DIRECTORY);
        if(fd){
            hexdigits_ = strlen(zeros);
            break;
        }
    }
    // If no pre-existing "0*" directories were found, set hexdigits so
    // that we have a reasonable number of files (1000) in each directory
    // when we have maxfiles total files.
    unsigned suggested_hexdigits = clip(1, int(floor(log_16(vols_.dc_maxfiles/1000.))), 4);
    if(hexdigits_ == 0)
        hexdigits_ = suggested_hexdigits;

    // warn if the actual number and the suggested number differ.
    if(hexdigits_ != suggested_hexdigits)
        complain(LOG_WARNING, "Found pre-existing cache directory: %s with %d-digit sub-directories.  Differs from recommended value of %d-digits when maxfiles = %zd",
                 root.c_str(), hexdigits_, suggested_hexdigits, vols_.dc_maxfiles.load());

    DIAGkey(_diskcache, "cachedir root: " << root << " maxfiles=" << vols_.dc_maxfiles << " hexdigits=" << hexdigits_ << "\n");
    auto Ndirs = 1<<(4*hexdigits_);
    
    check_root(Ndirs);

    // start the evict_loop thread.  The evict loop is almost independent
    // of the rest of the diskcache code.  Points are contact are:
    //
    //   - the eviction loop sets the injection_probability, which
    //     affects the behavior of diskcache::serialize.
    //   - evict_loop does unlinkat(diskcache::rootfd_, ...
    //     and calls 'diskcache::reldirname'
    //   - the diskcache destructor notifies the evict_loop thread
    //     and joins with it, using evict_thread_done_cv, evict_done_mtx_
    //     and evict_thread_done_.
    evict_thread_ = std::thread(&diskcache::evict_loop, this, Ndirs);
}

diskcache::~diskcache(){
    // shut down the evict_loop thread
    {
        std::lock_guard<std::mutex> lk(evict_done_mtx_);
        evict_thread_done_ = true;
        evict_thread_done_cv_.notify_all();
    }

    if(evict_thread_.joinable())
        evict_thread_.join();
    else
        complain(LOG_ERR, "~diskcache destructor called, but evict_thread is not joinable???");
}    

// It's not uncommon (python startup with an empty cache) to see lots
// of back-to-back requests for the same resource.  This hack discards
// rapidly repeated requests.  It can be disabled simply by not
// calling recently_refreshed() at the top of mabye_refresh().

// 'recently' is a vector of urls that we've recently (within
// 500ms) issued a request for.  If a url is already on the list, then
// recently_refreshed(url) returns true.  If it's not on the list,
// then recently_refreshed pushes it (with a timestamp) onto the back
// of the list.  We only keep items on the list for one second, so it
// "can't possibly" get too long.  Use a vector even though we do
// a fair amount of erase-from-the-middle.  
//
// Note that the 'EnhancedConsistency' code in openfilemap.cpp
// aggressively refreshes objects as soon as they expire, and if they
// are within the swr, it refreshes them again a short time (currently
// 750msec) later.  It's important that the window here be less than
// the delay in openfilemap.cpp.  The fact that these two are so
// closely related suggests a design error.
std::mutex recently_mtx;
using recently_p = std::pair<std::string, std::chrono::system_clock::time_point>;
std::vector<recently_p> recently;
bool recently_refreshed(const std::string& url){
    auto now = std::chrono::system_clock::now();
    auto short_time_ago = now - std::chrono::milliseconds(500);
    std::lock_guard<std::mutex> lgd(recently_mtx);
    // blow away anything too old
    auto e = std::lower_bound(recently.begin(), recently.end(), short_time_ago,
                              [&](const recently_p& a, const std::chrono::system_clock::time_point& then){
                                  return a.second < then;
                              });
    stats.recently_refreshed_erased += std::distance(recently.begin(), e);
    recently.erase(recently.begin(), e);
    // find an entry with a matching url
    e = std::find_if(recently.begin(), recently.end(), 
                  [&](const recently_p& a){
                      return a.first == url;
                  });
    if(e != recently.end()){
        stats.recently_refreshed_matched++;
        DIAGfkey(_diskcache, "recently_refreshed(%s) -> true\n", url.c_str());
        return true;
    }
    recently.emplace_back(url, now);
    DIAGfkey(_diskcache, "recently_refreshed(%s) -> false (emplaced)\n", url.c_str());
    stats.recently_refreshed_appended++;
    return false;
}

// The stats are informative but confusing:
//    maybe_rf_too_soon - how many maybe_bg_upstream_refreshes were elided because
//             we had just asked about the same url
//    maybe_rf_submitted - how many times maybe_bg_upstream_refresh sent a request
//             to the background thread-pool
//    maybe_rf_retired - how many of those requests have been retired.
//    stale_while_revalidate - number of maybe_bg_upstream_refresh calls
//    must_refresh - no data on disk or too stale to use.
//    rf_stale_if_error - how many times we returned stale
//             data because we got an error from upstream
//    rf_200 - upstream requests returned 200
//    rf_304 - upstream requests returned 304
//
//  The number of calls to 'maybe' is  equal to the number of
//  stale_while_revalidates:
//    stale_while_revalidate == maybe_rf_too_soon + maybe_rf_started
//
//  The number of upstream calls "returns" is equal to the
//  number of upstream calls made:
//    refresh_stale_if_error + rf_200 + rf_304 ==
//           maybe_rf_submitted + must_refresh
//
//  The number of calls to maybe_bg_upstream_refresh equals the
//  number of retired or failed background refreshes:
//         maybe_rf_started = maybe_rf_retired + detached_refresh_failures

void diskcache::maybe_bg_upstream_refresh(const req123& req, const std::string& path){
    if(recently_refreshed(req.urlstem)){
        stats.dc_maybe_rf_too_soon++;
        return;
    }
    if(disconnected_){
        stats.dc_rf_disconnected_skipped++;
        return;
    }
    stats.dc_maybe_rf_started++;
    DIAGkey(_diskcache, "tp->submit(detached_upstream_refresh) submitted by thread id: " << std::this_thread::get_id() << "\n");
    // The const_cast is safe.  We're not going to scribble on ncreq
    // itself.  We're going to let it be captured by the lambda, which
    // will make a copy, and we want the copy to be implicitly declared
    // non-const in the lambda so that we can scribble on the copy
    // in the lambda.
    req123& ncreq = const_cast<req123&>(req);
    // we need mutable, so that the captured ncreq is not implicitly declared const.
    tp->submit([=]() mutable { detached_upstream_refresh(ncreq, path); });
}

void diskcache::detached_upstream_refresh(req123& req, const std::string& path) noexcept try {
    DIAGkey(_diskcache, "detached_upstream_refresh in tid " << std::this_thread::get_id() << "(" << req.urlstem << ", " << path << " stale_if_error: " <<  req.stale_if_error << ")\n");
    // It's a background request, so it's not latency sensitive.  If we're
    // going to wait for a network round-trip, we might as well insist
    // on something that's actually fresh.  So set max_stale to 0.
    reply123 r;
    req.max_stale = 0;
    upstream_refresh(req, path, &r, true, false);
    stats.dc_maybe_rf_retired++;
}catch(std::exception& e){
    // upstream_refresh can throw in ways that are highly problematic, e.g.,
    // if the threadpool logic throws, we might find ourselves deadlocked
    // In those cases, it might be better to rethrow here, but we don't
    // have enough information to make the decision...
    DIAGkey(_diskcache, "upstream_refresh caught exception\n");
    stats.dc_detached_refresh_failures++;
    complain(LOG_WARNING, e, "detached_upstream_refresh:  caught error: ");
}catch(...){
    complain(LOG_CRIT, "detached_upstream_refresh: caught something other than std::exception.  This can't happen");
    // no point in rethrowing.  See comment in diskcache.hpp
}

void diskcache::detached_serialize(const reply123& r, const std::string& path, const std::string& url) noexcept try {
    DIAGkey(_diskcache,  "detached_serialize, running in thread: " << std::this_thread::get_id() << "\n");
    serialize(r, path, url);
}catch(std::exception& e){
    complain(LOG_WARNING, e, "detached_serialize:  caught error: ");
}catch(...){
    complain(LOG_CRIT, "detached_serialize:  caught something other than std::exception.  This can't happen");
    // no point in rethrowing.  See comment in diskcache.hpp
}

void diskcache::upstream_refresh(const req123& req, const std::string& path, reply123* r, bool already_detached, bool usable_if_error){
    if( disconnected_ && usable_if_error){
        // If we're disconnected and r is within the stale-if-error window,
        // return immediately.  Don't complain. Don't ask upstream.
        stats.dc_rf_stale_if_error++;
        stats.dc_rf_disconnected_skipped++;
        return;
    }
    if(upstream_->refresh(req, r)){
        stats.dc_rf_200++;
        if(already_detached){
            // we're already detached.  Call serialize synchronously.
            // Nobody's waiting for us, and the threadpool will throw
            // an EINVAL if we try to submit to it recursively.
            DIAGkey(_diskcache, "upstream_refresh(detached) in " << std::this_thread::get_id() << " r->expires: " << tp2dbl(r->expires) << ", r->etag64: "  << r->etag64 << "\n");
            serialize(*r, path, req.urlstem);  // might throw, but we're already_detached, so it's caught by caller
        }else{
            // We are not already detached.  This is a fine
            // opportunity to run the serializer detached.
            //
            // The lambda will run in another thread.  Take care to
            // pass args by value!  Don't do std::move(*r) because the
            // caller is still "using" r.  We're not allowed to trash
            // it.
            DIAGkey(_diskcache, "tp->submit(detached_serialize) submitted by thread: " << std::this_thread::get_id() << "\n");
            tp->submit([rv = *r, path, urlstem = req.urlstem, this](){ detached_serialize(rv, path, urlstem); });
        }
    }else{
        stats.dc_rf_304++;
        if(req.no_cache){
            // Not clear what we should do here.  If we get here, it
            // means some serious misunderstanding has happened.  We
            // could probably just log it and carry on.  Or we could
            // go farther and unlink the path so it doesn't keep
            // coming back to haunt us.  In any case, we should fix it
            // ASAP.
            throw se(EINVAL, "diskcache:: upstream_->refresh returned false, interpreted as 304 Not Modified, but the request is no-cache.  That shouldn't happen.  Find this error message in the code and FIX THE UNDERLYING PROBLEM!");
        }
        tp->submit([rv = *r, path, this](){ detached_update_expiration(rv, path); });
    }
}
    
bool
diskcache::refresh(const req123& req, reply123* r) try {
    // FIXME - there's way too much exception-catching going on
    // here.  That's a big red mis-design flag.  Why is the
    // diskcache code thinking about high-latency links and
    // disconnected operation?  What if we're not even using
    // a diskcache?  Shouldn't past_stale_while_revalidate still
    // matter?
    auto path = hash(req.urlstem);
    *r = deserialize(path);
    // According to RFC5861, stale_while_revalidate is specified by
    // the Cache-control header in the reply123, *r, which is under
    // the sole control of the origin server.
    //
    // But that's *not* the level of control we want when running
    // fs123 over a high-latency link.  For that, we want to exceed
    // the reply's r->swr.  Even if something is too stale for
    // "normal" clients and "normal" proxy caches, we may want to use
    // it anyway rather than wait for a slow round-trip to revalidate
    // it.  For that purpose, we have:
    //
    //     req.past_stale_while_revalidate
    //
    // which is added to the server-supplied r->stale_while_revalidate
    // to tell us whether we should return it.
    //
    // Keep in mind that past_stale_while_revalidate is a non-http
    // modification to the rules that applies only to the diskcache
    // (this code).  It's set by a client-side option (and fcntl):
    //    -oFs123PastStaleWhileRevalidate
    // It's not transmitted to http proxies or origin servers.
    //
    // On the other hand, stale-while-revalidate and max-stale have
    // clearly defined meaning in http.
    auto swr = r->stale_while_revalidate + std::chrono::seconds(req.past_stale_while_revalidate);
    if(req.max_stale >= 0){
        auto reqms = std::chrono::seconds(req.max_stale);
        if(reqms < swr)
            swr = reqms;
    }
    auto ttl = r->ttl();
    DIAGkey(_diskcache, "diskcache::refresh reply->{ttl: " <<  str(ttl) << " swr: " << str(r->stale_while_revalidate) << "} net swr: " << str(swr) << "\n");
    if( !req.no_cache && ttl > decltype(ttl)::zero() ){
        DIAGfkey(_diskcache, "diskcache::refresh hit\n");
        stats.dc_hits++;
    }else if( !req.no_cache && ttl > -swr ){
        DIAGfkey(_diskcache, "diskcache::refresh swr\n");
        stats.dc_stale_while_revalidate++;
        maybe_bg_upstream_refresh(req, path);
    }else{
        DIAGkey(_diskcache, "diskcache::refresh miss!\n");
        stats.dc_must_refresh++;
        bool usable_if_error;
        try{
            // We could save a copy of the reply here, but since
            // exceptions should be rare, that would entail making an
            // unnecessary copy of reply.content the vast majority of
            // the time.  Instead, we record whether it was usable,
            // and if it was, we deserialize it again in the catch
            // block.
            usable_if_error = r->valid() && std::chrono::seconds(req.stale_if_error) >= -ttl;
            upstream_refresh(req, path, r, false, usable_if_error);
        }catch(std::exception& e){
            if(usable_if_error){
                *r = deserialize(path);
                ttl = r->ttl();
                usable_if_error = r->valid() && std::chrono::seconds(req.stale_if_error) >= -ttl;
            }
            if( !usable_if_error )
                std::throw_with_nested(std::runtime_error(fmt("diskcache::refresh: upstream threw and cached result is %s.  staleness=%s, stale_if_error: %d\n",
                                                                       r->valid()?"valid":"invalid",
                                                                       str(-ttl).c_str(), req.stale_if_error)));
            complain(LOG_WARNING, e,
                     "diskcache::refresh:  returning stale data:  staleness: " + str(-ttl) + " stale_if_error: " + std::to_string(req.stale_if_error) + ". Upstream error: ");
            stats.dc_rf_stale_if_error++;
        }
    }
    return true;
 }catch(std::exception& e){
    std::throw_with_nested(std::runtime_error("diskcache::refresh(req.urlstem=" + req.urlstem + ")"));
 }

std::ostream& 
diskcache::report_stats(std::ostream& os) {
    os << stats;
    os << "dc_threadpool_backlog: " << tp->backlog() << "\n";
    return upstream_->report_stats(os);
}

std::string 
diskcache::hash(const std::string& s){
    auto hd = threeroe(s, hashseed_.first, hashseed_.second).Final().hexdigest();
    DIAGkey(_diskcache, "diskcache::hash(" + s + ") -> " + hd + "\n");
    return hd.substr(0, hexdigits_) + "/" + hd.substr(hexdigits_);
}

reply123
diskcache::deserialize_no_unlink(int rootfd, const std::string& path,
                                 std::string *returlp) {
    atomic_scoped_nanotimer _t(&stats.diskcache_deserialize_sec);
    refcounted_scoped_nanotimer _rt(deserialize_nanotimer_ctrl);
    refcounted_scoped_nanotimer _rtx(serdes_nanotimer_ctrl);
    acfd fd = rootfd == -1 ? ::open(path.c_str(), O_RDONLY) :
                        ::openat(rootfd, path.c_str(), O_RDONLY);
    if(!fd){
        DIAGkey(_diskcache, "diskcache::deserialize(" << path << ") miss\n");
        return {}; // invalid!
    }
    DIAGkey(_diskcache, "diskcache::deserialize(" << path << ") hit\n");
    
    // Should we advise that this file won't be reused?  The argument
    // in favor is that we shouldn't compete with the kernel for
    // buffercache resources.  We'd much rather see kernel pages used
    // to buffer the fuse-mounted file itself.  The argument against
    // is that the ChunkSize is bigger than the size typically
    // requested in a single callback, and hence there's a good chance
    // that a reader will make several requests for different offsets
    // in the same cached chunk, in which case we benefit from having
    // the whole chunk in buffer cache.
    //
    // sew::posix_fadvise(fd, 0, 0, POSIX_FADV_NOREUSE);

    // How much paranoia is appropriate here?  We can't really defend
    // against genuinely malicious file replacement, but we can defend
    // against "accidents" - whatever that means.  We certainly
    // shouldn't segfault (which rules out using mmap instead of
    // read).  It might helpful to establish a maximum-content-size,
    // which we could pre-allocate space for.  Note that in protocol=7
    // all requests (including /d/) have a upper limit on size, so this
    // realy is practical.
    struct stat sb;
    sew::fstat(fd, &sb);
    if(!S_ISREG(sb.st_mode))
        throw se(EINVAL, "diskcache::deserialize: not a regular file");
    // ?? anything else ?? E.g., limits on st_size?? Ownership and permissions?
    size_t content_len;
    reply123 ret;
    struct iovec iov[3];
    iov[0].iov_base = (char *)&ret + reply123_pod_begin;
    iov[0].iov_len = reply123_pod_length;
    iov[1].iov_base = &content_len;
    iov[1].iov_len = sizeof(content_len);
    size_t nread = sew::readv(fd, iov, 2);
    stats.diskcache_deserialize_bytes += nread;
    if(nread != (iov[0].iov_len + iov[1].iov_len))
        throw se(EINVAL, fmt("diskcache::deserialize: expected to read %zd+%zd bytes.  Only got %zd\n",
                                     iov[0].iov_len, iov[1].iov_len, nread));
    if( ret.magic != ret.MAGIC ){
        complain(LOG_NOTICE, "Rejecting cache file with incorrect magic number (got %d, expected  %d): %s",
                 ret.magic, ret.MAGIC, path.c_str());
        return {};
    }
    // Prior to 0.34.0, there was no url and this test demanded an
    // exact match between the size of the file and the size deduced
    // from the header.  In 0.34.0 we added the url, the url's length
    // and a magic number at the end of the file.  We *could* add more
    // error-checking here to confirm that the magic number and the
    // url_len at the end of the file "add up".  But we've never seen
    // this test fail, so it doesn't seem worth the trouble to do
    // extra I/O only to apply a stronger sanity-check that is
    // unlikely to ever fail.
    size_t bytes_not_counting_url = reply123_pod_length + sizeof(content_len) + content_len + 2*sizeof(int32_t);
    if((size_t)sb.st_size < bytes_not_counting_url)
        throw se(EINVAL, fmt("diskcache::deserialize: st_size=%jd, should be >= %zu\n",
			     (intmax_t)sb.st_size, bytes_not_counting_url));
    ret.content.resize(content_len);
    nread = sew::read(fd, &ret.content[0], content_len);
    stats.diskcache_deserialize_bytes += nread;
    if(nread != content_len){
        // Sep 2017 - we're seeing these, and I don't know why... Try
        // to report more in the throw:
        struct stat xsb{};
        errno = 0;
        fstat(fd, &xsb);
        throw se(EINVAL, fmt("diskcache::deserialize:  tried to read %zd content bytes.  Only got %zd.  fstat(fd): errno: %d, sb: %s\n",
                                              content_len, nread, errno, str(sb).c_str()));
    }
    if (returlp) {
        size_t urlsz = sb.st_size - bytes_not_counting_url;
        int32_t ulen, cmagic;
        returlp->resize(urlsz);
        iov[0].iov_base = &((*returlp)[0]);
        iov[0].iov_len = urlsz;
        iov[1].iov_base = &ulen;
        iov[1].iov_len = sizeof(ulen);
        iov[2].iov_base = &cmagic;
        iov[2].iov_len = sizeof(cmagic);
        nread = sew::readv(fd, iov, 3);
        auto expread = iov[0].iov_len + iov[1].iov_len + iov[2].iov_len;
        if (nread != expread) {
            throw se(EINVAL, fmt("diskcache::deserialize: tried to read %zu url bytes.  Only got %zd. errno: %d, urlsz %zu, sb: %s\n",
                                 expread, nread, errno, urlsz, str(sb).c_str()));
        }
        if (ulen != (int32_t)urlsz) {
            throw se(EINVAL, fmt("diskcache::deserialize: cache says url len %ld, expected %zu\n", (long)ulen, urlsz));
        }
        if (cmagic != reply123::MAGIC) {
            throw se(EINVAL, fmt("diskcache::deserialize: cache says magic %ld, expected %ld\n", (long)cmagic, (long)reply123::MAGIC));
        }
    }
    fd.close();
    // ret.content_threeroe is not NUL-terminated, so we have to use the
    // four-argument string::compare.
    static const size_t thirtytwo = sizeof(ret.content_threeroe);
    if(threeroe(ret.content).Final().hexdigest().compare(0, thirtytwo, ret.content_threeroe, thirtytwo) != 0){
        throw se(EINVAL, fmt("diskcache::deserialize:  threeroe mismatch:  threeroe(data): %s, threeroe(stored_in_header): %.32s.\n",
                             threeroe(ret.content).Final().hexdigest().c_str(), ret.content_threeroe));
    }
    return ret;
}

reply123
diskcache::deserialize(const std::string& path) try { 
    return deserialize_no_unlink(rootfd_, path);
 }catch(std::exception& e){
    // Unlink files that give us trouble deserializing?  In theory,
    // nothing in the cache is precious.  So if something trips us up,
    // it's usually better to remove it than to leave it where it will
    // trip us up again.
    //   
    // But would it be better to rename it so we can study it later?
    ::unlinkat(rootfd_, path.c_str(), 0);
    // Now that we've renamed it, we might as well return a miss rather
    // than throwing an exception.
    complain(LOG_WARNING, e, "diskcache::deserialize("+rootpath_ +"/"+path+"):  file unlinked.  Returning 'invalid', i.e., MISS");
    return {};
 }

void 
diskcache::serialize(const reply123& r, const std::string& path, const std::string& url){
    atomic_scoped_nanotimer _t(&stats.diskcache_serialize_sec);
    refcounted_scoped_nanotimer _rt(serialize_nanotimer_ctrl);
    refcounted_scoped_nanotimer _rtx(serdes_nanotimer_ctrl);
    static std::atomic<long long> rofs_defer_till{0};
    if(!should_serialize(r))
       return;
    if( rofs_defer_till > _t.started_at() ){
        stats.serialize_deferred_rofs++;
        return;
    }
    DIAGkey(_diskcache, "diskcache::serialize(" << path << " now=" << ins(std::chrono::system_clock::now()) << " eno=" << r.eno << " fresh=" << r.fresh() << " expires=" << ins(r.expires) << " etag64=" << r.etag64 << ")\n");
    // A single diskcache object is used concurrently by many threads.
    // Take care that they don't step on one anothers rngs.
    static std::atomic<int> seed(0); // give a different seed to every thread.
    VALGRIND_HG_DISABLE_CHECKING(&seed, sizeof(seed));
    static thread_local std::default_random_engine eng(seed++);
    std::uniform_real_distribution<float> ureal(0., 1.);
    if(  ureal(eng) > injection_probability_ ){
        DIAGfkey(_diskcache, "diskcache::serialize:  rejected with injection_probability=%.2f\n", injection_probability_.load());
        return;
    }
    if(!r.fresh())
        stats.serialize_stale++;  // used to return, but that denies a lot of swr and sie opportunities.

    std::string pathnew = path + ".new";
    const char* unlink_me = pathnew.c_str();
    acfd fd = ::openat(rootfd_, pathnew.c_str(), O_WRONLY|O_CREAT|O_EXCL, 0600);
    // O_EXCL|O_CREAT guarantees that only one process can possibly
    // own the .new file
    DIAGkey(_diskcache, "diskcache::serialize opened " << pathnew << " " << fd.get() << "\n");
    if(!fd){
        switch(errno){
        case EEXIST:
            // These aren't as rare as we might hope.  It's not uncommon to
            // get back-to-back reads for the same chunk, and the second one
            // will often run into the in-progress serialization of the first.
            stats.serialize_eexist++;
            // Not only that - the bandwidth was wasted!  It would take
            // some work to "fix" it though.  We'd need a whole new control
            // flow to "attach" one request to another already-in-progress
            // one. Let's count before we start writing new code...
            stats.serialize_eexist_wasted_bytes += r.content.size();
            break;
        case EROFS:
            // ext family filesystems will remount themselves read-only after a certain
            // number of write errors.  Trying to write to them can only make things worse.
            // This is a condition that requires administrative intervention.  Complain
            // loudly (LOG_ERR) every 5 minutes.
            stats.serialize_erofs++;
            rofs_defer_till = _t.started_at() + 300ull * 1000 * 1000 * 1000; // 5 minutes, in scoped_nanotimer's units
            complain(LOG_ERR, "diskcache::serialize EROFS.  Administrative intervention required!  Serialization will be deferred for 5 minutes");
            break;
        default:
            complain(LOG_WARNING, "diskcache::serialize failed to create %s.  errno=%m",
                   pathnew.c_str());
            stats.serialize_other_failures++;
            break;
        }
        return;
    }
    try{
        struct iovec iov[6];
        iov[0].iov_base = (char*)&r + reply123_pod_begin;
        iov[0].iov_len = reply123_pod_length;
        size_t content_len = r.content.size();
        iov[1].iov_base = &content_len;
        iov[1].iov_len = sizeof(content_len);
        iov[2].iov_base = const_cast<char*>(r.content.data());
        iov[2].iov_len = content_len;

        // append the url and its length and the value of 'magic' to
        // the end of the file.  This should be enough for an
        // unrelated process, e.g., a cache scanner, to walk the
        // cache looking for files that match a url, etc.
        int32_t url_len = url.size();
        iov[3].iov_base = const_cast<char*>(url.data());
        iov[3].iov_len = url_len;
        iov[4].iov_base = &url_len;
        iov[4].iov_len = sizeof(url_len);
        iov[5].iov_base = const_cast<int*>(&r.magic);
        iov[5].iov_len = sizeof(r.magic);
        size_t wrote = sew::writev(fd, iov, 6);
        stats.diskcache_serialize_bytes += wrote;
        // Don't close before renaming, as that would leave a window
        // for another thread to trash the file before we rename it.
        sew::renameat(rootfd_, pathnew.c_str(), rootfd_, path.c_str());
        // But if fd.close() throws, unlink the 'path', without the
        // appended ".new".
        unlink_me = path.c_str(); // c_str() is nothrow
        fd.close();
        // If fd.close() throws, there's conceivably a window for
        // another thread to read the corrupt(?) file between the
        // failed close and the unlinkat in the catch block.  But
        // we're already defending against data corruption in several
        // ways (magic numbers, threeroe sums or cryptographic
        // authentication, length consistency checks).  We'd probably
        // be safe even if we never unlinked the file.  We've made a
        // conscious decision to accept the risk.
	DIAGkey(_diskcache, "diskcache::serialize wrote " << path << "\n");
    }catch(std::exception& e){
        // unlinking the bad path is an attempt to avoid cascading errors.
        // There's a good chance this unlink will fail, e.g., if the
        // original error was ENOENT.  But let's try anyway, and record
        // the result for posterity in the nested exception.
        int ret = ::unlinkat(rootfd_, unlink_me, 0);
        std::throw_with_nested(std::runtime_error(fmt("diskcache::serialize(path=%s): unlinkat(%s) returned %d errno=%d",
                                                      path.c_str(), unlink_me, ret, errno)));
    }
}

void
diskcache::detached_update_expiration(const reply123& r, const std::string& path) noexcept try {
    atomic_scoped_nanotimer _t(&stats.diskcache_update_sec);
    refcounted_scoped_nanotimer _rt(update_nanotimer_ctrl);
    refcounted_scoped_nanotimer _rtx(serdes_nanotimer_ctrl);

    stats.diskcache_updates++;
    // I think that this:
    //    http://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_09_07
    // promises that write and writev are atomic.  I.e., that we can write over
    // the header bytes in the file and that readers will see either the new
    // or the old data, but never a mixture.
    acfd fd = sew::openat(rootfd_, path.c_str(), O_WRONLY);
    auto nwrote = sew::write(fd, (char*)&r + reply123_pod_begin, reply123_pod_length);
    stats.diskcache_update_bytes += nwrote;
    fd.close();
 }catch(std::exception& e){
    // unlinking the bad path is an attempt to avoid cascading errors.
    // There's a good chance this unlink will fail, e.g., if the
    // original error was ENOENT.  But let's try anyway, and let's not
    // worry about whether it succeeds.
    ::unlinkat(rootfd_, path.c_str(), 0);
    complain(LOG_WARNING, e, "detached_update_expiration(" + path +") caught error");
 }catch(...){
    complain(LOG_CRIT, "detached_update_expiration:  caught something other than std::exception.  This can't happen");
    // no point in rethrowing.  See comment in diskcache.hpp
 }
