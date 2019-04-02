#include "core123/diag.hpp"

using core123::diag_name;

struct B{
    B();
};

struct A{
    A() : b() {
        static auto _A = diag_name("A");
        static auto _B = diag_name("B");

        DIAGkey(_A, "In A's constructor");
        DIAGkey(_B, "Still in A's constructor");
    }
    B b;
};

A a;
int main(int, char **){
    std::cout << "OK (didn't segfault)\n";
    return 0;
}
