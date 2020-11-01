#include "openfilemap.hpp"
#include "fuseful.hpp"
#include "app_mount.hpp"
#include "inomap.hpp"
#include <core123/stats.hpp>
#include <core123/complaints.hpp>
#include <core123/exnest.hpp>
#include <core123/diag.hpp>
#include <core123/sew.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/throwutils.hpp>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <set>
#include <map>

using namespace core123;

namespace{
#define STATS_INCLUDE_FILENAME "openfilemap_statistic_names"
#define STATS_STRUCT_TYPENAME openfilemap_stats_t
#include <core123/stats_struct_builder>
openfilemap_stats_t stats;

auto _ofmap = diag_name("ofmap");
auto _shutdown = diag_name("shutdown");

// Our basic data structure is a hybird priority-queue/map.  We need
// to do the following quickly (O(1) or O(lg(N))
//  - insert reference-counted items given only an ino, returning a int64
//    'handle'.
//  - delete reference-counted items given a handle and an ino.
//  - quickly access the earliest 'expiration time'.
//  - update expiration times (resulting in a new order).
//  - run a thread that wakes up when the earliest expiration time
//    is reached.

// Use a multiset for the access-the-earliest data structure, ofpq.
// It's sorted.  We can delete from the middle.  And it's iterators
// aren't invalidated by churn, other than erasure of the specific
// pointed-to element.

// We'll also need a plain-old map, ofmap, keyed by ino.

// The data structures have some invariants that are guaranteed
// whenever the lock is not held.:
//  - every mrecord in ofmap has an miter that "points" to itself
//    in the ofmap.  We use miter to 'erase' the mrecord.
//  - every mrecord in ofmap has a qiter that is either
//    not-dereferenceable or points to an entry in the ofpq whose
//    miter points back.
//  - every pqrecord in ofpq has an miter that points to an mrecord in
//    ofmap.  The mrecord's qiter points back to the pqrecord.

// mrecord - the mapped_type of the ofmap: consists of an iterator
// pointing into ofpq, a reference count and a self-referential
// iterator.
//
// The 'miter' field is weird.  We need the ability to 'erase'
// an mrecord from the ofmap, given only a pointer to the mrecord
// itself.  But erase needs an iterator.  So we stash an iterator
// *inside* the the mrecord, and use that.  Something smells
// funny about this, but it seems to be working.
//
// Since the mrecord is, effectively, our fi->fh, anything else
// we might want to record at open-time for use at read-time
// has to go in the mrecord as well.  (Nothing for now)

struct pqrecord;
struct mrecord;
using ofmap_t = std::map<fuse_ino_t, mrecord>;
using ofpq_t = std::multiset<pqrecord>;

struct mrecord{
    int refcnt;
    // We'd really like to initialize qiter to a 'nulliter', i.e., an
    // iterator that reliably compares as != to any dereferenceable
    // iterator, and also reliably compares equal to any other
    // nulliter.  It's *possible* that specific implementations of
    // ofpq.end() satisfy these requirements, but I can't find a
    // guarantee of that in the standards.  So, we carry along
    // a bool that tells us whether qiter is dereferenceable.
    ofpq_t::iterator qiter;
    bool qiter_dereferenceable;
    ofmap_t::iterator miter;
    mrecord() : refcnt(0), qiter_dereferenceable(false)
    {}
    // mrecords should "stay put" where we emplace them.  Don't allow
    // any accidental copies.
    mrecord(const mrecord&) = delete;
    mrecord& operator=(const mrecord&) = delete;
};

// pqrecord - has an expires field copied directly from a reply123 and
// an iterator that points into the ofmap.
struct pqrecord{
    clk123_t::time_point expires;
    ofmap_t::iterator miter;
    bool operator<(const pqrecord& rhs) const{
        return expires < rhs.expires;
    }
    // when we construct a pqrecord, it expires 'no_sooner_than' in
    // the future.  This defends against scan() getting stuck in a
    // loop with stale replies reinserting themselves at the front.
    // We could skip this if we were sure that replies would never be stale,
    // but stale-if-error (not to mention buggy or misconfigured proxy
    // caches) might give us stale replies even though we requested
    // max-stale=0.  The specific value (750msec) is arbitrary and
    // lacks a clear justification.
    static constexpr auto no_sooner_than = std::chrono::milliseconds(750);
    // If the caller happens to know the current time, it can be
    // passed as the third argument.  Otherwise, we'll call
    // clk_t::now.
    pqrecord(const reply123& _r, ofmap_t::iterator _miter, clk123_t::time_point now = clk123_t::now()):
        expires(std::max(_r.expires, now + no_sooner_than)),
        miter(_miter)
    {
        if(_r.expires < now)
            stats.of_pq_stale_ctors++;
    }
    // without a reply123 argument, the constructor sets 'expires' to
    // -infinity.  When emplace'ed, such a record will automatically
    // be at the front of the pq.
    pqrecord(ofmap_t::iterator _miter):
        expires(clk123_t::time_point::min()),
        miter(_miter)
    {}
};
     
constexpr std::chrono::milliseconds pqrecord::no_sooner_than;

// Some unavoidable machinery:
//
// - we certainly need a background thread
// a map, a priority queue (implemented as a multiset) and a mutex.
std::thread scanthread;
ofmap_t ofmap;
ofpq_t ofpq;
std::mutex mtx;

// - to shut down cleanly, and to wake up promptly, we need some
//   global state that says whether we're done and a condition
//   variable to coordinate between threads.
bool loopdone;
std::condition_variable loopcv;

int decrefcnt(mrecord& mr){
    // N.B.  The lock must be held when this is called!
    int ret = --mr.refcnt;
    if(ret == 0){
        if(mr.qiter_dereferenceable)
            ofpq.erase(mr.qiter);
        ofmap.erase(mr.miter); // erases *mr!
    }
    return ret;
}

// scan - pop recently expired entries from the ofpq to refresh and
//  perhaps fuse_notify_inval them if they have changed.
void scan(){
    std::unique_lock<std::mutex> lk(mtx);
    while(!ofpq.empty()){
        auto p = ofpq.begin();
        if(clk123_t::now() < p->expires)
            break;
        auto mi = p->miter;
        mrecord& mr = mi->second;
        if(mr.miter != mi){
            complain(LOG_ERR, "openfilemap::scan:  mr.iter != mi.  Something is very wrong.");
            break;
        }
        DIAGfkey(_ofmap, "scan: popping entry expired at: %.6f (%.6f) ofpq.size (before pop): %zu\n", tp2dbl(p->expires), tpuntildbl(p->expires), ofpq.size());
        ofpq.erase(p);
        // maintain the invariant - we've erased p, so mr.qiter is
        // no longer dereferenceable
        mr.qiter_dereferenceable = false;
        auto ino = mi->first;
        // Release the lock so we don't lock out opens and releases
        // while we (possibly) slowly talk to upstream servers.  It's
        // even possible that we'd deadlock if held the lock while
        // doing the inval.
        //
        // But first, bump the refcnt so that a release while we're
        // not holding the lock doesn't knock mi out from under us
        mr.refcnt++;
        reply123 r;
        lk.unlock();
        try{
            r = begetattr_fresh(ino);
            stats.of_getattrs++;
            DIAGfkey(_ofmap, "scan:  ino=%lu, newreply.expires at: %.6f (%.6f)\n",
                     ino, tp2dbl(r.expires), tpuntildbl(r.expires));
        }catch(std::exception& e){
            complain(LOG_WARNING, e, "begetattr threw in openfilemap::scan.  Did an open file get moved out from under us? ino=%lu", ino);
            stats.of_throwing_getattrs++;
        }
        bool must_notify;
        if(r.eno == 0){
            auto r_validator = validator_from_a_reply(r);
            try{
                auto old_validator = ino_update_validator(ino, r_validator);
                // With a 7.1 client, ino_update_validator unconditionally
                // updates the validator in the inomap.  With a 7.2
                // client, the inomap is updated only if
                // r_validator>old_validator.  But with a 7.2 client, if
                // r_validator<old_validator, i.e., if the server
                // violates validator monotonicity, then
                // ino_update_validator throws an exception.
                must_notify = (old_validator != r_validator);
            }catch(std::exception& e){
                // The server(s) are sending us non-monotonic
                // validators.  We don't know what to believe.  So we
                // act as if the server err-ed out for some other
                // reason.  We'll notify the kernel to flush this ino
                // and then 'continue' with the loop without fiddling
                // with ofpq.  Maybe the server(s) will get their act
                // together and things will eventually settle down (?).
                r.eno = EIO;  // anything non-zero
                must_notify = true;  // unnecessary?  gcc6 thinks it 'may be used uninitialized' otherwise.
                complain(LOG_WARNING, e, "openfilemap::scan: ino=%lu. Pretend that reply.eno=%d (EIO) to avoid corrupting the openfilemap and inomap.", (unsigned long)ino, EIO);
            }
        }
        if(r.eno){
            stats.of_failed_getattrs++;
            complain(LOG_WARNING, fmt("openfilemap::scan(): ino=%lu (%s) non-zero eno (%d).  Notifying kernel to invalidate this ino", (unsigned long)ino, ino_to_fullname_nothrow(ino).c_str(), r.eno));
            // It's tempting to elide the notify_inval if
            // newreply.eno==ENOENT here, as would happen if the file
            // were unlinked on the server side within the expiration
            // window.  Eliding the notify_inval in that case might
            // save the client from some unnecessary ESTALEs.  BUT -
            // we can't do that because the server *might* also have
            // modified the file before unlinking it and we'd never
            // know.  Our kernel cache pages would reflect the
            // pre-change/pre-unlink data, but POSIX rules say we
            // should return post-change/pre-unlink data.  We could
            // try to rewrite the consistency rules to give us
            // wiggle-room on this point, but until then, we have to
            // invalidate.
            must_notify = true;
        }
        if(must_notify){
            DIAGfkey(_ofmap, "getattr failed or validator changed. Calling notify_inval(ino=%lu)\n", ino);
            stats.of_notify_invals++;
            lowlevel_notify_inval_inode(ino, 0, 0);
        }
            
        lk.lock();
        // While we were away ... both register and release may have
        // been called, possibly multiple times.  We bumped the refcnt
        // before releasing the lock, so mi could not have been
        // erased, even if release was called.  Now, it's time to
        // decrement it back to its "normal" value, with the possible
        // side-effect of erasing mi, and if mi was erased, or if we
        // got an error from begetattr, we're done with this pqrecord.
        if(decrefcnt(mr) == 0 || r.eno){
            DIAGf(_ofmap, "scan:  newreply ino=%lu refcount(mr) == 0 || r.eno=%d.  Not reinserting", ino, r.eno);
            continue;
        }
        DIAGfkey(_ofmap, "emplacing newreply in ofpq\n");
        stats.of_pq_reinserted++;
        if(mr.qiter_dereferenceable){
            // uncommon... somebody must have registered another
            // instance of this ino while we weren't holding the lock.
            // We're now looking at two possible expirations, the one
            // that was emplaced when we weren't looking and the one
            // in newreply.  So what to do???  Keep the new one (i.e.,
            // the one we just got).
            stats.of_pq_scanraces++;
            ofpq.erase(mr.qiter);
        }
        mr.qiter = ofpq.emplace(r, mi);
        mr.qiter_dereferenceable = true;
    }
    DIAGfkey(_ofmap, "finished scanloop with %zu entries in ofpq\n", ofpq.size());
    if(_ofmap && !ofpq.empty()){
        auto p = ofpq.begin();
        DIAGf(1, "next expiration at: %.6f (%.6f)\n", tp2dbl(p->expires), tpuntildbl(p->expires));
    }
}

// scanloop - More boilerplate ... loop forever, waking up to call
//   scan() only when the clock ticks past next_scan() or loopdone is
//   set.
void scanloop(){
    while(true)
        try {
            std::unique_lock<std::mutex> lk(mtx);
            if(loopdone)
                break;
            auto b = ofpq.begin();
            if(b == ofpq.end()){
                loopcv.wait(lk);
            }else{
                loopcv.wait_until(lk, std::max(b->expires, clk123_t::now()) + std::chrono::milliseconds(750));
            }
            stats.of_wakeups++;
            // The extra 750msec means we don't cycle too fast, allows
            // for clock skew and gives swr refreshes time to finish.
            // Too high?  Too low?  Config option?
            //
            // spurious wakeups are ok.  We'll just scan the ofpq
            // (which will probably do nothing) and loop back around
            // to wait again.
            if(loopdone)
                break;
            // scan does some tricky lock management itself, so
            // release it before calling
            lk.unlock();
            scan();
        }catch(std::exception& e){
            complain(e, "scanloop threw an exception.  This is probably very bad");
            DIAGfkey(_ofmap, "scanloop threw an exception.  This is probably very bad: %s", e.what());
        }
    complain(LOG_NOTICE, "openfilemap::scanloop:  broke out of scan loop.  We must be shutting down\n");
    if(!ofpq.empty())
        complain(LOG_WARNING, "openfilemap::scanloop exiting with ofpq non-empty.  Reads on open files will get ENOTCONN after shutdown.");
    if(!ofmap.empty())
        complain(LOG_WARNING, "openfilemap::scanloop exiting with ofmap non-empty.  Reads on open files will get ENOTCONN after shutdown.");
}

} // namespace <anonymous>

