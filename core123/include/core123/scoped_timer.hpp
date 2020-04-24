#pragma once
#include <chrono>
#include <atomic>
#include <utility>
#include <mutex>

// scoped_timer is scoped_nanotimer after drinking the std::chrono
// kool-aid.  I.e., it's returns std::chrono::durations rather than
// unsigned long long nanoseconds...

// First, we define Timer<Clk_T>, which may be used as a stopwatch
// with explicit invocation of methods to restart, and read the timer.
// E.g.
//
//    Timer tt;
//    ...do some work that needs to be timed...
//    auto e1 = tt.elapsed();
//    ... more work ...
//    auto e2 = tt.restart(); // ns since constructor
//    ... more work ...
//    auto e3 = tt.elapsed(); // ns since restart
//
// Note that elapsed() and restart() return the time (duration)
// since construction or the last restart, not the amount of time
// since the last call to elapsed.
//
// The restart() method restarts the clock.  Subsequent calls to
// elapsed() will measure the time since restart().  The destructor
// will record the time since restart() in the destination pointer.
//
// scoped_timer is a trivial RAII class that uses Timer to perform
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
//   scoped_timer::accum_type foo_time;
//   foo()
//   {
//     scoped_timer st(&foo_time);
//     ...foo can return normally, or via an exception ...
//   }
//
// At the end of st's scope (in this example, when foo returns)
// foo_time (st's accumulator) will be incremented by the elapsed
// time since st was constructed.
//
// The accumulate() method calls restart() and then adds the elapsed
// time to the current destination, if any.
//
// In addition, scoped_timer has the same elapsed() and restart()
// methods as Timer, so it can be used as an (unscoped) stopwatch.
//
// atomicscoped_timer is identical, except that the target of the
// accumulation is a std::atomic<ClkT::duration>.  Multiple
// atomicscoped_timers in multiple threads may safely share a single
// accumulator.  Nevertheless, the non-const methods of any particular
// atomicscoped_timer (i.e., restart and accumulate) are not MT-safe.

namespace core123{
template <typename ClkT = std::chrono::steady_clock>
class timer {
public:
    using clk_type = ClkT;
    using duration = typename clk_type::duration;
    using time_point = typename clk_type::time_point;
    timer(time_point asif_now=ClkT::now()) : tstart_{asif_now}{}
    duration elapsed(time_point asif_now = ClkT::now()) const {
        return asif_now - tstart_;
    }
    duration restart(time_point asif_now = ClkT::now()) {
        auto ts = tstart_;
        tstart_ = asif_now;
        return asif_now - ts;
    }
    auto started_at() const {
        return tstart_;
    }
private:
    typename ClkT::time_point tstart_;
};

// atomic_timer: like timer, but all methods are thread-safe.  Also
//  has an is_lock_free() method and an is_always_lock_free static
//  bool constexpr.
template <typename ClkT = std::chrono::steady_clock>
class atomic_timer{
public:
    using clk_type = ClkT;
    using duration = typename clk_type::duration;
    using time_point = typename clk_type::time_point;
    atomic_timer(time_point asif_now = ClkT::now()) : tstart_{asif_now}{}
    bool is_lock_free() const noexcept { return tstart_.is_lock_free(); }
#if __cpp_lib_atomic_is_always_lock_free >= 201603
    static constexpr bool is_always_lock_free = std::atomic<time_point>::is_always_lock_free;
#endif
    duration elapsed(time_point asif_now = ClkT::now()) const {
        return asif_now - tstart_.load();
    }
    duration restart(time_point asif_now = ClkT::now()){
        auto ts = tstart_.exchange(asif_now);
        return asif_now - ts;
    }
    time_point started_at() const {
        return tstart_.load();
    }
protected:
    std::atomic<time_point> tstart_;
};

// Define operator+= for:
//   std::atomic<duration> += duration
template <class Rep, class Period>
std::atomic<std::chrono::duration<Rep, Period>>&
operator+=(std::atomic<std::chrono::duration<Rep, Period>>& lhs, const std::chrono::duration<Rep, Period>& rhs){
    auto old = lhs.load();
    while(!lhs.compare_exchange_weak(old, old+rhs))
        ;
    return lhs;
}

// _scoped_timer adds the scoping on top of timer.  The accum_type
// template parameter must be either the ClkT's duration, or
// std::atomic of the ClkT's duration.  It's templated so I don't have
// to write out the same code twice.
template <class ClkT, class accum_type_ = typename ClkT::duration>
class _scoped_timer : public timer<ClkT>{
    static_assert( std::is_same<accum_type_, typename ClkT::duration>::value ||
                   std::is_same<accum_type_, std::atomic<typename ClkT::duration> >::value,
                   "the accum type must be either unsigned long long or std:atomic<ull>");
    accum_type_* accum_;
        
public:
    typedef accum_type_ accum_type;
    
