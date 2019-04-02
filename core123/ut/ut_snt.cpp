#include "core123/scoped_nanotimer.hpp"
#include <iostream>
#include <cassert>
#include <cmath>
#include <unistd.h>

using namespace std::chrono;
using core123::scoped_nanotimer;

int main(int, char **){
    // Check that we haven't goofed up a conversion
    // or an epoch shift.

    // N.B.  we can't really avoid occasaional failures
    // here.  Especially under heavy load, if we get swapped
    // out between two "adjacent" clock invocations, we could
    // easily violate the limits on 'mismatch'.
    long long t = 0;
    auto t0sys = system_clock::now();
    {
        scoped_nanotimer snt1(&t);
        ::sleep(1);
    }
    auto deltasys = system_clock::now() - t0sys;
    auto deltasysnanos = duration_cast<nanoseconds>(deltasys).count();
    // both t and deltasysnanos should be around 1.e9
    std::cout << deltasysnanos << " " << t << "\n";
    double mismatch = (deltasysnanos - t)*1.e-9;  // seconds
    assert( fabs(mismatch) < 1.e-2 ); // 10 msec
    
    return 0;
}
