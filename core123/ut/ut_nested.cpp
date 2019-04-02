#include <core123/exnest.hpp>
#include <stdexcept>
#include <iostream>
#include <cassert>

using core123::exnest;
using core123::rexnest;

void foo(){
    throw std::runtime_error("thrown by foo()");
}

void func() try {
    foo();
 }catch(std::exception& e){
    throw_with_nested(std::runtime_error("added by func()"));
 }

void check_nest() try {
    std::cout << "\nChecking that nested exceptions 'work'\n";
    func();
 }catch(std::exception& e){
    std::vector<std::string> expected = {"added by func()", "thrown by foo()"};
    int i = 0;
    std::cout << "Outer to inner:\n";
    for(auto& a : exnest(e)){
        std::cout << a.what() << "\n";
        assert(a.what() == expected[i++]);
    }

    std::cout << "Inner to outer:\n";
    for(auto& a : rexnest(e)){
        std::cout << a.what() << "\n";
        assert(a.what() == expected[--i]);
    }
 }

void justone() try{
    std::cout << "\nChecking that unnested exceptions 'work'\n";
    throw std::runtime_error("this is the original error");
 }catch(std::exception& e){
    std::vector<std::string> expected = {"this is the original error"};
    int i = 0;
    std::cout << "Outer to inner:\n";
    for(auto& a : exnest(e)){
        std::cout << a.what() << "\n";
        assert(a.what() == expected[i++]);
    }

    std::cout << "Inner to outer:\n";
    for(auto& a : rexnest(e)){
        std::cout << a.what() << "\n";
        assert(a.what() == expected[--i]);
    }
 }    

int main(int, char **){
    justone();
    check_nest();
    return 0;
}
