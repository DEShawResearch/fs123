#include <core123/shared_span.hpp>
#include <core123/span.hpp>
#include <core123/ut.hpp>
#include <core123/complaints.hpp>
#include <cstddef>

using core123::complain;
using core123::shared_span;

int main(int, char **) try {
    shared_span ss(33);
    {
        auto ss2 = ss;
        EQUAL(ss.use_count(), 2);
    }
    EQUAL(ss.use_count(), 1);
    EQUAL(ss.avail_front(), 0);
    EQUAL(ss.avail_back(), 0);

    auto middle = ss.subspan(10, 12);
    EQUAL(middle.size(), 12);
    EQUAL(middle.avail_front(), 10);
    EQUAL(middle.avail_back(), 11);

    middle = middle.grow_back(3);
    EQUAL(middle.size(), 15);
    EQUAL(middle.avail_front(), 10);
    EQUAL(middle.avail_back(), 8);
    
    EQUAL(middle.grow_front(10).avail_front(), 0);
    bool caught = false;
    try{
        middle.grow_front(11);
    }catch(std::out_of_range& oor){
        caught = true;
    }

    EQUAL(middle.grow_back(8).avail_back(), 0);
    try{
        middle.grow_back(9);
    }catch(std::out_of_range& oor){
        caught = true;
    }
    CHECK(caught);

    auto w = middle.whole_span();
    CHECK(w == ss.whole_span());
    EQUAL(w.size(), 33);
    EQUAL(w.avail_front(), 0);
    EQUAL(w.avail_back(), 0);

    EQUAL(w.use_count(), 3); // ss, middle, w

    return utstatus(true);
}catch(std::exception& e) {
    complain(e, "Exception caught.  main returns 1");
    return 1;
 }
