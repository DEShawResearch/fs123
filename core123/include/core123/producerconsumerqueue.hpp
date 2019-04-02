#pragma once
#include <mutex>
#include <condition_variable>
#include <list>

// producerconsumequeue<T> - a basic thread-safe producer/consumer queue for
//  values of type T.  The constructor argument sets the maximum size
//  of the queue.  Producers that try to enqueue or emplace onto a full queue will wait,
//  just as consumers who try to dequeue from an empty queue.
//
//  Features:
//   thread-safe for multiple producers and consumers.
//   works when T has move-semantics, e.g., T=std::unique_ptr<V>
//   easy-to-use semantics for closing and looping-until-closed.
//  Methods:
//   enqueue(T item) - if the queue is closed return false.
//     Otherwise, move item onto the back of the queue and return true.
//   emplace(args...) - like enqueue, but T is constructed from args...
//   close() - Disables further enqueues.
//   bool dequeue(T& result) - Wait until either the queue is closed or it
//       is non-empty.  Then, if it is closed, return false.  Otherwise,
//       swap the front of the queue with result and then pop the front of the queue.
//   size() - return the number of elements in the queue.
//   closed() - return whether the queue is closed.
//   empty() - return whether the queue is empty.
//
// N.B. empty() and size() may return stale info, but since close() is
// irreversible, a true return from closed() will be true forever.

// Basic usage:
// Consumer Thread(s):
//   T next;
//   while( q.dequeue(next) ){
//      process(next);
//   }
//
// Producer Thread(s):
//    q.enqueue(t1);
//   ...
//    q.enqueue(t2);
//   ...
//    q.close()

// Missing features:
//  - timeouts would be nice.
//  - close() and closed() may not be the semantics we want.
//  - it would be easy to allow "line jumping" with push_front.
//
// Question: Should we only notify when the queue becomes empty?
//  According to some stackoverflowers, notifying when there are no
//  waiters is pretty quick.  Things get complicated when we try to do
//  it less often.
//
// Question: should we hold the mutex when notifying?  Helgrind thiks
//   so.  Google and stackoverflow are less certain, but nobody thinks
//   it's *wrong* to hold the mutex while notifying.
//
// Question: should we be taking steps to notify_one() even if
//   something throws in enqueue or dequeue?  I don't think so.
//   The rule is that we *must* notify whenever we push or pop
//   from l.  In all cases, we notify on the very next line, so
//   there's no chance for a throw to interfere with the rule.

namespace core123{
template <class T>
class producerconsumerqueue {
private:
    mutable std::mutex mtx;
    std::list<T> l;
    std::condition_variable dequeue_able_cv;
    std::condition_variable enqueue_able_cv;
    bool is_closed;
    size_t capacity;

public:
    using value_type = T;
    producerconsumerqueue(size_t cap = ~size_t(0)) : is_closed{false}, capacity(cap){}

    // The destructor requires no special handling.  Obviously, we
    // can't destroy this while it's still in use, e.g., if another
    // thread is waiting in dequeue.  But that's no more true of pcq
    // than of anything else. If another thread has a pointer or
    // reference to a string or a UDT, we can't destroy them until
    // we're sure that the other thread is done with them.  It's not
    // the string's or the UDT's responsibility to manage that.  It's
    // done either with program logic or smart pointers.  In
    // threadpool.hpp, for example, the threadpool's destructor calls
    // pcq.close() and then joins with all the threads before
    // destroying the pcq itself.
    ~producerconsumerqueue() = default;

    bool enqueue(T t){
        std::unique_lock<std::mutex> lk{mtx};
        while( l.size() >= capacity && !is_closed )
            enqueue_able_cv.wait(lk);
        if(is_closed)
            return false;
        l.push_back(std::move(t));
        dequeue_able_cv.notify_one();
        return true;
    }

    template <typename ... Args>
    bool emplace(Args&& ... args){
        std::unique_lock<std::mutex> lk{mtx};
        while( l.size() >= capacity && !is_closed )
            enqueue_able_cv.wait(lk);
        if(is_closed)
            return false;
        l.emplace_back(std::forward<Args>(args)...);
        dequeue_able_cv.notify_one();
        return true;
    }        

    void close(){
        std::lock_guard<std::mutex> lg{mtx};
        is_closed = true;
        dequeue_able_cv.notify_all();
        enqueue_able_cv.notify_all();
    }        

    bool dequeue(T& result){
        std::unique_lock<std::mutex> lk{mtx};
        while( l.empty() && !is_closed )
            dequeue_able_cv.wait(lk);
        if(l.empty()) 
            return false;
        std::swap(result, l.front());
        l.pop_front();
        enqueue_able_cv.notify_one();
        return true;
    }
    
    bool empty() const {
        std::lock_guard<std::mutex> lg{mtx};
        return l.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lg{mtx};
        return l.size();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lg{mtx};
        return is_closed;
    }
};
} // namespace core123
