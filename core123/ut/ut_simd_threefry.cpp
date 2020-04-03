#if defined(__ICC)
#include <stdio.h>
int main(int, char **){
    printf(__FILE__ " Icc (through version 18) doesn't fully support gcc's 'vector' extensions.");
    return 0;
}
#else
#pragma GCC diagnostic ignored "-Wpsabi" // see comments in simd_threefry.hpp
#include <core123/simd_threefry.hpp>
#include <core123/streamutils.hpp>
#include <core123/timeit.hpp>

#include <numeric>
#include <iostream>
#include <chrono>



using core123::threefry;
using core123::insbe;
using core123::timeit;
using namespace std::chrono;

static constexpr unsigned ROUNDS = 12;

int main(int argc, char **argv){
    using vcbrng = threefry<4, uint64_tx8, ROUNDS>;
    vcbrng::domain_type ctr;
    using eltype = vcbrng::domain_type::value_type;
    ctr[0] = eltype{00,01,02,03,04,05,06,07};
    ctr[1] = eltype{10,11,12,13,14,15,16,17};
    if(ctr.size()>2){
        ctr[2] = eltype{20,21,22,23,24,25,26,27};
        ctr[3] = eltype{30,31,32,33,34,35,36,37};
    }
    vcbrng tf;
    auto r = tf(ctr);
    std::cout << "Vector threefry: sizeof(range_type): " << sizeof(vcbrng::range_type) << "\n";
    std::cout << std::hex;
    switch(r.size()){
    case 4:
        std::cout << r[0][0] << " " << r[1][0] << " " << r[2][0] << " " << r[3][0] << "\n";
        std::cout << r[0][1] << " " << r[1][1] << " " << r[2][1] << " " << r[3][1] << "\n";
        std::cout << r[0][2] << " " << r[1][2] << " " << r[2][2] << " " << r[3][2] << "\n";
        std::cout << r[0][3] << " " << r[1][3] << " " << r[2][3] << " " << r[3][3] << "\n";
        break;
    case 2:
        throw "Nope.  Not done yet";
        std::cout << r[0][0] << " " << r[1][0] << "\n";
        std::cout << r[0][1] << " " << r[1][1] << "\n";
        std::cout << r[0][2] << " " << r[1][2] << "\n";
        std::cout << r[0][3] << " " << r[1][3] << "\n";
        break;
    }    
    tf.setkey(r);    // Don't allow the optimizer to exploit zero-valued keys.
    ctr = r;         // set the key and counter to "random" values
    static const int LOOP = 2;
    static const int millis = 2000;
    eltype sum = {};
    auto result = timeit(milliseconds(millis),
           [&ctr, &sum, tf](){
               eltype incr = {1, 1, 1, 1, 1, 1, 1, 1};
               for(int i=0; i<LOOP; ++i){
                   ctr[0] += incr;
                   auto r = tf(ctr);
                   sum = std::accumulate(r.begin(), r.end(), sum); // i.e., sum += r[0] + r[1] + .. + r[N-1];
               }
           });
    // Print the sum, so the optimizer can't elide the whole thing!
    std::cout << "sum = " << sum[0] << " " << sum[1] << " " << sum[2] << " " << sum[3] << " " << sum[4] << " " << sum[5] << " " << sum[6] << " " << sum[7] << "\n";
    float ns_per_byte = 1.e9*result.sec_per_iter()/LOOP/sizeof(vcbrng::range_type);
    printf("8-way simd threefry: %lld calls in about %d ms.  %.2f ns per call.  %.3f ns per byte.\n",
           LOOP*result.count, millis,
           1.e9*result.sec_per_iter()/LOOP, ns_per_byte);
    static const float GHz = 3.7;
    printf("at %.1fGHz that's %.2f cpb or %.1f bytes per cycle\n", GHz, ns_per_byte * GHz, 1./(ns_per_byte * GHz));
           

    std::cout << "Scalar threefry:\n";
    {
    using cbrng = threefry<4, uint64_t, ROUNDS>;
    cbrng::range_type r;
    cbrng tf;
    r = tf({00, 10, 20, 30});
    std::cout << insbe(r) << "\n";
    r = tf({01, 11, 21, 31});
    std::cout << insbe(r) << "\n";
    r = tf({02, 12, 22, 32});
    std::cout << insbe(r) << "\n";
    r = tf({03, 13, 23, 33});
    std::cout << insbe(r) << "\n";
    }

    return 0;
}
#endif // not __ICC
