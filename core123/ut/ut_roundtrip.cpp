#include <core123/svto.hpp>
#include <core123/strutils.hpp>
#include <core123/ut.hpp>
#include <core123/threefry.hpp>
#include <cmath>

using namespace core123;

using rg = threefry<2,uint64_t>;
rg tf;
rg::domain_type ctr = {};

template <typename T>
void roundtrip(T t){
    std::string st = str(t);
    T tst = svto<T>(st);
    if(std::isnan(t)){
        CHECK(std::isnan(tst));
    }else{
        EQUAL(tst, t);
    }
}

template <typename T>
void chk(){
    // First, let's check some corner cases:
    roundtrip(T(0.));
    roundtrip(T(-0.));
    roundtrip(std::numeric_limits<T>::max());
    roundtrip(std::numeric_limits<T>::min());
    roundtrip(std::numeric_limits<T>::infinity());
    roundtrip(-std::numeric_limits<T>::infinity());
    roundtrip(std::numeric_limits<T>::quiet_NaN());
    // Check 10 increments near 1000 "random" values
    for(uint64_t i=0; i<10000; ++i){
        ctr[0]++;
        auto r2x64 = tf(ctr);
        uint64_t r64 = r2x64[0];
        const int min_exponent = std::numeric_limits<T>::min_exponent;
        const int max_exponent = std::numeric_limits<T>::max_exponent;
        const int exponent_range = 1 + max_exponent - min_exponent;
        uint64_t mantissa = r2x64[1]%exponent_range + min_exponent;
        T rT = ldexp(T(r64), mantissa - 64);
        for(int j=0; j<10; ++j){
            roundtrip(rT);
            rT = nextafter(rT, 0.);
        }
    }
}

template <typename T>
void ichk(){
    // First, let's check some corner cases:
    roundtrip(T(0));
    roundtrip(std::numeric_limits<T>::max());
    roundtrip(std::numeric_limits<T>::min());
    // Check 10 increments near 1000 "random" values
    for(uint64_t i=0; i<1000; ++i){
        ctr[0]++;
        T rT = tf(ctr)[0];
        for(int j=0; j<10; ++j){
            roundtrip(rT);
            rT += 1;
        }
    }
}


int main(int argc, char **argv){
    std::cerr.precision(21); // So that any errors get printed with 21 digits!
    chk<float>();
    chk<double>();
    chk<long double>();
    ichk<int>();
    ichk<short>();
    ichk<long>();
    ichk<long long>();
    ichk<unsigned int>();
    ichk<unsigned short>();
    ichk<unsigned long>();
    ichk<unsigned long long>();
    
    return utstatus();
}
