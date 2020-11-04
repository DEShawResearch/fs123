// This is more of a placeholder than a unit test, but it's small and
// quick, and it exercises some code in inomap.cpp that's arguably
// too-clever-by-half, so let's keep it around.

#include "inomap.hpp"

int main(int, char**){
    fuse_ino_t fino = 123456;
    // This is how we call ino_remember in fs123_init to remember the
    // ino of the root.  Purify thinks there's a UMR (Uninitialized Memory
    // Reference) here, but some bisection reveals that the problem
    // problem report can be eliminated by commenting out the strlen()
    // in inorecord::fill_buf.  So it's almost certainly a false-positive
    // from purify.  It can't hurt to run it under valgrind, though...
    ino_remember(fino, "", 1, ~0);
    return 0;
}
