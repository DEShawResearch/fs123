#pragma once
#include <chrono>
#include <atomic>
#include <utility>
#include <mutex>

// scoped_nanotimer is a trivial RAII class that performs
// high-resolution timing of a scope.  It records a start time at
// object creation and when restart() is called, and returns the time
// since start on each elapsed() and restart() invocation.  On
// destruction, it adds the time since start to its 'accumulator'
// pointer, if non-NULL.  The accumulator is assigned at construction
// and may be changed with the set_accumulator() method.
//
// One way to use it is to accumulate the amount of time spent in all
// calls to a function (or any other scope).  For example:
//
//   long long foo_time = 0;
//   foo()
//   {
//     scoped_nanotimer st(&foo_time);
//     ...foo can return normally, or via an exception ...
//   }
//
// At the end of st's scope (in this example, when foo returns)
// foo_time (st's accumulator) will be incremented by the number of
// nanoseconds since st was constructed.
//
// scoped_nanotimer may also be used as a stopwatch with explicit
// invocation of methods to restart, and read the timer.  E.g.
//
//    scoped_nanotimer st;
//    ...do some work that needs to be timed...
//    long long e1 = st.elapsed();
//    ... more work ...
//    long long e2 = st.restart(); // ns since constructor
//    ... more work ...
//    long long e3 = st.elapsed(); // ns since restart
//
// Note that elapsed() and restart() return the number of nanoseconds
// since construction or the last restart, not the amount of time
// since the last call to elapsed.
//
// The restart() method restarts the clock.  Subsequent calls to
// elapsed() will measure the time since restart().  The destructor
// will record the time since restart() in the destination pointer.
//
// The accumulate() method calls restart() and then adds the elapsed
// time to the current destination, if any.
//
// atomicscoped_nanotimer is identical, except that the target of
// the accumulation is a std::atomic<long long>, so it
// is safe to use in multi-threaded environments

// First, we define _nanotimer, which isn't really scoped at all (at
// least not in an interesting way).  It just records the start time,
// tstart_, and provides the elapsed() and restart() methods.

namespace core123{
class _nanotimer {
    using steady = std::chrono::steady_clock;
#ifdef __clang__
    // This *may* be a bug in clang... If we try to say:
    //   std::atomic<std::chrono::steady_clock::time_point>
    // clang-4.0.0 complains about:
    // /proj/desres/root/CentOS6/x86_64/gcc/5.2.0-31x/lib/gcc/x86_64-unknown-linux-gnu/5.2.0/../../../../include/c++/5.2.0/atomic:185:7: error: exception
    //      specification of explicitly defaulted default constructor does not match the calculated one
    //      atomic() noexcept = default;
    //
    // I'm not sure who's at fault here.  But we can work around it by providing
    // a wrapper that has noexcept constructors and enough 'pass-through' methods
    // to leave the rest of the code mostly intact.
    struct _steady_tp{
        steady::time_point v;
        _steady_tp() noexcept{}
        _steady_tp(steady::time_point _v) noexcept : v(_v){}
        auto time_since_epoch() const { return v.time_since_epoch(); }
        operator steady::time_point() { return v; }
    };
#else
    using _steady_tp = steady::time_point;
#endif    
protected:
    std::atomic<_steady_tp> tstart_;

    template <typename DUR>
    static long long dur2ll(DUR d){
        return std::chrono::duration_cast<std::chrono::duration<long long, std::nano> >(d).count();
    }

public:
    _nanotimer() : tstart_(steady::now()){}

    long long elapsed() const {
	auto t = steady::now();
        steady::time_point ts = tstart_.load();
	return dur2ll(t-ts);
    }
    long long restart() {
        auto t = steady::now();
        steady::time_point ts = tstart_.exchange(t);
        return dur2ll(t - ts);
    }
    long long started_at() const {
        return dur2ll(tstart_.load().time_since_epoch());
    }
};

// _scoped_nanotimer adds the scoping on top of _nanotimer.  The
// accum_type template parameter must be either long long or
// std::atomic<long long>.  It's templated so I don't
// have to write out the same code twice.
template <typename accum_type>
class _scoped_nanotimer : public _nanotimer{
    static_assert( std::is_same<accum_type, long long>::value ||
                   std::is_same<accum_type, std::atomic<long long> >::value,
                   "the accum type must be either long long or std:atomic<ll>");
    accum_type* accum_;
        
public:
    _scoped_nanotimer(accum_type* p = nullptr) : _nanotimer(), accum_(p){}

