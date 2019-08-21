#include <core123/stacktrace.hpp>
#include <core123/complaints.hpp>
#include <core123/exnest.hpp>
#include <iostream>

using core123::stacktrace_error;
using core123::complain;
using core123::throw_nest;
using core123::ins;
using core123::str;
using core123::stacktrace_from_here;

void try_using_ins_and_str(){
    std::cout << "Trying ins: " << ins(stacktrace_from_here()) << "\n";
    std::cout << "Trying str: " << str(stacktrace_from_here()) << "\n";
}

void idiom0(){
    throw stacktrace_error("idiom0");
}

void idiom1(){
    throw_nest(std::invalid_argument("Oof"), stacktrace_error("Pow"));
}

int main(int , const char **) {
    try_using_ins_and_str();

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

