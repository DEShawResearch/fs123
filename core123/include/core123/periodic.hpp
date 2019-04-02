#pragma once
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <future>
#include "fwd_capture.hpp"

// periodic - Periodically call a function (or any other callable).

// Member functions:

// template <typename Function>
// periodic(Function&& function) :  The constructor starts an
//     asynchronous thread which repeatedly calls the function, which
//     must return a std::chrono::duration.  Each time the function
//     returns, the asynchronous thread sleeps for the returned
//     duration (which may vary from one invocation to the next).  If
//     the function throws, the loop terminates silently and the
//     function will not be called again.

//     If function is an rvalue reference, it will be
//     std::move()-ed into the asynchronous thread.  But if it is an
//     lvalue reference, the reference will be copied, allowing the
//     caller to share state with the asynchronous thread.  The
//     mutex() member is useful for synchronization.

//     N.B.  The thread's condition variable may be "spuriously
//     unblocked", in which case the function will be called before the
//     specified time has elapsed.

// trigger() : if the function is currently running, wait for it to
//     finish.  Then, before it runs again, notify the asynchronous
//     thread to schedule the next invocation of the function
//     immediately, regardless of the duration returned by the last
//     invocation.

// ~periodic() : if the function is currently running, wait for it to
//     finish.  Then, notify the asynchronous thread, causing it to
//     immediately exit its loop and complete, regardless of the
//     duration returned by the last function invocation.  The calling
//     thread then waits for the asynchronous thread to complete and
//     destroys all associated state.

// mutex() : return a reference to a mutex that is held by the
//     asynchronous thread whenever the function is running (and also
//     by trigger()).  Locking this mutex will prevent the function
//     from running, allowing the locking thread to safely modify
//     state shared with the function.  The lifetime of the returned
//     reference is the same as the periodic object.  Deadlock will
//     occur if trigger() is called with the mutex locked.

// Usage:
//
// // Very simple once-per-minute loop:
//
//     periodic p([](){ std::cerr << "Foo\n"; return std::chrono::minutes(1); });
//
// // Sharing data between calling thread and asynchronous thread:
//
//    auto how_long = std::chrono::seconds(1);
//    std::string message = "Every second";
//    periodic q([&](){ std::cerr << message << std::endl; return how_long; });
//
//    // ... q's thread prints "Every second" every second ...
//
//    {
//        std::lock_guard<std::mutex> lg(q.mutex());
//        how_long = std::chrono::seconds(2);
//        message = "Every two seconds";
//    }
//
//    // ... q's thread prints "Every two seconds" every two seconds ...

namespace core123{

class periodic{
public:
    template <typename Function>
    periodic(Function&& F){
        fut = std::async(std::launch::async,
                         [this, Ftuple = fwd_capture(FWD(F))]() mutable {
                             std::unique_lock<std::mutex> lk(mtx);
                             do{
                                 auto how_long = std::get<0>(FWD(Ftuple))();
                                 cv.wait_for(lk, how_long);
                                 // either how_long has elapsed, or we
                                 // were notified by trigger(), or we
                                 // were notified by the destructor or
                                 // we were unblocked "spuriously".
                                 // If we were unblocked "spuriously"
                                 // we'll get an extra "spurious" call
                                 // to F().  It doesn't seem worth
                                 // adding more code to prevent that.
                             }while(!done);
                        });
    }

    void trigger(){
        std::unique_lock<std::mutex> lk(mtx);
        cv.notify_all();
    }

    std::mutex& mutex(){ return mtx; }
    
    ~periodic(){
        std::unique_lock<std::mutex> lk(mtx);
        done = true;
        cv.notify_all();
        lk.unlock();
        try{
            fut.get(); // wait for the thread to complete
        }catch(...){}
    }
private:
    std::condition_variable cv;
    bool done = false;
    std::mutex mtx;
    std::future<void> fut;
};

} // namespace core123
