#include <core123/stacktrace.hpp>
#include <core123/complaints.hpp>
#include <core123/exnest.hpp>
#include <iostream>

using core123::stacktrace_error;
using core123::complain;
using core123::throw_nest;

void idiom0(){
    throw stacktrace_error("idiom0");
}

void idiom1(){
    throw_nest(std::invalid_argument("Oof"), stacktrace_error("Pow"));
}

int main(int , const char **) {
    try{
        idiom0();
    }catch(std::exception& e){
        complain(e, "complaining about idiom0");
    }

    try{
        idiom1();
    }catch(std::invalid_argument& ia){
        complain(ia, "Caught an invalid argument from idiom1");
    }catch(...){
        std::cout << "FAILED - we shouldn't get here\n";
        return 1;
    }
}