    _scoped_timer(accum_type_* p = nullptr) : timer<ClkT>(), accum_(p){}

    _scoped_timer(const _scoped_timer&) = delete;
    _scoped_timer& operator=(const _scoped_timer&) = delete;
    _scoped_timer(_scoped_timer&& rhs) noexcept : timer<ClkT>(rhs){
        accum_ = rhs.accum_;
        rhs.accum_ = nullptr;
    }
    _scoped_timer& operator=(_scoped_timer&& rhs) noexcept
    {
        // Howard Hinnant argues that we don't need the if(this==&rhs) test here:
        //  http://stackoverflow.com/questions/9322174/move-assignment-operator-and-if-this-rhs
        timer<ClkT>::operator=(std::move(rhs));
        accum_ = rhs.accum_;
        rhs.accum_ = nullptr;
        return *this;
    }
    void set_accumulator(accum_type_ *p = nullptr) {
        accum_ = p;
    }
    auto accumulate() -> decltype(this->restart()){
        auto e = this->restart();
        if(accum_)
            *accum_ += e;
        return e;
    }
    ~_scoped_timer() {
        if(accum_)
            *accum_ += this->elapsed();
    }
};

// Finally, typedefs for the two permissible template
// instantiations of _scoped_timer:
using scoped_timer = _scoped_timer<std::chrono::steady_clock>;
using atomic_scoped_timer = _scoped_timer<std::chrono::steady_clock, std::atomic<std::chrono::steady_clock::duration>>;

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
// a refcounted_scoped_timer_ctrl *outside* of all the scopes that
// you want to instrument.  E.g.,
// 
//     refcounted_scoped_timer<> end_to_end_timer;
//
// Then in every instrumented scope, say:
//
//     auto _raii end_to_end_timer.make_instance();
//
// The timer will start when the first thread enters the
// scope, and will stop (and accumulate) when the last thread
// exits the scope.
//
// In the outer scope, read the accumulated time with:
//
//     end_to_end_timer.elapsed(); // returns a steady_clock::duration
//

template <typename ClkT = std::chrono::steady_clock>
struct refcounted_scoped_timer{
    using accum_type = typename ClkT::duration;
private:
    std::mutex mtx;
    typename ClkT::time_point start;
    int refcnt = 0;
    friend struct instance;
    std::atomic<accum_type> p{accum_type{}};

    struct instance{
        refcounted_scoped_timer& rst;
        instance(refcounted_scoped_timer& _rst):
            rst(_rst)
        {
            std::lock_guard<std::mutex> lg(rst.mtx);
            if(rst.refcnt++ == 0)
                rst.start = ClkT::now();
        }
        ~instance() {
            std::lock_guard<std::mutex> lg(rst.mtx);
            if(--rst.refcnt == 0)
                rst.p += ClkT::now() - rst.start;
        }
    };
public:
    accum_type elapsed() const {
        return p.load();
    }
    
    instance make_instance(){
        return instance(*this);
    }
};

} // namespace core123
