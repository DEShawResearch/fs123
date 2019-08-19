#include <iostream>
#include <core123/periodic.hpp>

// FIXME - this "unit test" exercises the features of periodic, but it
// doesn't *check* them for correctness.  The only errors we'll see are
// segfaults!

using core123::periodic;
using core123::ins;

// A plain-old function:
auto foo(){
    std::cerr << "foo (every 2 seconds)" << std::endl;
    return std::chrono::seconds(2);
}

// A struct with an operator() member:
struct Bar{
    int i = 0;
    auto operator()(){ std::cerr << "Bar sleep for(i=" << ++i << ")" << std::endl; return std::chrono::seconds(i); }
};

int main(int, char **){
    // Basic usage : call a function periodically (every two seconds).
    periodic pfoo{foo};

    // Basic usage:  call a lambda periodically (every second).
    periodic plambda{[](){ std::cerr << "q" << std::endl; return std::chrono::seconds(1); }};

    // Slightly fancier usage:  call a functor periodically.  We can change the rate by modifying the object b.
    Bar b;
    periodic pb{b};

    // Let's watch for a few seconds.
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // lock pb's mutex before accessing b's internal state:
    {
        std::lock_guard<std::mutex> lg(pb.mutex());
        std::cerr << "in main, b.i = " << b.i << std::endl;
    }

    // Code from the "documentation" in periodic.hpp
    // Use duration<float> to confirm that we've worked around
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68519
    auto how_long = std::chrono::duration<float>(1.0f);
    std::string message = "Every second";
    periodic plref([&](){ std::cerr << message << std::endl; return how_long; });

    std::this_thread::sleep_for(std::chrono::seconds(5));
    // plref prints "Every second" every second ...

    // Change a variable captured by reference in the lambda.
    {
        std::lock_guard<std::mutex> lg(plref.mutex());
        how_long = std::chrono::duration<float>(2.0f);
        message = "Every two seconds";
    }
    // plref now prints "Every two seconds" every two seconds ...

    // Let's watch...
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // Trigger:
    std::cerr << "pb.trigger()\n";
    pb.trigger();
    // Change b's rate:
    {
        std::lock_guard<std::mutex> lg(pb.mutex());
        std::cerr << "reset b.i=2" << std::endl;
        b.i = 2;
    }
    std::cerr << "pb.trigger()\n";
    pb.trigger();

    // Let's test periodic threads that work with time_points
    // rather than durations.  It's a bit contrived...
    periodic pluntil([&](){
                         using sc = std::chrono::system_clock;
                         auto now = sc::now();
                         std::cerr << "Should be close to an integral number of seconds (except the first time): " << ins(now) << "\n";
                         auto now_timet = sc::to_time_t(now);
                         now_timet++;
                         return sc::from_time_t(now_timet);
                     });

    // Let's watch for a little longer
    std::this_thread::sleep_for(std::chrono::seconds(10));

    return 0;
}
    
