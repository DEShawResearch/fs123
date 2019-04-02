#pragma once
#include <chrono>
#include <atomic>
#include <utility>

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
protected:
    typename ClkT::time_point tstart_;

public:
    timer() : tstart_(ClkT::now()){}
    using duration = typename ClkT::duration;

    duration elapsed() const {
        return ClkT::now() - tstart_;
    }
    duration restart() {
        auto ts = tstart_;
        tstart_ = ClkT::now();
        return tstart_ - ts;
    }
    auto started_at() const {
        return tstart_;
    }
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

} // namespace core123