    _scoped_nanotimer(const _scoped_nanotimer&) = delete;
    _scoped_nanotimer& operator=(const _scoped_nanotimer&) = delete;
    _scoped_nanotimer(_scoped_nanotimer&& rhs) noexcept : _nanotimer(rhs){
        accum_ = rhs.accum_;
        rhs.accum_ = nullptr;
    }
    _scoped_nanotimer& operator=(_scoped_nanotimer&& rhs) noexcept
    {
        // Howard Hinnant argues that we don't need the if(this==&rhs) test here:
        //  http://stackoverflow.com/questions/9322174/move-assignment-operator-and-if-this-rhs
        _nanotimer::operator=(std::move(rhs));
        accum_ = rhs.accum_;
        rhs.accum_ = nullptr;
        return *this;
    }
    void set_accumulator(accum_type *p = nullptr) {
        accum_ = p;
    }
    auto accumulate() -> decltype(restart()){
        auto e = restart();
        if(accum_)
            *accum_ += e;
        return e;
    }
    ~_scoped_nanotimer() {
        if(accum_)
            *accum_ += elapsed();
    }
};

// Finally, typedefs for the two permissible template
// instantiations of _scoped_nanotimer:
using scoped_nanotimer = _scoped_nanotimer<long long>;
using atomic_scoped_nanotimer = _scoped_nanotimer<std::atomic<long long>>;

// Consider two threads downloading data:
//
//   t=0             t=0.75
//    |----------------|         thread 1 50MB
//         |----------------|    thread 2 50MB
//        t=0.25           t=1

// The system delivered a constant 100MB/s for 1 second: the first
// 25MB in 1/4 sec to thread 1, then 50MB in the next half-second
// to both threads, and then 25MB in the last 1/4sec to thread2.
// But if we divide the total data (100MB) by the total time
// recorded by both threads, (1.5sec), the result is only 66MB/s.
//
// To get the actual delivered bandwidth, we need a timer that
// starts when thread1 begins downloading data and ends when
// thread2 finishes downloading data.  Such a timer would record
// 1sec, and dividing the data volume by that timer would
// correctly measure the delivered data rate.
//
// The refcounted_scoped_timer does the job.  To use it, create
// one refcounted_scoped_timerCtrl control-block with a reference
// to the statistic to be accumulated, e.g.,
// 
//     refcounted_scoped_timerCtrl ctrlblk(stats.something);
//
// Then in every scope to be timed, add:
//
//     refcounted_scoped_timer(ctrlblk);
//
// The timer will start when the first thread enters the
// scope, and will stop (and accumulate) when the last thread
// exits the scope.

struct refcounted_scoped_nanotimer_ctrl{
     // Is there a lock-free strategy?
     std::mutex mtx;
     std::chrono::steady_clock::time_point start;
     int refcnt;
     std::atomic<long long>& p;
     refcounted_scoped_nanotimer_ctrl(std::atomic<long long>& _p):
         refcnt(0), p(_p)
     {}
};
struct refcounted_scoped_nanotimer{
     refcounted_scoped_nanotimer_ctrl& ctrl;
     refcounted_scoped_nanotimer(refcounted_scoped_nanotimer_ctrl& _ctrl) :
         ctrl(_ctrl)
     {
         std::lock_guard<std::mutex> lg(ctrl.mtx);
         if(ctrl.refcnt++ == 0)
             ctrl.start = std::chrono::steady_clock::now();
     }
     long long started_at() const {
         using namespace std::chrono;
         return duration_cast<nanoseconds>(ctrl.start.time_since_epoch()).count();
     }
     ~refcounted_scoped_nanotimer() {
         std::lock_guard<std::mutex> lg(ctrl.mtx);
         using namespace std::chrono;
         if(--ctrl.refcnt == 0)
             ctrl.p += duration_cast<nanoseconds>(steady_clock::now() - ctrl.start).count();
     }
};

} // namespace core123

