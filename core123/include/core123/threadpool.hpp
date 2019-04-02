#pragma once

#include "producerconsumerqueue.hpp"
#include <future>
#include <thread>
#include <set>

// A pool of threads that execute T-valued functors of
// no arguments.
// 
// Usage:
//   threadpool<someType> tp(100); // 100 threads in pool
//   ...
//   auto fut = tp.submit(f);
//   // if f() returns someType then fut is std::future<someType>
//  
//   // f() will eventually be called by one of the threads in the thread
//   // pool.  The value it returns (or anything it throws) will be made
//   // available to the caller (or anyone else) in fut.
//   // See the documentation for std::future.  E.g.,
//   someType st = fut.get();  // will either return what f returned or will 
//                             // rethrow whatever f threw.
namespace core123{
template <typename T>
class threadpool{
    using workunit_t = std::packaged_task<T()>;
    using q_t = producerconsumerqueue<workunit_t>;

    struct wthread : public std::thread {
        wthread(q_t *q) : std::thread([q]() {
                try{
                    workunit_t wu;
                    while(q->dequeue(wu))
                        wu();
                }catch(std::exception &e){
                    std::throw_with_nested(std::logic_error("threadpool::worker wu() threw an exception even though it's a packaged_task."));
                }
            }){}
        wthread(wthread&&) = default;
        ~wthread(){ 
            // DANGER!  If this an active thread (and not the empty
            // husk from which a thread has been std::move()-ed), then
            // the destructor WILL HANG until somebody calls
            // q->close().  DO NOT LET WTHREADS GO OUT OF SCOPE UNLESS
            // YOU'RE SURE q->close HAS BEEN CALLED.
            if(joinable()) // not joinable if we've been std::move()-ed from
                join();
        }
    };        

    q_t q;
    std::list<wthread> threads;
    std::set<std::thread::id> thread_ids;
    
public:
    threadpool(size_t N, size_t backlog = ~size_t(0))
        : q(backlog)
    {
        for(size_t i=0; i<N; ++i) try {
            threads.emplace_back(&q);
            thread_ids.insert( threads.back().get_id() );
        }catch(std::exception& e){
            shutdown();
            std::throw_with_nested(std::runtime_error("threadpool::threadpool:  failed to create worker threads"));
        }
    }
        
    // It's essential to call shutdown *before* threads goes
    // out-of-scope.  Calling threads.clear() calls each wthread's
    // destructor, which calls join() to avoid a nasty
    // std::terminate().  But join() hangs until the worker function
    // has returned.  So we must first call q.close(), which causes
    // all the worker functions to return when their current workunit
    // is completed.
    void shutdown(){
        q.close();
        threads.clear();
    }

    ~threadpool(){
        shutdown();
    }

    // Call submit_nocheck if you *know* that you're not
    // in a threadpool.
    template <typename CallBackFunction>
    std::future<T> submit_nocheck(CallBackFunction&&  f){
        workunit_t wu(std::move(f));
        auto fut = wu.get_future();
        if( !q.enqueue(std::move(wu)) )
            throw std::logic_error("could not enqueue workunit into threadpool queue.  Threadpool has probably been shutdown");
        return fut;
    }

    template <typename CallBackFunction>
    auto submit(CallBackFunction&& f){
        if(this_thread_in_pool())
            throw std::logic_error("A workunit may not submit more work to its own threadpool");
        return submit_nocheck(std::move(f));
    }

    bool this_thread_in_pool() const {
        return thread_ids.count(std::this_thread::get_id());
    }

    size_t backlog() const{
        return q.size();
    }
};

        
} // namespace core123
