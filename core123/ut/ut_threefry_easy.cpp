#include <core123/threefry.hpp>
#include <core123/streamutils.hpp>
#include <core123/threeroe.hpp>
#include <core123/ut.hpp>
#include <iomanip>
#include <sstream>

// This is more of an 'example' file than a unit test...

using core123::threefry;
using core123::insbe;
using core123::threeroe;

int main(int, char **){
    // Possibly the simplest way to call threefry:
    auto random_numbers = core123::threefry<2,uint32_t>({1,2})({0,1});
    // the 'key' is {1,2} and the 'counter' is {0,1}.

    // Let's print it:
    std::ostringstream oss;
    oss << std::hex;
    oss << "threefry<2,uint32_t>({1,2})({0,1})\n";
    oss << insbe(",", random_numbers) << "\n";

    // A slightly more idiomatic example:
    // Choose a counter-based generator type, e.g., threefry or philox:
    using cbrng_t = threefry<4, uint64_t>;
    // Construct an instance with an arbitrary key:
    auto cbrng = cbrng_t({31,41,59,26});
    // Construct an all-zero counter:
    auto c = cbrng_t::domain_type{};
    // Loop a few times, incrementing the low field of the counter.
    // N.B.  if we want to increment more than 2^64 times, we'll need
    // some fancier code to do the increment...
    oss << "threefry<4,uint64_t>({0,1,2,3})({0,0,0,0} ... {0,0,0,9})\n";
    for(int i=0; i<10; ++i){
        auto random_values = cbrng(c);  // 4 "random" uint64s
        c[0]++;
        oss << insbe(",", random_values) << "\n";
    }
    std::cout << oss.str();

    // Make it a "unit test" by verifying that the output never changes...
    auto hash = threeroe(oss.str()).hexdigest();
    EQSTR(hash, "eb44dc9a444e7a30fc0a2a0d5ed60ce9");

    return utstatus();
}
