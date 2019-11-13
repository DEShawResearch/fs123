#include "core123/intutils.hpp"
#include "core123/ut.hpp"

// Note that mulhilo is tested in ut_mulhil
// The threeroe, threefry and philox known-answer-tests
// also do a pretty good job of exercising rotl.

using core123::endian;
using core123::byteswap;
using core123::popcnt;
using core123::popcnt_nobuiltin;

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

int main(int, char **){
    test_byteswap();
    test_popcnt(); // Also tested in ut_bits and in ut_bloom.
    return utstatus();
}
    
