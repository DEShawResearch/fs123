#include "core123/diag.hpp"

using core123::diag_name;

struct B{
    B();
};

B::B(){
    static auto _A = diag_name("A");
    static auto _B = diag_name("B");
    DIAGkey(_A, "In B's constructor");
    DIAGkey(_B, "Still in B's constructor");
}

    
