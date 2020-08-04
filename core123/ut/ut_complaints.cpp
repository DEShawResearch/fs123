#include <core123/complaints.hpp>
#include <core123/sew.hpp>
#include <core123/stacktrace.hpp>
#include <stdexcept>
#include <thread>
#include <chrono>

using core123::complain;
using core123::set_complaint_destination;
using core123::set_complaint_level;
using core123::set_complaint_max_hourly_rate;
using core123::get_complaint_max_hourly_rate;
using core123::get_complaint_hourly_rate;
using core123::set_complaint_averaging_window;
using core123::get_complaint_averaging_window;
using core123::start_complaint_delta_timestamps;
using core123::fmt;
using core123::str;
using core123::stacktrace_from_here;

void open_does_not_exist(){
    core123::sew::open("/does/not/exist", O_RDONLY);
}

void foo(){
    try{
        open_does_not_exist();
    }catch(std::exception& e){
        std::throw_with_nested(std::runtime_error("line 1 of color commentary\nline 2 of color commentary"));
    }
}

void throws_a_nested_error(){
    try{
        foo();
    }catch(std::exception& e){
        std::throw_with_nested(std::runtime_error(std::string("in ") + __func__));
    }
}

void deep(int n){
    complain(LOG_NOTICE, "deep(%d)", n);
    if(n<=0)
        complain(LOG_NOTICE, "Calling complainbt at the bottom of a stack of recursive calls to deep:\n" + str(stacktrace_from_here()));
    else
        deep(n-1);
}

int main(int, char **){
    deep(10);
    set_complaint_destination("%stdout", 0666);
    complain("This is a complaint at the default level (no newline)");
    complain("This one  has an explicit newline at the end\n");
    set_complaint_level(LOG_NOTICE);
    complain(LOG_INFO, "This one is at level=LOG_INFO.  You probably shouldn't see it");
    set_complaint_level(LOG_INFO);
    complain(LOG_INFO, "Again at level=LOG_INFO.  You should see this one");
    try{
        throws_a_nested_error();
    }catch(std::exception& e){
        complain(e, "This should be a nested exception:");
    }

    start_complaint_delta_timestamps();
    complain(fmt("A complaint immediately after turning on delta timestamps.  Check that %%f works: pi=%f", 3.141592653589793238));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    complain(fmt("Another complaint after %d milliseconds.  What's the deltatstamp?", 50));

    float averaging_window = 1;
    float max_hourly_rate = 3600.;
    complain(fmt("Check that throtting works.  Changing the averaging_window to %f and the max_hourly_rate to %f\n",
             averaging_window, max_hourly_rate));
    set_complaint_max_hourly_rate(max_hourly_rate);
    set_complaint_averaging_window(averaging_window);
    for(int i=0; i<1000; ++i){
        complain(fmt("the max_hourly_rate is %f.  The complaint hourly rate is: %f.  Many of these messages will be throttled", get_complaint_max_hourly_rate(), get_complaint_hourly_rate()));
    }

    for(int s=0; s<10; ++s){
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for(int i=0; i<1000; ++i){
            try{
                throws_a_nested_error();
            }catch(std::exception& e){
                complain(e, fmt("the max_hourly_rate is %f.  The hourly  rate is:  %f Many of these messages will be throttled", get_complaint_max_hourly_rate(), get_complaint_hourly_rate()));
            }
        }
    }

    complain(LOG_CRIT, fmt("LOG_CRIT are not throttled.  Let's watch the hourly rate decay - with a time-constant of %f\n", get_complaint_averaging_window()));
    for(int s=0; s<10; ++s){
        complain(LOG_CRIT, fmt("complaint_hourly_rate: %f\n", get_complaint_hourly_rate()));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

