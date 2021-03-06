#include <core123/timeit.hpp>
//#include <core123/mt_timeit.hpp>  // Unreliable!
#include <core123/envto.hpp>
#include <core123/threefry.hpp>
#include <core123/philox.hpp>
#include <core123/chacha.hpp>
#include <iostream>

static int Nthreads;
using core123::philox;
using core123::threefry;
using core123::chacha;

template <typename CBRNG>
void timecheck(const std::string& name, int millis){
    typename CBRNG::key_type k = {};
    typename CBRNG::domain_type c;
    CBRNG tf(k);
    bool wow = false;
    static const int LOOP = 8;
    using namespace std::chrono;
    auto result = core123::timeit(milliseconds(millis),
                    [&](){
                        for(int i=0; i<LOOP; ++i){
                            c[0]++;
                            auto r = tf(c);
                            if(r[0] == 0 && r[1]==0)
                                wow = true;
                            if(r.size()>2 && r[2] == 0 && r[3] == 0)
                                wow = true;
                        }});
    if(wow)
        std::cout << "Wow.  We got a zero!" << "\n";
    printf("%s: %lld calls in about %d ms.  %.2f ns per call.  %.2f ns per byte.\n",
           name.c_str(), LOOP*result.count, millis,
           1.e9*result.sec_per_iter()/LOOP, 1.e9*result.sec_per_iter()/LOOP/sizeof(typename CBRNG::range_type));

#if 0 // mt_timeit is unreliable
    wow = false;
    result = core123::mt_timeit(milliseconds(millis),
                    [&](){
                        for(int i=0; i<LOOP; ++i){
                            c[0]++;
                            auto r = tf(c);
                            if(r[0] == 0 && r[1]==0)
                                wow = true;
                            if(r.size()>2 && r[2] == 0 && r[3] == 0)
                                wow = true;
                        }}, Nthreads);
    if(wow)
        std::cout << "Wow.  We got a zero!" << "\n";
    printf("%s: (%d threads) %lld calls in about %d ms.  %.2f ns per call.  %.2f ns per byte.\n",
           name.c_str(), Nthreads, LOOP*result.count, millis,
           1.e9*result.sec_per_iter()/LOOP, 1.e9*result.sec_per_iter()/LOOP/sizeof(typename CBRNG::range_type));
#endif
    
}

void iterated_logistic_map(unsigned n, double r, double x){
    while(n--)
        x = r*x*(1-x);
    if(x==.5)
        throw std::runtime_error("Surprising result.");
}
            
    

int main(int, char**){
    Nthreads = core123::envto<int>("UT_TIMEIT_NTHREADS", 6);
    int msec = core123::envto<int>("UT_TIMEIT_MSEC", 200);
    
    // First, let's try out timeit with arguments:
    auto m = core123::timeit(std::chrono::milliseconds(msec), iterated_logistic_map, 100, 3.8, 0.5);
    printf("iterated_logistic_map(100, 3.8, 5): %g times per sec\n", m.iter_per_sec());

    std::cout << "Recommended generators:\n";
    timecheck<philox<2, uint64_t, 10>>("philox<2, uint64_t, 10>", msec);
    timecheck<philox<4, uint64_t, 10>>("philox<4, uint64_t, 10>", msec);
    timecheck<philox<2, uint32_t, 10>>("philox<2, uint32_t, 10>", msec);
    timecheck<philox<4, uint32_t, 10>>("philox<4, uint32_t, 10>", msec);
    timecheck<threefry<2, uint64_t, 20>>("threefry<2, uint64_t, 20>", msec);
    timecheck<threefry<4, uint64_t, 20>>("threefry<4, uint64_t, 20>", msec);
    timecheck<threefry<4, uint32_t, 20>>("threefry<4, uint32_t, 20>", msec);
    timecheck<chacha<8>>("chacha<8>", msec);
    std::cout << "\n";
    std::cout << "Crush-resistant generators:\n";
    timecheck<philox<2, uint64_t, 6>>("philox<2, uint64_t, 6>", msec);
    timecheck<philox<4, uint64_t, 7>>("philox<4, uint64_t, 7>", msec);
    timecheck<philox<2, uint32_t, 7>>("philox<2, uint32_t, 7>", msec);
    timecheck<philox<4, uint32_t, 7>>("philox<4, uint32_t, 7>", msec);
    timecheck<threefry<2, uint64_t, 13>>("threefry<2, uint64_t, 13>", msec);
    timecheck<threefry<4, uint64_t, 13>>("threefry<4, uint64_t, 13>", msec);
    timecheck<threefry<4, uint32_t, 12>>("threefry<4, uint32_t, 12>", msec);
    timecheck<chacha<4>>("chacha<4>", msec);
    std::cout << "\n";
    std::cout << "Cryptographic generators:\n";
    timecheck<threefry<4, uint64_t, 72>>("threefry<4, uint64_t, 72>", msec);
    timecheck<chacha<20>>("chacha<20>", msec);

    return 0;
}
