#pragma once
#include "backend123.hpp"
#include "fs123/acfd.hpp"
#include "volatiles.hpp"
#include <core123/threadpool.hpp>
#include <core123/expiring.hpp>
#include <core123/autoclosers.hpp>
#include <core123/periodic.hpp>
#include <atomic>
#include <string>
#include <thread>
#include <condition_variable>
#include <memory>
#include <random>
#include <chrono>

// diskcache isn't quite generic because it works strictly
// with cacheable objects in the form of a backend123::reply.

struct scan_result{
    size_t nbytes;
    std::vector<std::string> names;

    scan_result() : nbytes(0), names(){}
};

struct diskcache : public backend123{
    // constructor takes a root and some sizing parameters.
    diskcache(backend123*, const std::string& root,
              uint64_t hash_seed_first, volatiles_t& vols);
    void set_upstream(backend123* upstream) { upstream_ = upstream; }
    bool refresh(const req123& req, reply123*) override; 
    std::ostream& report_stats(std::ostream& os) override;
    std::string get_uuid() override;

    // FIXME - hash, serialize, and deserialize are
    // 'internal', so they could be protected.  But that would make
    // unit-testing harder.

    // the hash function return a path relative to root.
    std::string hash(const std::string&);
    // serialize and deserialize work with backend::reply's
    // which have an errno, a struct stat, and a ttl.
    // serialize may do nothing if policy doesn't permit
    // or desire serialization of the argument.
    // serialization requires converting the 'ttl' into
    // a 'good_till', so that it ages out naturally.
    void serialize(const reply123&, const std::string&, const std::string&);

    // NOTE: deserialize removes anything from the cache that it cannot
    // parse!
    reply123 deserialize(const std::string&);

    // deserialize_no_unlink - convenience routine for read-only
    // deserialization and debugging.
    //
    // N.B.  If it throws, *reply and *returlp are not guaranteed to
    // reflect the on-disk data.
    static void deserialize_no_unlink(int rootfd, const std::string& path,
                                      reply123* reply,
                                      std::string *returlp = nullptr);


protected:
    void evict(size_t Nevict, size_t dir_to_evict, scan_result& sr);
    void check_root();
    std::string reldirname(unsigned i) const;
    std::chrono::system_clock::duration evict_once();
    scan_result do_scan(unsigned dir_to_evict) const;

    backend123* upstream_;
    acfd rootfd_;
    size_t Ndirs_;
    size_t dir_to_evict_ = 0;
    size_t files_evicted_ = 0;
    size_t files_scanned_ = 0;
    size_t bytes_scanned_ = 0;
    size_t overfull_ = 0;
    std::default_random_engine urng_; // not seeded.  Should we care...
    std::string rootpath_; // only used in diagnostics and error reports
    unsigned hexdigits_;
    std::atomic<float> injection_probability_;
    std::unique_ptr<core123::periodic> evict_thread_;
    // construct the hashseed from the baseurl in the constructor.
    // This allows different baseurls to coexist in the same diskcache
    // even if they have the same relative paths.
    const std::pair<uint64_t,uint64_t> hashseed_;
    // machinery for backgrounding refresh and serialization:
    void maybe_bg_upstream_refresh(const req123& req, const std::string& path, reply123* r);
    void upstream_refresh(const req123& url, const std::string& path, reply123* r, bool already_detached, bool usable_if_error);
    // The detached methods are intended to be called in a lambda submit-ed to
    // the threadpool, e.g.,
    //    tp->submit([=]() mutable { detached_upstream_refresh(r,p);})
    // The future returned from the tp->submit() is discarded, so there's
    // nobody waiting for any exceptions thrown by detached_whatever.
    // To emphasize this, we declare them noexcept, even though a
    // thrown exception wouldn't actually do any harm.
    void detached_upstream_refresh(req123& req, const std::string& path, reply123* r) noexcept ;
    void do_serialize(const reply123* r, const std::string& path, const std::string& urlstem, bool already_detached);
    std::unique_ptr<core123::threadpool<void>> tp;
    volatiles_t& vols_;
    std::string uuid;
    bool foreground_serialize;
};

#define DISKCACHE_STATISTICS \
STATISTIC(dc_hits)\
STATISTIC(dc_stale_while_revalidate)\
STATISTIC(dc_maybe_rf_too_soon)\
STATISTIC(dc_maybe_rf_started)\
STATISTIC(dc_maybe_rf_retired)\
STATISTIC(dc_must_refresh)\
STATISTIC(dc_detached_refresh_failures)\
STATISTIC(dc_rf_304)\
STATISTIC(dc_rf_304_bytes)\
STATISTIC(dc_rf_200)\
STATISTIC(dc_rf_stale_if_error)\
STATISTIC(dc_rf_disconnected_skipped)\
STATISTIC(dc_wasted_copy_reply_bytes)\
STATISTIC(dc_serializes)\
STATISTIC(dc_serialize_bytes)\
STATISTIC_NANOTIMER(dc_serialize_sec)\
STATISTIC_NANOTIMER(dc_serialize_inuse_sec)\
STATISTIC(dc_serialize_eexist)\
STATISTIC(dc_serialize_erofs)\
STATISTIC(dc_serialize_deferred_rofs)\
STATISTIC(dc_serialize_eexist_wasted_bytes)\
STATISTIC(dc_serialize_other_failures)\
STATISTIC(dc_serialize_stale)\
STATISTIC(dc_deserialize_bytes)\
STATISTIC_NANOTIMER(dc_deserialize_sec)\
STATISTIC_NANOTIMER(dc_deserialize_inuse_sec)\
STATISTIC(dc_updates)\
STATISTIC(dc_update_bytes)\
STATISTIC_NANOTIMER(dc_update_sec)\
STATISTIC_NANOTIMER(dc_update_inuse_sec)\
STATISTIC_NANOTIMER(dc_serdes_inuse_sec)\
STATISTIC(dc_failed_updates)\
STATISTIC(dc_eviction_dirscans)\
STATISTIC(dc_eviction_evicted)\
STATISTIC(dc_recently_refreshed_appended)\
STATISTIC(dc_recently_refreshed_matched)\
STATISTIC(dc_recently_refreshed_erased)
