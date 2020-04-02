// shared_ptr<ArrayType> was only specified in C++17.  Do we have support?

#include <core123/shared_blob.hpp>
#include <core123/complaints.hpp>
#include <core123/ut.hpp>

using core123::complain;
using core123::shared_blob;

// N.B.  shared_blob is essential to shared_span, so it also
// gets a work-out in ut_shared_span.cpp

// Here, we're mainly checking that the pp-symbols,
// __has_std_byte and __has_shared_ptr_array are "working".

int main(int, char **) try {
    shared_blob ss(33);
    EQUAL(ss.size(), 33);
    EQUAL(ss.as_span().size(), ss.size());
    return utstatus(true);
}catch(std::exception& e) {
    complain(e, "Exception caught.  main returns 1");
    return 1;
 }
