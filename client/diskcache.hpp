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

    // The fancy_sharing option enables inter-process communication
    // between multiple clients sharing a single disk cache.  When
    // fancy sharing is enabled, only one 'custodian' process is
    // responsible for evictions, and the others poll for updates to
    // the 'injection_probability' at regular intervals.  Fancy and
    // non-fancy clients can safely share the same cache.
    diskcache(std::unique_ptr<backend123>, const std::string& root,
              uint64_t hash_seed_first, bool fancy_sharing, volatiles_t& vols);
    bool refresh(const req123& req, reply123*) override; 
    std::ostream& report_stats(std::ostream& os) override;
    // override set_disconnected(bool) so it passes the news upstream.
    bool set_disconnected(bool d) override { upstream_->set_disconnected(d); return backend123::set_disconnected(d); }

    // FIXME - hash, serialize, deserialize and update_expiration are
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

    // convenience routine for read-only deserialization
    static reply123 deserialize_no_unlink(int rootfd, const std::string& path,
                                          std::string *returlp = nullptr);


protected:
    void evict(size_t Nevict, size_t dir_to_evict, scan_result& sr);
    void check_root();
    std::string reldirname(unsigned i) const;
    std::chrono::system_clock::duration evict_once();
    scan_result do_scan(unsigned dir_to_evict) const;

    // Members and methods for 'fancy_sharing':
    static constexpr char const * status_filename_ = "statusv0";
    void write_status(float) const;
    float read_status(); // returns the recommended value of injection_probability_
    bool custodian_check();
    acfd statusfd_;
    bool custodian_ = false;

    std::unique_ptr<backend123> upstream_;
    acfd rootfd_;
    size_t Ndirs_;
    size_t dir_to_evict_ = 0;
    size_t files_evicted_ = 0;
    size_t files_scanned_ = 0;
    size_t bytes_scanned_ = 0;
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
    void maybe_bg_upstream_refresh(const req123& req, const std::string& path);
    void upstream_refresh(const req123& url, const std::string& path, reply123* r, bool already_detached, bool usable_if_error);
    // The detached methods are intended to be called in a lambda submit-ed to
    // the threadpool, e.g.,
    //    tp->submit([=]() mutable { detached_upstream_refresh(r,p);})
    // The future returned from the tp->submit() is discarded, so there's
    // nobody waiting for any exceptions thrown by detached_whatever.
    // To emphasize this, we declare them noexcept, even though a
    // thrown exception wouldn't actually do any harm.
    void detached_upstream_refresh(req123& req, const std::string& path) noexcept ;
    void detached_serialize(const reply123&, const std::string&, const std::string&) noexcept;
    void detached_update_expiration(const reply123& r, const std::string& path) noexcept;
    std::unique_ptr<core123::threadpool<void>> tp;
    volatiles_t& vols_;
};