// The public API:

// _startscan and _stopscan are generic boilerplate.
void openfile_startscan(){
    scanthread = std::thread(scanloop);
}

void openfile_stopscan(){
    std::unique_lock<std::mutex> lk(mtx);
    DIAGfkey(_ofmap||_shutdown, "openfile_stopscan - set loopdone and notify cv\n");
    loopdone = true;
    loopcv.notify_one();
    lk.unlock();
    DIAGfkey(_ofmap||_shutdown, "joining scan thread loopdone = %d\n", loopdone);
    if(scanthread.joinable())
        scanthread.join();
    DIAGfkey(_ofmap||_shutdown, "scanthread joined.  We're done here.\n");
    complain(LOG_NOTICE, "openfile_stopscan:  scanthread joined.  Scanning stopped");
}

// _register and _release are tricky.
uint64_t
openfile_register(fuse_ino_t ino, const reply123& r){
    if(r.eno)
        throw se(EIO, "openfile_register called with r.eno!=0.  This shouldn't happen");
    std::lock_guard<std::mutex> lgd(mtx);
    decltype(ofmap)::iterator mi;
    bool created;
    std::tie(mi, created) = ofmap.emplace(std::piecewise_construct, std::make_tuple(ino), std::make_tuple() );
    auto& mr = mi->second;  // refers into the map - regardless of created
    if(created)
        mr.miter = mi; // set the self-reference

    try{ // don't leak the new entry.
        if(!mr.qiter_dereferenceable){
            // We get here normally when created is true, and rarely
            // when an earlier, transient begetattr error caused us
            // to 'continue' out of the scan loop without restoring
            // qiter to a dereferenceable state.
            mr.qiter = ofpq.emplace(r, mi);
            mr.qiter_dereferenceable = true;
        }
        // we know mr.qiter is dereferenceable now
        if( mr.qiter->miter != mi ){
            complain("inconsistent iterators:  mr.qiter->miter != mi.  This can't happen.  We're probably leaking memory and may be dereferencing bogus pointers/iterators");
            throw se(EIO, "inconsistent iterators in ofmap/ofpq");
        }

        mr.refcnt++;
        if(mr.qiter->expires != r.expires){
            // the expiration time has changed.  Update the ofpq.
            DIAGfkey(_ofmap, "old entry's expiration time changed.  erase.\n");
            ofpq.erase(mr.qiter);
            mr.qiter = ofpq.emplace(r, mi);
        }
        if( mr.qiter == ofpq.begin() ){
            DIAGfkey(_ofmap, "openfile_register:  newly registered qi as front of ofpq.  Call loopcv.notify_one()\n");
            loopcv.notify_one();
        }
        return reinterpret_cast<uint64_t>(&mr);  // will be assigned to fi->fh by caller
    }catch(std::exception& e){
        if(created)
            ofmap.erase(mi);
        std::throw_with_nested(std::runtime_error("ofmap_register with created = " + std::to_string(created)));
    }
}

