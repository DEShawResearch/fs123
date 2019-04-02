#include "core123/intutils.hpp"
#include "core123/ut.hpp"

// Note that mulhilo is tested in ut_mulhil
// The threeroe, threefry and philox known-answer-tests
// also do a pretty good job of exercising rotl.

using core123::endian;
using core123::byteswap;

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

int main(int, char **){
    test_byteswap();
    return utstatus();
}
    
