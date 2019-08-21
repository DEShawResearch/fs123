#include <core123/demangle.hpp>
#include <core123/ut.hpp>
#include <iostream>

using core123::demangle;

int main(int, char **){
    EQUAL(demangle("i"), "int");
    std::cout << demangle("i") << "\n";
    EQUAL(demangle(typeid(main)), "int (int, char**)"); // can we always count on spacing??
    std::cout << demangle(typeid(main)) << "\n";
    try{
        demangle("this is not a name");
        CHECK(false);
    }catch(std::system_error& e){
        std::cerr << "Correctly threw a system-error: " << e.what() << "\n";
        CHECK(true);
    }
    return utstatus();
}