// openfile_expire_now - called by the read callback when it notices
// that a validator in an /f reply is newer than the validator in the
// inomap.  I.e., the kernel's cached content is probably out-of-date.
// We find the corresponding entry in the ofpq, change its expiration
// to -infinity, re-insert it (at the top), and notify the condition
// variable to wake up the scan loop.  The scan loop will actually
// handle the cache flush and any other consequences.
void openfile_expire_now(fuse_ino_t ino, uint64_t fifh){
    if(fifh == 0){
        complain(LOG_WARNING, "openfile_expire_now called with fifh==0.  How?");
        return;
    }
    std::lock_guard<std::mutex> lgd(mtx);
    stats.of_immediate_expirations++;
    mrecord& mr = *reinterpret_cast<mrecord*>(fifh);
    if( ino != mr.miter->first || &mr != &mr.miter->second )
        throw se(EIO, "openfile_expire_now: mr.miter does not 'point' back to (ino,mr).  Something is very wrong");
    DIAGfkey(_ofmap, "openfile_expire_now(ino=%lu, fifh=%#lx) refcnt=%d%s\n",
             (unsigned long)ino,
             (unsigned long)fifh, mr.refcnt,
             mr.qiter_dereferenceable?"":", qiter not dereferencable.  Do nothing");
    if(mr.qiter_dereferenceable){
        ofpq.erase(mr.qiter);
        mr.qiter = ofpq.emplace(mr.miter); // at front of ofpq - see comment in ctor
        loopcv.notify_one();
    }
}

void openfile_release(fuse_ino_t ino, uint64_t fifh){
    if(fifh==0)
        return;  // it wasn't registered - don't release it.
    std::lock_guard<std::mutex> lgd(mtx);
    mrecord& mr = *reinterpret_cast<mrecord*>(fifh);
    if( ino != mr.miter->first || &mr != &mr.miter->second )
        throw se(EIO, "openfilemap_release: mr.miter does not point back to (ino,mr).  Something is very wrong");
    DIAGfkey(_ofmap, "openfile_release(ino=%lu, fifh=%#lx) refcnt=%d\n",
             (unsigned long)ino,
             (unsigned long)fifh, mr.refcnt);
    decrefcnt(mr); // might erase miter
}

std::string
openfile_report(){
    std::lock_guard<std::mutex> lg(mtx);
    std::ostringstream oss;
    oss << "ofpq_size: " << ofpq.size() << "\n"
        << "ofmap_size: " << ofmap.size() << "\n"
        << stats;
    return oss.str();
}
