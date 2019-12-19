#if 0
// RESULTS ARE TOO CONFUSING!  In ut_timeit.hpp it reports nothing
// like the expected performance for a multi-core machine.  Until
// that's understood, it's #ifdef'ed out...

#include <list>
#include <future>
#include "scoped_timer.hpp"
#include "timeit.hpp"

// See documentation in timeit.hpp.

// This version has an extra 'nthread' argument that tries to run the
// function in that many asynchronous threads.  

namespace core123{
template <class Rep, class Period, class Functor>
timeit_result
mt_timeit(const std::chrono::duration<Rep, Period>& dur, Functor f, int nthread = 1){
    std::atomic<bool> done(false);
    std::list<std::future<long long int>> flist;
    refcounted_scoped_timer<> end_to_end_timer;
    for(int i=0; i<nthread; ++i){
        flist.emplace_back(std::async(std::launch::async,
                           [&](){
                               long long n = 0;
                               {
                                   auto inst = end_to_end_timer.make_instance();
                                   do{
                                       f();
                                       n++;
                                   }while(!done.load());
                               }
                               return n;
                           }));
    }

    std::this_thread::sleep_for(dur);
    done.store(1);
    
    long long count = 0;
    for(auto& f : flist){
        count += f.get();
    }
    return {count, end_to_end_timer.elapsed()};
}
} // namespace core123

#endif // completely disabled
