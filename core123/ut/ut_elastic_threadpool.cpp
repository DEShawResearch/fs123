#define CORE123_DIAG_FLOOD_ENABLE 1
#include "core123/elastic_threadpool.hpp"
#include "core123/scoped_timer.hpp"
#include "core123/datetimeutils.hpp"
#include "core123/sew.hpp"
#include <condition_variable>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <sstream>
#include <chrono>
#include <string>

using core123::elastic_threadpool;
namespace sew = core123::sew;
using core123::str;

std::atomic<int> ai;
class Foo {
private:
    int divisor_;
public:
    Foo(int divisor) : divisor_{divisor} {}
    int operator()() {
	std::this_thread::sleep_for( std::chrono::milliseconds(10) );
	auto k = ai++;
	if(k%divisor_==0)
	    throw std::runtime_error(str("Foo: won't return", 10*k, "because", k, "is divisible by", divisor_));
	return 10*k;
    }
};

int main(int, char**){
    std::vector<std::future<int>> results;
    elastic_threadpool<int> tp(10, 2);

    // Just for informational purposes - how big is each of the entries
    // in the elastic_threadpool's pcq?
    std::cout << "sizeof(elastic_threadpool<int>'s packaged_task) " << sizeof(std::packaged_task<int()>) << "\n";
    std::cout << "sizeof(elastic_threadpool<double>'s packaged_task) " << sizeof(std::packaged_task<double()>) << "\n";
    std::cout << "sizeof(elastic_threadpool<array<int, 64>>'s packaged_task) " << sizeof(std::packaged_task<std::array<int, 64>()>) << "\n";

    auto cp = getenv("UT_THREADPOOL_DIVISOR");
    auto divisor = cp ? atoi(cp) : 5;
    for(int i=0; i<20; ++i){
        // Note that we're pushing 40 tasks.  20 of them return their lambda parameter.
        // And 20 of them return 10*std_atomic_counter++ (unless the counter is divisible
        // by 5, in which case they throw!
        results.push_back( tp.submit( [=](){ return i; }) );
	auto f = Foo(divisor);
        results.push_back(tp.submit(f));
    }
    while(tp.backlog()){
        ::sleep(1);
    }
    for( auto& r : results ){
        try{
            std::cout << r.get() << "\n";
        }catch(std::runtime_error& e){
            std::cout << "exception delivered by r.get: " << e.what() << "\n";
        }
    }
    std::cout << "outer thread: " << std::this_thread::get_id() << "\n";

    // Try that again, but this time, discard the futures returned by submit.
    // The standard says that ~future is  non-blocking, unless it came from
    // std::async.  And our futures don't come from std::async, so we should
    // be ok...
    for(int i=0; i<20; ++i){
        tp.submit( [=](){ return i; });
	auto f = Foo(divisor);
        tp.submit(f);
    }
    while(tp.backlog()){
        ::sleep(1);
    }
    tp.shutdown();
    results.clear();
    std::cout << "outer thread: " << std::this_thread::get_id() << "\n";

    // let's get a sense of the space and time overheads of using the threadpool.
    std::ostringstream cmdoss;
    cmdoss << "grep Vm /proc/" << getpid() << "/status";
    sew::system(cmdoss.str().c_str());
    core123::timer<> t;
    elastic_threadpool<int> tpx(1, 1);
    auto elapsed = t.elapsed();
    std::cout << "construction of elastic_threadpool(1, 1, 1):  " << str(elapsed) << "\n";
    sew::system(cmdoss.str().c_str());
    
    static const int N=10000;
    t.restart();
    bool ready = false;
    std::condition_variable cv;
    std::mutex m;
    for(int i=0; i<N; ++i){
        results.push_back(tpx.submit([i, &m, &cv, &ready](){
                    std::unique_lock<std::mutex> lk(m);
                    cv.wait(lk, [&ready]{return ready;});
                    return i;}));
    }
    elapsed = t.elapsed();
    std::cout << "after submitting " << N << " requests:  " << str(elapsed/N) << " seconds per submission\n";
    std::cout << "backlog: " << tpx.backlog() << "\n";
    sew::system(cmdoss.str().c_str());

    int sum = 0;
    t.restart();
    {
        std::unique_lock<std::mutex> lk(m);
        ready = true;
    }
    cv.notify_all();
    for(int i=0; i<N; ++i){
        sum += results[i].get();
    }
    elapsed = t.elapsed();
    std::cout << "sum = " << sum << "\n";
    std::cout << "after get-ing " << N << " futures:  " << str(elapsed/N) << " seconds per get\n";
    sew::system(cmdoss.str().c_str());
    
    results.clear(); results.shrink_to_fit();
    std::cout << "after destroying all the futures:\n";
    sew::system(cmdoss.str().c_str());

    return 0;
}
