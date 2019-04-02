#include "core123/threadpool.hpp"
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

using core123::threadpool;
namespace sew = core123::sew;
using core123::str;

std::atomic<int> i;
class Foo {
private:
    int divisor_;
public:
    Foo(int divisor) : divisor_{divisor} {}
    int operator()() {
	std::this_thread::sleep_for( std::chrono::milliseconds(10) );
	auto k = i++;
	if(k%divisor_==0)
	    throw std::runtime_error("Sorry.  I don't like "+std::to_string(k) +" because it is divisible by "+std::to_string(divisor_));
	return i;
    }
};

int main(int, char**){
    threadpool<int> tp(10);

    // Just for informational purposes - how big is each of the entries
    // in the threadpool's pcq?
    std::cout << "sizeof(threadpool<int>'s packaged_task) " << sizeof(std::packaged_task<int()>) << "\n";
    std::cout << "sizeof(threadpool<double>'s packaged_task) " << sizeof(std::packaged_task<double()>) << "\n";
    std::cout << "sizeof(threadpool<array<int, 64>>'s packaged_task) " << sizeof(std::packaged_task<std::array<int, 64>()>) << "\n";

    std::vector<std::future<int>> results;
    auto cp = getenv("UT_THREADPOOL_DIVISOR");
    auto divisor = cp ? atoi(cp) : 5;
    for(int i=0; i<20; ++i){
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
    static const int Nth = 1;
    threadpool<int> tpx(Nth);
    auto elapsed = t.elapsed();
    std::cout << "construction of threadpool(" << Nth << "):  " << str(elapsed) << "\n";
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
    ready = true;
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
