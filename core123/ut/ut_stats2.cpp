// Demonstrate that we can accumulate into the same statistics object
// from a second compilation unit (this file):

#include "ut_stats.hpp"

void fizzbuzz(int k){
    for (auto i = 1; i <= k; i++) {
        core123::atomic_scoped_nanotimer _t(&t.fb_sec);
        if ((i % 3) == 0) c.fizz++;
        if ((i % 5) == 0) c.buzz++;
    }
}
