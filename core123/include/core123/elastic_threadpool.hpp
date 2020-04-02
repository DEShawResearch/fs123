#pragma once

#include "producerconsumerqueue.hpp"
#include "strutils.hpp"
#include "complaints.hpp"
#include <future>
#include <atomic>
#include <thread>
#include <set>
#include <stdexcept>

// elastic_threadpool: A pool of threads that execute T-valued
// functors of no arguments.
//
// Usage:
//   elastic_threadpool<someType> tp(50, 5);
//   ...
//   auto f = [...]()->someType{...};
//   auto fut = tp.submit(f);
//   // decltype(fut) is std::future<someType>
//  
//   // f() will eventually be called by one of the threads in the pool
//   // The value it returns (or anything it throws) will be made
//   // available to the caller (or anyone else) in fut.  E.g.,
//   someType st = fut.get();  // will either return what f returned or will 
//                             // rethrow whatever f threw.
//
// elastic_threadpool's constructor takes two arguments:
//   elastic_threadpool<T> nthreadmax(int nthreadmax, int nidlemax)
//
// The thread pool will adapt to load by creating and destroying
// threads.  Threads are either idle, i.e., waiting for work to be
// submit()-ed, or carrying out submit()-ed work.  There will be no
// more than nthreadmax threads in existence, and no more than
// nidlemax will ever be idle.  The submit() method is non-blocking,
// regardless of load.  It places a task on the work queue and
// immediately returns to its caller.
//
// Under "reasonable" load conditions, there will usually be close to
// nidlemax idle threads and the work queue will be empty.  Under high
// load, when work is being submit()-ed faster than nthreadmax threads
// can retire it, a 'backlog' of tasks will collect in the work queue.
// Each queue entry consumes about 16 bytes (plus malloc overhead), so
// there is little value in limiting the depth of the backlog queue.
//
// The shutdown() method drains the work queue and waits until all
// previously submit()-ed work has been retired.  It is an error if
// submit() is called after shutdown().  It's up to the caller to
// provide any necessary synchronization.
//
// elastic_threadpool's destructor calls shutdown().  Hence, it also
// waits for all submit()-ed work to be retired.
//
// Methods nidle(), nthread() and backlog() report the number of idle
// threads, the number of executing threads and the length of the
// backlog.  Callers should note that underlying value may change
// before the caller "looks at" the result.

namespace core123{

class raii_ctr{
    std::atomic<int>& c;
    const int incr;
public:
    raii_ctr(std::atomic<int>& _c, int _incr=1) :
        c(_c), incr(_incr)
    { c += incr; }
    ~raii_ctr(){ c -= incr; }
};

template <typename T>
class elastic_threadpool{
    using workunit_t = std::packaged_task<T()>;

    bool need_more_threads(){
        return (nthreads() < nthreadmax) && (nidle() < 1);
    }

    bool too_many_threads(){
        // N.B.  at the point that too_many_threads is called, the
        // calling thread has incremented both nthreads and nidle.
        // So both of them are guaranteed to be >= 1.
        return (nthreads() > nthreadmax) || (nidle() > nidlemax);
    }

    void start_thread() try {
        std::thread([this]() {
                        ++nth;
                        ++nidl;
                        try{
                            workunit_t wu;
                            while(!too_many_threads() && workq.dequeue(wu)){
                                raii_ctr decr_idle(nidl, -1);
                                wu();
                            }
                        }
                        catch(std::exception&e) {complain(e, "Ignoring unexpected exception thrown from elastic_threadpool thread");}
                        catch(...) {complain("elastic_threadpool threw ...  This is *very* unexpected!  Ignoring."); }
                        // N.B.  The idiomatic way to do this is with notify_all_at_thread_exit,
                        // but that appears to be broken/misdesigned.  See:
                        // https://stackoverflow.com/questions/59130819/tsan-complaints-with-notify-all-at-thread-exit
                        // https://cplusplus.github.io/LWG/issue3343
                        // So instead of using raii_ctr, we decrement the counters and call
                        // cv.notify_all ourselves *with the lock held*.
                        std::unique_lock<std::mutex> lk(m);
                        --nidl;
                        --nth;
                        cv.notify_all();
                    }).detach();
    }catch(std::exception& e){
        // N.B.  Under very heavy load the thread constructor can fail
        // with std::errc::resource_unavailable_try_again.  That's only
        // a problem if a) there are no other threads running (i.e.,
        // nth==0) and b) no other tasks are submit()-ed (so we don't
        // try again).
        complain(e, "elastic_threadpool:  failed to start thread");
        if(nth==0)
            complain("elastic_threadpool:  submitted tasks will hang until the next call to submit()");
    }

    const int nthreadmax;
    const int nidlemax;
    std::atomic<int> nth{0};
    std::atomic<int> nidl{0};
    std::mutex m;
    std::condition_variable cv;
    producerconsumerqueue<workunit_t> workq;
    
public:
    elastic_threadpool(int _nthreadmax, int _nidlemax)
        : nthreadmax(_nthreadmax), nidlemax(_nidlemax)
    {
        if(nidlemax <= 0 ||  nthreadmax < nidlemax)
            throw std::invalid_argument(fmt("elastic_threadpool(nthreadmax=%d, nidlemax=%d):  must have nthreadmax>=nidlemax>0", nthreadmax, nidlemax));
    }
        
    void shutdown(){
        workq.close(); // causes workq.dequeue in worker threads to return false - worker threads then exit, notifying the cv.
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this]{ return nth==0; });
    }
    
    ~elastic_threadpool(){
        try{ shutdown(); }
        catch(std::exception& e){  complain(e, "elastic_threadpool::~elastic_threadpool:  ignore exception thrown by shutdown:"); }
        catch(...){ complain("elastic_threadpool::~elastic_threadpool:  ignore ... thrown by shutdown"); }
    }
    
    template <typename CallBackFunction>
    std::future<T> submit(CallBackFunction&&  f){
        if(need_more_threads())
            start_thread();
        workunit_t wu(std::move(f));
        auto fut = wu.get_future();
        if( !workq.enqueue(std::move(wu)) )
            throw std::logic_error("could not enqueue workunit into threadpool queue.  Threadpool has probably been shutdown");
        return fut;
    }

    size_t backlog() const{
        return workq.size();
    }

    int nidle() const{
        return nidl.load();
    }

    int nthreads() const{
        return nth.load();
    }
};

        
} // namespace core123
