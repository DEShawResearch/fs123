#include "core123/intutils.hpp"
#include "core123/ut.hpp"

// Note that mulhilo is tested in ut_mulhil
// The threeroe, threefry and philox known-answer-tests
// also do a pretty good job of exercising rotl.

using core123::endian;
using core123::byteswap;
using core123::popcount;
using core123::popcount_nobuiltin;

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

void test_popcount(){
    // This is hardly a thorough test, but at least it exercises
    // the code paths and catches typos.
    // N.B.  We check our explicit implementation in core123::detail
    // and *either* the C++20 std::popcount, or the __GNUC__ __builtin_popcountl.
#define CHKpopc(v, pcv) EQUAL(popcount(v), pcv); EQUAL(popcount_nobuiltin(v), pcv)
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
    test_popcount(); // Also tested in ut_bits and in ut_bloom.
    return utstatus();
}
    
