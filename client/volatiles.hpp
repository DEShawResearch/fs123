#pragma once

#include <core123/envto.hpp>
#include <atomic>

// One-stop shopping for configuration and real-time values that may
// be updated asynchronously, e.g., by ioctl handlers, periodic
// maintenance tasks, signal handlers, "command" pipes, etc...  We
// stash them all here.  This is *not* generic code.  It's completely
// fs123-client specific.

// The 'volatiles' are *individually* atomic.  I.e., individual loads
// and stores won't be "torn".  But no promise or suggestion is made
// that more than one can be read or written atomically.

// N.B.  Most of the volatiles are initialized from environment
// variables.  To work correctly with the fs123 client's idiosyncratic
// command-line-to-environment strategy, the volatile_t should *not*
// be constructed until the filesystem 'init' callback (fs123_init) is
// running.

struct volatiles_t{
    // used in backend_http.cpp
    std::atomic<long> connect_timeout{core123::envto<long>("Fs123ConnectTimeout", 20L)};
    std::atomic<long> transfer_timeout{core123::envto<long>("Fs123TransferTimeout", 40L)};
    std::atomic<long> curl_maxredirs{core123::envto<long>("Fs123CurlMaxRedirs", 2)};
    std::atomic<bool> namecache{core123::envto<bool>("Fs123NameCache", true)};

    // See retry logic in app_mount.cpp
    std::atomic<unsigned> retry_timeout{core123::envto<unsigned>("Fs123RetryTimeout", 0)};
    std::atomic<unsigned> retry_initial_millis{core123::envto<unsigned>("Fs123RetryInitialMillis", 100)};;
    std::atomic<unsigned> retry_saturate{core123::envto<unsigned>("Fs123RetrySaturate", 1)};

    std::atomic<bool> ignore_estale_mismatch{core123::envto<bool>("Fs123IgnoreEstaleMismatch", false)};

    // Used in diskcache.cpp to control eviction.
    std::atomic<float> evict_lwm{core123::envto<float>("Fs123EvictLwm", 0.7)};
    std::atomic<float> evict_target_fraction{core123::envto<float>("Fs123EvictTargetFraction", 0.8)};
    std::atomic<float> evict_throttle_lwm{core123::envto<float>("Fs123EvictThrottleLwm", 0.9)};
    std::atomic<unsigned> evict_period_minutes{core123::envto<unsigned>("Fs123EvictPeriodMinutes", 60)};
    std::atomic<size_t> dc_maxmbytes{core123::envto<size_t>("Fs123CacheMaxMBytes", 100)};
    std::atomic<size_t> dc_maxfiles{core123::envto<size_t>("Fs123CacheMaxFiles", dc_maxmbytes*1000000/16384)};

    // Timeouts are increased when the load average-per-core exceeds this in backend_http.cpp
    std::atomic<float> load_timeout_factor{core123::envto<float>("Fs123LoadTimeoutFactor", 1.5f)};

    // things we measure from time to time about our
    // current "environment":
    const static unsigned hw_concurrency; // = std::thread::hardware_concurrency()
    // There's a lot more in struct sysinfo?  Is any of it
    // interesting?  All of it?  At the moment, all we're interested
    // in is the load average.
    std::atomic <float> load_average{0.0f};
};
