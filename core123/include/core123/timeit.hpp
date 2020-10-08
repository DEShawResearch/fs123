#pragma once

// inspired by Python's timeit utility...
//
// timeit(dur, func, args...) - Call func(args...) repeatedly for the
// specified duration.  Report how many times the function was called,
// and how much time was spent (which is close, but not exactly equal
// to the specified duration).
//
// Usage:
//
// #include <core123/timeit.hpp>
//
// auto result = timeit(std::chrono::seconds(1),
//                      [](){
//                  ... do something ...
//                      });
// std::cout << "lambda was called " << result.count << " times in about 1s\n";
// std::cout << "Actually, the elapsed time was exactly:  " << dur2string(result.dur) << " seconds\n";
//
// You can pass any functor at all to timeit: a plain-old
// function, a class with an operator()(), or a lambda (as above).
//
// Note that if f() has no side-effects, it's *possible* for an
// aggressive optimizer to completely elide calls to it, resulting in
// timings that are not representative.
//

#include <atomic>
#include <chrono>
#include <thread>
#include "scoped_timer.hpp"

namespace core123{
struct timeit_result{
    long long count;
    using dur_t = std::chrono::high_resolution_clock::duration;
    dur_t dur;
    timeit_result(long long count_, const dur_t& dur_):
        count(count_), dur(dur_)
    {}
    timeit_result() : timeit_result(0, {}){}
    float iter_per_sec() const { return count/std::chrono::duration<float>(dur).count(); }
    float sec_per_iter() const { return std::chrono::duration<float>(dur).count()/count; }
};

template <class Rep, class Period, class Fn, class ... Args>
timeit_result
timeit(const std::chrono::duration<Rep, Period>& dur, Fn&& f, Args&&... args){
    std::atomic<bool> done(false);
    std::thread t( [&](){
            std::this_thread::sleep_for(dur);
            done.store(1);
        });
    
    long long n = 0;
    // N.b. use the high_resolution_clock, even though it might not be
    // 'steady'.  Timeit is typically used for short timing runs
    // O(seconds), and we're more interested in high accuracy than we
    // are worried about the clock's steadiness.
    timer<std::chrono::high_resolution_clock> nt;
    do{
        // Unrolling this produced very confusing results.  Any f()
        // that's fast enough that the overhead of n++ and
        // !done.load() is significant seems to also be small enough
        // that wrapping it in a loop (even with constant bounds)) is
        // noticeably different from manual unrolling, Duff's device,
        // etc.  Let's just "keep it simple".  The caller can unroll
        // inside f() if it matters...
        f(std::forward<Args>(args)...);
        n++;
    }while(!done.load());
    auto elapsed = nt.elapsed();
    t.join();
    return {n, elapsed};
}
} // namespace core123
