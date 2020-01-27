#include "core123/chacha.hpp"
#include "core123/ut.hpp"
#include "core123/counter_based_engine.hpp"

using namespace core123;

int main(int, char **){
    chacha<12> cha12;
    auto g = make_counter_based_engine<32>(cha12, {0});
    std::cout << std::hex;
    std::cout << "g.sequence_length: " << g.sequence_length() << "\n";
    std::cout << g() << " " << g() << " " << g() << " " << g() << "\n";

    // Test vectors for the Quarter round from  RFC8439 sections 2.1.1 and 2.2.1
    std::array<uint32_t, 16> qrin = {0x11111111, 0x01020304, 0x9b8d6f43, 0x01234567,
                                     0x516461b1, 0x2a5f714c, 0x53372767, 0x3d631689
    };
    std::array<uint32_t, 16> qrka = {0xea2a92f4, 0xcb1cf8ce, 0x4581472e, 0x5881c4bb,
                                     0xbdb886dc, 0xcfacafd2, 0xe46bea80, 0xccc07c79};
    std::cout << "QR test vector in:  " << insbe(&qrin[0], &qrin[4]) << "\n";
    chacha<12>::QR(qrin[0], qrin[1], qrin[2], qrin[3]);
    std::cout << "QR test vector out:  " << insbe(&qrin[0], &qrin[4]) << "\n";

    std::cout << "QR test vector in:  " << insbe(&qrin[4], &qrin[8]) << "\n";
    chacha<12>::QR(qrin[4], qrin[5], qrin[6], qrin[7]);
    std::cout << "QR test vector out:  " << insbe(&qrin[4], &qrin[8]) << "\n";
    CHECK(qrin == qrka);

    // Test vector from RFC8439 section 2.3.2:
    chacha<20> cha20;
    chacha<20>::domain_type iv = {0x03020100,  0x07060504,  0x0b0a0908,  0x0f0e0d0c,
                               0x13121110,  0x17161514,  0x1b1a1918,  0x1f1e1d1c,
                               0x00000001,  0x09000000,  0x4a000000,  0x00000000};
    auto r = cha20(iv);
    chacha<20>::range_type ka = {0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3,
                              0xc7f4d1c7, 0x0368c033, 0x9aaa2204, 0x4e6cd4c3,
                              0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9,
                              0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2};
    CHECK(r == ka);
    std::cout << "RFC8349 Section 2.3.2 Test Vector for the ChaCha20 Block Function:\n";
    std::cout << insbe(&r[0], &r[4]) << "\n";
    std::cout << insbe(&r[4], &r[8]) << "\n";
    std::cout << insbe(&r[8], &r[12]) << "\n";
    std::cout << insbe(&r[12], &r[16]) << "\n";
    
    return utstatus();
}
