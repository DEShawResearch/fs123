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
#include <core123/uuid.hpp>
#include <random>
#include <vector>
#include <utility>
#include <thread>
#include <cmath>

static const int UUID_ASCII_LEN=36; // really?  Not defined in uuid.h?

using namespace core123;

static auto _diskcache = diag_name("diskcache");
static auto _evict = diag_name("evict");

namespace{
#define STATS_MACRO_NAME DISKCACHE_STATISTICS
#define STATS_STRUCT_TYPENAME diskcache_stats_t
#include <core123/stats_struct_builder>
diskcache_stats_t stats;

inline double log_16(double x){
    return ::log2(x)/4.;
}

 refcounted_scoped_nanotimer_ctrl serialize_nanotimer_ctrl(stats.dc_serialize_inuse_sec);
 refcounted_scoped_nanotimer_ctrl deserialize_nanotimer_ctrl(stats.dc_deserialize_inuse_sec);
 refcounted_scoped_nanotimer_ctrl update_nanotimer_ctrl(stats.dc_update_inuse_sec);
 refcounted_scoped_nanotimer_ctrl serdes_nanotimer_ctrl(stats.dc_serdes_inuse_sec);

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

void
create_uuid_symlink(int rootfd){
    std::string uu = gen_random_uuid();
    if(::symlinkat(uu.c_str(), rootfd, ".uuid") < 0){
        if(errno != EEXIST)
            throw se("create_uuid_symlink: symlinkat failed");
        complain(LOG_WARNING, "create_uuid_symlink:  failed with EEXIST.  This is normal if multiple diskcaches are starting concurrently, but is very surprising otherwise");
        return;
    }
    // Don't leave a dangling symlink.  This is not necessary for
    // correctness, but it's just bad form to leave a dangling
    // symlink.  If either the open or the close fails, something is
    // badly broken and sew will tell us about it.
    sew::close(sew::openat(rootfd, uu.c_str(), O_CREAT|O_WRONLY, 0600));
}

} // end namespace <anon>

void 
diskcache::check_root(){
    // FIXME - there should be more checks here!

    // make sure that the directories we need either
    // already exist, or that we successfully create them.
    for(unsigned i=0; i<Ndirs_; ++i){
        makedirsat(rootfd_, reldirname(i), 0700, true);
    }
    // Check for a symlink called .uuid in the root directory.  If it's
    // not there, create it.  Then readlink it into the uuid member.
    // UUIDs are *always* UUID_ASCII_LEN (36) bytes plus a null
    char uuid_buf[UUID_ASCII_LEN+1];
    int ntry = 0;
 tryagain:
    auto nbytes = ::readlinkat(rootfd_, ".uuid", uuid_buf, sizeof(uuid_buf));
    if(nbytes != UUID_ASCII_LEN){
        if(nbytes < 0 && errno == ENOENT && ++ntry<2){
            create_uuid_symlink(rootfd_);
            goto tryagain;
        }
        throw se("diskcache::diskcache:  uuid symlink is borked");
    }
    uuid = std::string(uuid_buf, UUID_ASCII_LEN);
}

std::string 
diskcache::reldirname(unsigned i) const /*protected*/{
    char s[32];
    sprintf(s, "%0*x", int(hexdigits_), i);
    return s;
}

