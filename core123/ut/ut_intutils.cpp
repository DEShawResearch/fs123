#include "core123/intutils.hpp"
#include "core123/ut.hpp"

// Note that mulhilo is tested in ut_mulhil
// The threeroe, threefry and philox known-answer-tests
// also do a pretty good job of exercising rotl.

using core123::endian;
using core123::byteswap;
using core123::popcnt;
using core123::popcnt_nobuiltin;
using core123::gcd;
using core123::lcm;

void test_byteswap(){
    CHECK(endian::little != endian::big);
    CHECK(endian::native == endian::little || endian::native == endian::big);
    EQUAL(byteswap(uint16_t(0x1234)), uint16_t(0x3412));
    EQUAL(byteswap(uint32_t(0x1234)), uint32_t(0x34120000));
    EQUAL(byteswap(uint64_t(0x1234)), uint64_t(0x3412000000000000));
        
    EQUAL(byteswap(uint32_t(0x12345678)), uint32_t(0x78563412));
    EQUAL(byteswap(uint64_t(0x12345678)), uint64_t(0x7856341200000000));
    
    EQUAL(byteswap(uint64_t(0x123456789abcdef1)), uint64_t(0xf1debc9a78563412));
}

void test_popcnt(){
    // This is hardly a thorough test, but at least it exercises
    // the code paths and catches typos.
#define CHKpopc(v, pcv) EQUAL(popcnt(v), pcv); EQUAL(popcnt_nobuiltin(v), pcv)
    CHKpopc((0x12345678u), 13);
    CHKpopc(uint64_t(0x12345678u), 13);
    CHKpopc(uint64_t(0x12345678u)<<32, 13);
    CHKpopc((0u), 0);
    CHKpopc((~uint64_t(0)), 64);
    CHKpopc((~uint32_t(0)), 32);
    CHKpopc((uint16_t(~uint16_t(0))), 16);
#undef CHKpopc
}

void test_gcd(){
    EQUAL(gcd(3, 5), 1);
    EQUAL(gcd(60, 24), 12);
    EQUAL(gcd(97*37*37*2*2, 5u*7*37*2), 37*2);
    EQUAL(gcd(0, 0), 0);
    EQUAL(gcd(0, 10), 10);
    EQUAL(gcd(10, 0), 10);
    for(size_t i=1; i<200; ++i){
        EQUAL(gcd(i, size_t(0)), i);
        for(int j=i; j<2000; ++j){
            auto gij = gcd(i, j);
            auto gji = gcd(j, i);
            EQUAL(gij, gji);
            CHECK( i%gij == 0 );
            CHECK( j%gij == 0 );
            // it's a divisor, but is it the *greatest*?
            for(size_t k = gij+1; k<i; ++k)
                CHECK( i%k != 0 || j%k != 0 );
        }
    }
}

void test_lcm(){
    EQUAL(lcm(3,5), 15);
    EQUAL(lcm(97*37*37*2*2, 5u*7*37*2), 5*7*97*37*37*2*2);
    // We could do more, but ...
}

int main(int, char **){
    test_byteswap();
    test_popcnt(); // Also tested in ut_bits and in ut_bloom.
    test_gcd();
    test_lcm();
    return utstatus();
}
    
