#include <core123/exnest.hpp>
#include <stdexcept>
#include <iostream>
#include <cassert>

using core123::exnest;
using core123::rexnest;
using core123::throw_nest;

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

void check_throw_nest() try {
    std::cout << "\nChecking that throw_nest 'works'\n";
    throw_nest(std::overflow_error("outer"), std::logic_error("inner"));
 }catch(std::overflow_error& oe){
    std::vector<std::string> expected  {"outer", "inner"};
    assert(oe.what() == std::string("outer"));
    int i = 0;
    for(auto& a : exnest(oe)){
        std::cout << a.what() << "\n";
        assert(a.what() == expected[i++]);
    }
 }

void check_throw_nest3() try {
    std::cout << "\nChecking that throw_nest 'works'\n";
    throw_nest(std::overflow_error("outer"), std::invalid_argument("middle"), std::logic_error("inner"));
 }catch(std::overflow_error& oe){
    std::vector<std::string> expected  {"outer", "middle", "inner"};
    assert(oe.what() == std::string("outer"));
    int i = 0;
    for(auto& a : exnest(oe)){
        std::cout << a.what() << "\n";
        assert(a.what() == expected[i++]);
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
    check_throw_nest();
    check_throw_nest3();
    return 0;
}