scan_result
diskcache::do_scan(unsigned dirnum) const /*protected*/{
    scan_result ret;
    acDIR dp = sew::opendirat(rootfd_, reldirname(dirnum).c_str());
    struct dirent* entryp;
    DIAGf(_evict, "do_scan with dirfd(dp)=%d", dirfd(dp));
    // glibc deprecated readdir_r in 2.24 (2016), but it was
    // thread-safe long before that: https://lwn.net/Articles/696474/
    //
    // "in modern implementations (including the glibc
    // implementation), concurrent calls to readir(3) that specify
    // different directory streams are thread-safe."
    while( (entryp = sew::readdir(dp)) ){
        std::string fname = entryp->d_name;
        struct stat sb;
        DIAGfkey(_evict>1, "readdir -> %x/%s\n", dirnum, fname.c_str());
        try{
            sew::fstatat(dirfd(dp), fname.c_str(), &sb, 0);
        }catch(std::system_error& se){
            // rethrow everything but ENOENT
            DIAGfkey(_evict, "fstatat(%d) threw se.code().value() = %d\n", dirfd(dp), se.code().value());
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
diskcache::evict(size_t Nevict, size_t dir_to_evict, scan_result& sr) /*protected*/ {
    // try to evict Nevict files from the current dir_to_evict_.
    // The names (and perhaps other metadata) are in sr.
    std::string pfx = reldirname(dir_to_evict) + "/";
    for(size_t i=0; i<Nevict; ++i){
        // Randomly pick one of the names in the inclusive range [i, size()-1]
        size_t j = std::uniform_int_distribution<size_t>(i, sr.names.size()-1)(urng_);
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
diskcache::custodian_check() /*protected*/{
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
diskcache::write_status(float prob) const /*protected*/{
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
diskcache::read_status() /*protected*/{
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

// evict_once is called periodically by a core123::periodic object.
//
// If the calling thread is not the cache 'custodian', it sets the
// injection probability by atomically reading the statusfile and
// returns, telling the periodic object to call it again in 10
// seconds.
//
// If the calling thread is the cache 'custodian', it scans one of the
// cache directories and estimates the cache 'usage_fraction'.  If the
// usage fraction exceeds the 'evict_target', files are deleted
// (randomly) from the scanned directory to reach the 'evict_lwm'.  If
// the usage_fraction exceeds the 'evict_throttle_lwm', the injection
// probability is set to a value less than one to throttle cache
// growth.  It returns, telling the periodic object how long to wait
// before calling again.  The value returned is the injection
// probability times the 'evict_period_minutes' divided by the number
// of cache directories.  Thus, under unloaded conditions, the entire
// cache will be scanned in evict_period_minutes, and more frequently
// when the cache is under pressure.
std::chrono::system_clock::duration
diskcache::evict_once() /*protected*/ try {
    // It's easier to work with sleepfor in a floating point
    // representation.  We'll duration_cast it before returning.  And
    // in an abundance of caution, we'll use duration<double> instead
    // of <float>, because we've learnd that duration<float> and
    // system_clock::duration don't mix.
    std::chrono::duration<double> sleepfor;
    float inj_prob;
    if(custodian_check()){
        auto scan = do_scan(dir_to_evict_);
        auto Nfiles = scan.names.size();
        float maxfiles_per_dir = float(vols_.dc_maxfiles) / Ndirs_;
        float maxbytes_per_dir = float(vols_.dc_maxmbytes)*1000000. / Ndirs_;
        double filefraction = Nfiles / maxfiles_per_dir;
        double bytefraction = scan.nbytes / maxbytes_per_dir;
        double usage_fraction = std::max( filefraction, bytefraction );
        size_t Nevict = 0;
        DIAG(_evict, str("Usage fraction:", usage_fraction, "in directory", reldirname(dir_to_evict_)));
        if(usage_fraction > 1.0){
            if(overfull_++ == 0)
                complain(LOG_WARNING, "Usage fraction %g > 1.0 in directory: %zx.  Disk cache may be dangerously close to max-size.  This message is issued once per scan",
                         usage_fraction, dir_to_evict_);
        }
        // We don't evict anything until we're above evict_target_fraction.
        if( usage_fraction > vols_.evict_target_fraction ){
            // We're above evict_target_fraction.  Try to get down to 'evict_lwm'
            auto evict_fraction = (usage_fraction - vols_.evict_lwm)/usage_fraction;
            Nevict = clip(0, int(ceil(Nfiles*evict_fraction)), int(scan.names.size()));
            complain(LOG_NOTICE, "evict %zd files from %zx. In this directory: files: %zu (%g) bytes: %zu (%g)",
                     Nevict, dir_to_evict_, Nfiles, filefraction, scan.nbytes, bytefraction);
        }
        evict(Nevict, dir_to_evict_, scan);
        if(++dir_to_evict_ >= Ndirs_)
            dir_to_evict_ = 0;
        if(dir_to_evict_ == 0){
            complain((files_evicted_ == 0)? LOG_INFO : LOG_NOTICE, "evict_once:  completed full scan.  Found %zd files of size %zd.  Evicted %zd files.  Found %zd over-full directories", 
                     files_scanned_, bytes_scanned_, files_evicted_, overfull_);
            files_evicted_ = 0;
            files_scanned_ = 0;
            bytes_scanned_ = 0;
            overfull_ = 0;
        }
        files_evicted_ += Nevict;
        files_scanned_ += Nfiles;
        bytes_scanned_ += scan.nbytes;
        stats.dc_eviction_dirscans++;

        // If usage_fraction is above evict_throttle_lwm, we assume we're "under attack".
        // There are two responses:
        //   lower the injection_probability to randomly reject insertion of new objects
        //   lower the interval between directory scans.
        inj_prob = clip(0., (1. - usage_fraction)/(1. - vols_.evict_throttle_lwm), 1.);
        if(statusfd_) // i.e., we really are fancy_sharing.
            write_status(inj_prob);
        sleepfor = std::chrono::minutes(vols_.evict_period_minutes);
        sleepfor *= inj_prob / Ndirs_;
        DIAG(_evict, str("evict_once: Custodian: sleepfor after *=:", sleepfor, sleepfor.count(), "inj_prob:", inj_prob));
    }else{
        inj_prob = read_status();
        sleepfor = std::chrono::seconds(10);
        DIAG(_evict, str("evict_once: Non-custodian: sleepfor:", sleepfor, "inj_prob:", inj_prob));
    }
    if(inj_prob < 1.0)
        complain(LOG_NOTICE, "Injection probability set to %g", inj_prob);
    else if(injection_probability_ < 1.0)
        complain(LOG_NOTICE, "Injection probability restored to 1.0");
    injection_probability_.store(inj_prob);
    return std::chrono::duration_cast<std::chrono::system_clock::duration>(sleepfor);
 }catch(std::exception& e){
    // We're the thread's entry point, so the buck stops here.  If we
    // rethrow, we terminate the core123::periodic, which seems
    // "harsh".  Our only choice is to hope that somebody notices the
    // complaint.  OTOH, let's avoid thrashing by waiting for 5
    // minutes before trying again.
    injection_probability_.store(0.);
    complain(e, "evict_once:  caught exception.  Diskcache injection disabled.  Eviction deferred for 5 minutes.");
    return std::chrono::minutes(5);
 }

    
diskcache::diskcache(backend123* upstream, const std::string& root,
                     uint64_t hash_seed_first, bool fancy_sharing, volatiles_t& vols) :
    backend123(),
    upstream_(upstream),
    injection_probability_(1.0),
    hashseed_(hash_seed_first, 0),
    vols_(vols)
{
    // We made injection_probability_ std:atomic<float> so we wouldn't
    // have to worry about it getting ripped or torn.  But helgrind
    // worries anyway.  Tell it to stop.
    size_t nthreads = envto<size_t>("Fs123RefreshThreads", 10);
    size_t backlog = envto<size_t>("Fs123RefreshBacklog", 10000);
    foreground_serialize = envto<bool>("Fs123ForegroundSerialize", false);
    tp = std::make_unique<threadpool<void>>(nthreads, backlog);
    makedirs(root, 0755, true); // EEXIST is not an error.
    rootfd_ = sew::open(root.c_str(), O_DIRECTORY);
    rootpath_ =  root;

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
    Ndirs_ = 1<<(4*hexdigits_);
    
    check_root();

    // start the periodic evict_thread.  The evict_thread is almost
    // independent of the rest of the diskcache code.  Points are
    // contact are:
    //
    //   - evict_once sets the injection_probability, which
    //     affects the behavior of diskcache::serialize.
    //   - evict_once does unlinkat(diskcache::rootfd_, ...
    //     and calls 'diskcache::reldirname'
    evict_thread_ = std::make_unique<periodic>([this](){return evict_once();});
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
    stats.dc_recently_refreshed_erased += std::distance(recently.begin(), e);
    recently.erase(recently.begin(), e);
    // find an entry with a matching url
    e = std::find_if(recently.begin(), recently.end(), 
                  [&](const recently_p& a){
                      return a.first == url;
                  });
    if(e != recently.end()){
        stats.dc_recently_refreshed_matched++;
        DIAGfkey(_diskcache, "recently_refreshed(%s) -> true\n", url.c_str());
        return true;
    }
    recently.emplace_back(url, now);
    DIAGfkey(_diskcache, "recently_refreshed(%s) -> false (emplaced)\n", url.c_str());
    stats.dc_recently_refreshed_appended++;
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

void diskcache::maybe_bg_upstream_refresh(const req123& req, const std::string& path, reply123* replyp) /*protected*/ {
    if(recently_refreshed(req.urlstem)){
        stats.dc_maybe_rf_too_soon++;
        return;
    }
    if(vols_.disconnected){
        stats.dc_rf_disconnected_skipped++;
        return;
    }
    stats.dc_maybe_rf_started++;
    DIAGkey(_diskcache, "tp->submit(detached_upstream_refresh) submitted by thread id: " << std::this_thread::get_id() << "\n");
    // We have to copy the reply because the refresh happens on
    // another thread and "this" copy might be gone before that thread
    // executes.  But nobody will ever need the copy of
    // reply->contents: either it will be replaced by the
    // upstream_refresh, or the upstream will tell us 304 Not
    // Modified, and we'll replace just the on-disk metadata, ignoring
    // copy_of_replyp.content.  It shouldn't be too hard to save
    // ourselves the cost of copying reply->content (which might be
    // non-negligible).  But this code is way too fragile already, so
    // before we do that, let's gather some statistics to assess how
    // much we might save.
    stats.dc_wasted_copy_reply_bytes += replyp->content.size();
    // The const_cast-ing and mutable modifier here is safe because
    // the lambda is working with a copy of req and replyp.  But it's
    // yet another indicator that the API is mis-designed.
    tp->submit([=, req=const_cast<req123&>(req), reply=replyp->copy()]() mutable { detached_upstream_refresh(req, path, &reply); });
}

void diskcache::detached_upstream_refresh(req123& req, const std::string& path, reply123* replyp) noexcept /*protected*/ try {
    DIAGkey(_diskcache, "detached_upstream_refresh in tid " << std::this_thread::get_id() << "(" << req.urlstem << ", " << path << " stale_if_error: " <<  req.stale_if_error << ")\n");
    // It's a background request, so it's not latency sensitive.  If we're
    // going to wait for a network round-trip, we might as well insist
    // on something that's actually fresh.  So set max_stale to 0.
    req.max_stale = 0;
    upstream_refresh(req, path, replyp, true/*already_detached*/, false/*usable_if_error*/);
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

void diskcache::do_serialize(const reply123* r, const std::string& path, const std::string& urlstem, bool already_detached){
    // already_detached means two things:
    //  1 - we're already running in the threadpool.  DO NOT tp->submit.
    //  2 - we're wrapped in a try{}catch(...){}.  Don't worry about throwing.
    if(already_detached || foreground_serialize){
        // we're already detached.  Call serialize synchronously.
        // Nobody's waiting for us, and the threadpool will throw
        // an EINVAL if we try to submit to it recursively.
        DIAGkey(_diskcache, "upstream_refresh(detached) in " << std::this_thread::get_id() << " r->expires: " << ins(r->expires) << ", r->etag64: "  << r->etag64 << "\n");
        serialize(*r, path, urlstem);  // might throw, but we're already_detached, so it's caught by caller
    }else{
        // We are not already detached.  Submit the serializer to
        // the threadpool.
        //
        // Work around issues when r->content is "copy-on-write".  (It
        // shouldn't be, but it still is in RedHat's devtoolsets in
        // 2020).  Since C++11, r->copy() *should* make a bona fide
        // copy of r->content, and the copy should have a lifetime
        // completely independent of the original.  But if std::string
        // is CoW, things get confused, and when the copied reply
        // ultimately gets destroyed in the lambda, we end up with a
        // double-free.  To work around, we force r->content to be
        // copied by calling its *non-const* operator[]() before
        // calling r->copy().
        const char* rc0 = &const_cast<std::string&>(r->content)[0];
        tp->submit([rv = r->copy(), path, rc0, urlstem = urlstem, this](){
                       try{
                           if(rc0 == &rv.content[0]){
                               complain(LOG_CRIT, "Uh oh.  Copy-on-write problems!  rc0=%p, &rv.content[0]=%p.  Reply was not copied correctly into lambda %s:%d", rc0, &rv.content[0], __FILE__, __LINE__);
                               std::terminate();
                           }
                           serialize(rv, path, urlstem);
                       }catch(std::exception& e){
                           complain(LOG_WARNING, e, "detached_serialize:  caught error: ");
                       }catch(...){
                           complain(LOG_CRIT, "detached_serialize:  caught something other than std::exception.  This can't happen");
                           // no point in rethrowing.  See comment in diskcache.hpp
                       }
                   });
    }
}    

void diskcache::upstream_refresh(const req123& req, const std::string& path, reply123* r, bool already_detached, bool usable_if_error)/*protected*/{
    if( vols_.disconnected && usable_if_error){
        // If we're disconnected and r is within the stale-if-error window,
        // return immediately.  Don't complain. Don't ask upstream.
        stats.dc_rf_stale_if_error++;
        stats.dc_rf_disconnected_skipped++;
        return;
    }
    if(upstream_->refresh(req, r)){
        stats.dc_rf_200++;
        do_serialize(r, path, req.urlstem, already_detached);
    }else{
        if(req.no_cache){
            // Not clear what we should do here.  If we get here, it
            // means some serious misunderstanding has happened.  We
            // could probably just log it and carry on.  Or we could
            // go farther and unlink the path so it doesn't keep
            // coming back to haunt us.  In any case, we should fix it
            // ASAP.
            throw se(EINVAL, "diskcache:: upstream_->refresh returned false, interpreted as 304 Not Modified, but the request is no-cache.  That shouldn't happen.  Find this error message in the code and FIX THE UNDERLYING PROBLEM!");
        }
        // In earlier versions, we had some "clever" code that used a
        // single pwrite to update only the expiration times in the
        // on-disk cache.  POSIX says that read() and pwrite() are
        // "atomic", i.e., "If two threads each call one of these
        // functions, each call shall either see all of the specified
        // effects of the other call or none of them".  But Linux
        // (apparently) interprets this as though "the effects" of the
        // write are undefined from the time the call is made till the
        // time it returns.  So a read that happens concurrently with
        // a write has no(!) all-or-nothing guarantee.
        // 
        // In light of this, we now just call serialize, which
        // replaces the whole file using the conventional
        // open(tmpfile)/write/close/rename idiom.  Writing 128k of
        // data so that we can atomically update 32 bytes seems
        // wasteful.  But it shouldn't be noticeable unless Fs123Chunk
        // is a lot bigger than 128(kB).
        stats.dc_rf_304++;
        stats.dc_rf_304_bytes += r->content.size();
        do_serialize(r, path, req.urlstem, already_detached);
    }
}
    
bool
diskcache::refresh(const req123& req, reply123* r) /*override*/ try {
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
        maybe_bg_upstream_refresh(req, path, r);
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
                                                                       r->valid()?"valid":"invalid or non-existant",
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
diskcache::report_stats(std::ostream& os) /*override*/{
    os << stats;
    return os << "dc_threadpool_backlog: " << tp->backlog() << "\n";
}

std::string
diskcache::get_uuid() /*override*/{
    if(uuid.size() != 36)
        throw std::runtime_error("diskcache::get_uuid wrong length??");
    return uuid;
}

std::string 
diskcache::hash(const std::string& s){
    auto hd = threeroe(s, hashseed_.first, hashseed_.second).hexdigest();
    DIAGkey(_diskcache, "diskcache::hash(" + s + ") -> " + hd + "\n");
    return hd.substr(0, hexdigits_) + "/" + hd.substr(hexdigits_);
}

void /*static*/
diskcache::deserialize_no_unlink(int rootfd, const std::string& path,
                                 reply123 *ret,
                                 std::string *returlp) {
    atomic_scoped_nanotimer _t(&stats.dc_deserialize_sec);
    refcounted_scoped_nanotimer _rt(deserialize_nanotimer_ctrl);
    refcounted_scoped_nanotimer _rtx(serdes_nanotimer_ctrl);
    acfd fd = rootfd == -1 ? ::open(path.c_str(), O_RDONLY) :
                        ::openat(rootfd, path.c_str(), O_RDONLY);
    if(!fd){
        DIAGkey(_diskcache, "diskcache::deserialize(" << path << ") miss\n");
        *ret = reply123{}; // invalid!
        return;
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
    struct iovec iov[3];
    iov[0].iov_base = (char *)ret + reply123_pod_begin;
    iov[0].iov_len = reply123_pod_length;
    iov[1].iov_base = &content_len;
    iov[1].iov_len = sizeof(content_len);
    size_t nread = sew::readv(fd, iov, 2);
    stats.dc_deserialize_bytes += nread;
    if(nread != (iov[0].iov_len + iov[1].iov_len))
        throw se(EINVAL, fmt("diskcache::deserialize: expected to read %zd+%zd bytes.  Only got %zd\n",
                                     iov[0].iov_len, iov[1].iov_len, nread));
    if( ret->magic != ret->MAGIC ){
        complain(LOG_NOTICE, "Rejecting cache file with incorrect magic number (got %d, expected  %d): %s",
                 ret->magic, ret->MAGIC, path.c_str());
        *ret = reply123{}; // invalid
        return;
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
    ret->content.resize(content_len);
    nread = sew::read(fd, &ret->content[0], content_len);
    stats.dc_deserialize_bytes += nread;
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
    // ret->content_threeroe is not NUL-terminated, so we have to use the
    // four-argument string::compare.
    static const size_t thirtytwo = sizeof(ret->content_threeroe);
    if(threeroe(ret->content).hexdigest().compare(0, thirtytwo, ret->content_threeroe, thirtytwo) != 0){
        throw se(EINVAL, fmt("diskcache::deserialize:  threeroe mismatch:  threeroe(data): %s, threeroe(stored_in_header): %.32s. content: %zu@%p, initial bytes: %s\n",
                             threeroe(ret->content).hexdigest().c_str(), ret->content_threeroe,
                             ret->content.size(), ret->content.data(), hexdump(ret->content.substr(0, 512), true).c_str()));
    }
}

reply123
diskcache::deserialize(const std::string& path) try { 
    reply123 ret;
    deserialize_no_unlink(rootfd_, path, &ret);
    return ret;
 }catch(std::exception& e){
    // Unlink files that give us trouble deserializing?  In theory,
    // nothing in the cache is precious.  So if something trips us up,
    // it's usually better to remove it than to leave it where it will
    // trip us up again.
    //   
    auto newpath = path + "." + str(std::chrono::system_clock::now());
    ::renameat(rootfd_, path.c_str(), rootfd_, newpath.c_str());
    // If this happens often, *and* we understand why, it might be better
    // to unlink instead:
    //::unlinkat(rootfd_, path.c_str(), 0);
    // Now that we've renamed it, we might as well return a miss rather
    // than throwing an exception.
    complain(LOG_WARNING, e, "diskcache::deserialize("+rootpath_ +"/"+path+"):  file renamed to " + newpath + ".  Returning 'invalid', i.e., MISS");
    return {};
 }

void 
diskcache::serialize(const reply123& r, const std::string& path, const std::string& url){
    atomic_scoped_nanotimer _t(&stats.dc_serialize_sec);
    refcounted_scoped_nanotimer _rt(serialize_nanotimer_ctrl);
    refcounted_scoped_nanotimer _rtx(serdes_nanotimer_ctrl);
    static std::atomic<long long> rofs_defer_till{0};
    if(!should_serialize(r))
       return;
    if( rofs_defer_till > _t.started_at() ){
        stats.dc_serialize_deferred_rofs++;
        return;
    }
    DIAGkey(_diskcache, "diskcache::serialize(" << path << " now=" << ins(std::chrono::system_clock::now()) << " eno=" << r.eno << " fresh=" << r.fresh() << " expires=" << ins(r.expires) << " etag64=" << r.etag64 << ")\n");
    // A single diskcache object is used concurrently by many threads.
    // Take care that they don't step on one anothers rngs.
    static std::atomic<int> seed(0); // give a different seed to every thread.
    static thread_local std::default_random_engine eng(seed++);
    std::uniform_real_distribution<float> ureal(0., 1.);
    if(  ureal(eng) > injection_probability_ ){
        DIAGfkey(_diskcache, "diskcache::serialize:  rejected with injection_probability=%.2f\n", injection_probability_.load());
        return;
    }
    if(!r.fresh())
        stats.dc_serialize_stale++;  // used to return, but that denies a lot of swr and sie opportunities.

    std::string pathnew = path + ".new";
    acfd fd = ::openat(rootfd_, pathnew.c_str(), O_WRONLY|O_CREAT|O_EXCL, 0600);
    // O_EXCL|O_CREAT guarantees that only one thread can have a valid
    // fd for the file known as pathnew.  Any thread attempting to
    // open an existing pathnew will get a 'false' fd.  This remains
    // true until pathnew is unlink-ed or rename-ed, which means we
    // must try *very* hard to unlink or rename a successfully open-ed
    // pathnew when we're done with it (see below).
    DIAGkey(_diskcache, "diskcache::serialize opened " << pathnew << " " << fd.get() << "\n");
    if(!fd){
        switch(errno){
        case EEXIST:
            // These aren't as rare as we might hope.  It's not uncommon to
            // get back-to-back reads for the same chunk, and the second one
            // will often run into the in-progress serialization of the first.
            stats.dc_serialize_eexist++;
            // Not only that - the bandwidth was wasted!  It would take
            // some work to "fix" it though.  We'd need a whole new control
            // flow to "attach" one request to another already-in-progress
            // one. Let's count before we start writing new code...
            stats.dc_serialize_eexist_wasted_bytes += r.content.size();
            break;
        case EROFS:
            // ext family filesystems will remount themselves read-only after a certain
            // number of write errors.  Trying to write to them can only make things worse.
            // This is a condition that requires administrative intervention.  Complain
            // loudly (LOG_ERR) every 5 minutes.
            stats.dc_serialize_erofs++;
            rofs_defer_till = _t.started_at() + 300ull * 1000 * 1000 * 1000; // 5 minutes, in scoped_nanotimer's units
            complain(LOG_ERR, "diskcache::serialize EROFS.  Administrative intervention required!  Serialization will be deferred for 5 minutes");
            break;
        default:
            complain(LOG_WARNING, "diskcache::serialize failed to create %s.  errno=%m",
                   pathnew.c_str());
            stats.dc_serialize_other_failures++;
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
#if 1   // N.B.  We believe r.content is now managed correctly, even
        // if std::string is copy-on-write.  There is an O(1)
        // check in do_serialize to confirm that.  So the
        // O(content.size()) check here is probably unnecessary.
        if(threeroe(r.content).hexdigest().compare(0, 32, r.content_threeroe, 32) != 0){
            throw se(EINVAL, fmt("diskcache::serialize: threeroe mismatch: r.content.data(): %p, r.content.size(): %zu threeroe(data): %s, threeroe(in header): %.32s",
                                 r.content.data(), r.content.size(),
                                 threeroe(r.content).hexdigest().c_str(),
                                 r.content_threeroe
                                 ));
        }
#endif
        size_t wrote = sew::writev(fd, iov, 6);
        stats.dc_serializes++;
        stats.dc_serialize_bytes += wrote;
        // see comments above about O_EXCL|O_CREAT.  We have exclusive
        // access to the file known as pathnew until it has been
        // rename-ed even if we close the file descriptor associated
        // with it.
        fd.close();
        sew::renameat(rootfd_, pathnew.c_str(), rootfd_, path.c_str());
	DIAGkey(_diskcache, "diskcache::serialize wrote " << path << "\n");
    }catch(std::exception& e){
        // it's critical that we unlink pathnew.  Otherwise, we'll
        // never successfully open it again.  (see comments about
        // O_EXCL|O_CREAT).
        int ret = ::unlinkat(rootfd_, pathnew.c_str(), 0);
        if(ret && errno != ENOENT)
            complain(LOG_CRIT, "diskcache::serialize:  Unable to unlink " + pathnew + ".  Reason: %m.  Because of O_EXCL logic, it will be impossible to serialize " + path + " in the future.");
        // Unlinking path isn't strictly necessary.  The next attempt
        // to read it will almost certainly decide it needs to be
        // refreshed.  But it seems worthwhile to try to clear out as
        // much cruft as possible to avoid cascading errors.
        ret = ::unlinkat(rootfd_, path.c_str(), 0);
        if(ret && errno != ENOENT)
            complain(LOG_CRIT, "diskcache::serialize:  Unable to unlink " + path + " after serialization failure.  Reason: %m");
        std::throw_with_nested(std::runtime_error("diskcache::serialize(path=" + path + "): failed"));
    }
}
