// A pseudo-random function based on Bernstein's cryptographic ChaCha20.
// See: https://cr.yp.to/chacha.html
//      https://cr.yp.to/chacha/chacha-20080128.pdf
//      https://www.rfc-editor.org/info/rfc8439
//
// For our *NON*-cryptographic purposes, we ignore the separation of
// the block into "key", "counter" and "nonce", and treat the ChaCha20
// block function as a key-less pseudo-random function from a 384-bit
// (12 x uint32_t) input domain to a 512-bit (16 x uint32_t) output
// range.
//
// I.e., chacha is a core123::prf_common function whose:
//   domain_type is std::array<uint32_t, 12>
//   range_type  is std::array<uint32_t, 16>
//   key_type    is std::array<uint32_t, 0>
//
// Usage:
//    chacha<4> c;
//    std::array<uint32_t, 16> r = c({... up to 12 uint32_t values});
//    // r contains 16 "random" values.
//
// To use it as a conventional engine:
//
//    auto g = make_counter_based_engine<32>(chacha<4>(), {... up to 11 values ...});
//    ... g() ... // up to 64 billion times.
//
// Quality:
//
// Preliminary indications are that chacha<4> is statistically sound,
// (passing practrand) and *very* fast.  However, chacha<4> has no
// safety margin.  The output of chacha<3> is clearly non-random.
//
// chacha<8> is recommended as a good compromise between safety and
// speed.
//
// As of Jan 2020, there are no published cryptographic attacks on
// chacha<8>.  Chacha<20> is widely used in highly secure
// applications.  It would be *extremely* surprising to discover
// deviations from randomness in either of them.

#pragma once

#include "detail/prf_common.hpp"
#include "intutils.hpp"
#include <string.h>

namespace core123{
template<int R>
struct chacha : public detail::prf_common<12, 16, 0, uint32_t>{
    static_assert(R%2==0, "Number of rounds must be even");
    // the constants spell out "expand 32-byte k"
    static constexpr uint32_t c0 = 0x61707865;
    static constexpr uint32_t c1 = 0x3320646e;
    static constexpr uint32_t c2 = 0x79622d32;
    static constexpr uint32_t c3 = 0x6b206574;

    static void QR(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d){
        a += b; d ^= a; d = rotl(d, 16);
        c += d; b ^= c; b = rotl(b, 12);
        a += b; d ^= a; d = rotl(d, 8);
        c += d; b ^= c; b = rotl(b, 7);
    }

    range_type operator()(domain_type in){
        uint32_t s0 = c0, s1 = c1, s2 = c2, s3 = c3;
        uint32_t s4 = in[0], s5 = in[1], s6 = in[2], s7 = in[3];
        uint32_t s8 = in[4], s9 = in[5], s10 = in[6], s11 = in[7];
        uint32_t s12 = in[8], s13 = in[9], s14 = in[10], s15 = in[11];        
        for(int i=0; i<R; i+=2){
            QR(s0, s4, s8, s12);
            QR(s1, s5, s9, s13);
            QR(s2, s6, s10, s14);
            QR(s3, s7, s11, s15);
            //if(i+1 == R) break; // only if we allow odd values of R?
            QR(s0, s5, s10, s15);
            QR(s1, s6, s11, s12);
            QR(s2, s7, s8, s13);
            QR(s3, s4, s9, s14);
        }
#if 1
        range_type ret = { s0+ c0,    s1+ c1,    s2+ c2,     s3+ c3,
                           s4+in[0],  s5+in[1],  s6+in[2],   s7+in[3],
                           s8+in[4],  s9+in[5], s10+in[6],  s11+in[7],
                          s12+in[8], s13+in[9], s14+in[10], s15+in[11]};
        return ret;
#else   // Dropping the final addition saves 16 additions (compared
        // with only 48 operations in the R=4 loop).
        // Nevertheless, on x86-64, there's no performance gain.  It
        // may be no more expensive to write a sum to a memory address
        // than just a value.
        return {s0,  s1,  s2,  s3,
                s4,  s5,  s6,  s7,
                s8,  s9,  s10, s11,
                s12, s13, s14, s15};
#endif
    }

    // boilerplate to forward to common_type:
    using common_type = detail::prf_common<12, 16, 0, uint32_t>;
    chacha() : common_type(){}
    chacha(key_type _k) : common_type(_k){ }

    CORE123_DETAIL_OSTREAM_OPERATOR(os, chacha, f){
        return os; // there's no internal state!
        //return os << static_cast<const common_type&>(f);
    }

    CORE123_DETAIL_ISTREAM_OPERATOR(is, chacha, f){
        return is; // there's no internal state!
        //return is >> static_cast<common_type&>(f);
    }
};

}
