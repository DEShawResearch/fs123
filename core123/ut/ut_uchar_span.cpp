#include <core123/uchar_span.hpp>
#include <core123/span.hpp>
#include <core123/ut.hpp>
#include <core123/complaints.hpp>
#include <cstddef>

using core123::complain;
using core123::padded_uchar_span;
using core123::uchar_blob;

int main(int, char **) try {
    uchar_blob ub(33);
    padded_uchar_span ss(ub);

    EQUAL(ss.avail_front(), 0);
    EQUAL(ss.avail_back(), 0);

    auto middle = ss.subspan(10, 12);
    EQUAL(middle.size(), 12);
    EQUAL(middle.avail_front(), 10);
    EQUAL(middle.avail_back(), 11);

    {
        auto grown = middle.grow_back(3);
        EQUAL(grown.avail_back(), 8);
        EQUAL(grown.avail_front(), 10);
        EQUAL(grown.size(), 15);
        EQUAL(grown.avail_front(), 10);
        EQUAL(grown.avail_back(), 8);
        middle = grown;
    }
    
    middle = middle.grow_front(10);
    EQUAL(middle.avail_front(), 0);
    EQUAL(middle.avail_front(), 0);

    bool caught = false;
    try{
        middle = middle.grow_front(11);
    }catch(std::out_of_range& oor){
        caught = true;
    }
    CHECK(caught);

    caught = false;
    middle = middle.grow_back(8);
    EQUAL(middle.avail_back(), 0);
    try{
        middle.grow_back(9);
    }catch(std::out_of_range& oor){
        caught = true;
    }
    CHECK(caught);

    auto w = middle.bounding_box();
    CHECK(w == ss.bounding_box());
    EQUAL(w.size(), 33);
    EQUAL(w.avail_front(), 0);
    EQUAL(w.avail_back(), 0);

    return utstatus(true);
}catch(std::exception& e) {
    complain(e, "Exception caught.  main returns 1");
    return 1;
 }
