// simd_threefry.hpp - allows one to instantiate the templated
// threefry generators with an element type of one of GCC's Vector
// Extensions.
//
//  https://gcc.gnu.org/onlinedocs/gcc/Vector-Extensions.html
//
// This is a work in progress.  It "seems to work".
//
// USE WITH CARE.  CHECK CAREFULLY BEFORE TRUSTING RESULTS.
// See the Notes below for why.
//
// The basic idea is to #include this file:
//
// #include <core123/simd_threefry.hpp>
//
// which typedefs 'uint{64,32}_tx{8,4,2}' to be 8-long, 4-long and
// 2-long gcc "Vector Extensions" of 64- and 32-bit unsigned integers.
//
// After which you can say, e.g.,:
//
// threefry<4, uint64_tx8> generator;
//
// The resulting generator has counter and key types that are arrays
// of 4 (the first template argument) elements, each of which is
// a SIMD vector of 8 (the x8) uint64_t's.  Each "slot" in the
// simd vector corresponds to a distinct threefry evaluation.  E.g.,
// in the third slot:
//   counter[0][3], counter[1][3], counter[2][3] and counter[3][3]
// will be mixed with:
//   key[0][3], key[1][3], key[2][3] and key[3][3]
// to produce the random values that appear in:
//   output[0][3], output[1][3], output[2][3] and output[3][3]
//
// The four output values are exactly the same as would be returned
// by threefry<4, uint64_t> with the same key and counter values.
// 
// All slots (8 of them, in this example) are computed completely
// independently.
//
// Thus "granularity" of threefry<N, uintW_txV> is N*W*V bits.  I.e.,
// it is constructed with N*W*V key bits and its operator() takes
// counter argument with N*W*V bits to produce random results with
// N*W*V output bits.
//

// Notes:
//
// This is a WORK IN PROGRESS.  It "seems to work", but a thorough and
// convincing unit test is desperately needed.  The vector extensions
// have been around for a long time, but they're not commonly used by
// application code.  They're in a gray area from C++ type system's
// point of view, so mixing them with templates and function
// overloading is venturing into poorly explored territory.  All in
// all - CAUTION IS ADVISED.  TEST THOROUGHLY BEFORE USING.
//
// GCC compiles simd vectors into correct code even if the target
// architecture lacks registers and instructions of the required
// width.  But there's little benefit (and maybe even performance
// degradation) from using this file unless you also enable generation
// of code that uses vector instructions.  E.g., something like
//    -march=haswell
// to use 256-bit wide AVX2 instructions for uint64_tx4
// or uint32_tx8.
// Or:
//    -march=skylake-avx512
// to use 512-bit wide AVX512 instructions for uint64_tx8.
//
// See:
//   https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html
// for which instruction sets are enabled by which -march=options. 
//
// When asked to work with vectors that exceed the instruction
// set's capabilities (e.g., uint64_tx8 without AVX512), gcc
// issues warnings like:
//
//    warning: AVX512F vector return without AVX512F enabled changes the ABI [-Wpsabi]
//
// It's possible to silence these warnings by uncommenting the #pragmas below.
//
// Since this is header-only code, it tempting to think that ABI
// issues don't matter - the caller is unavoidably compiled with the
// same ABI as the callee because they're in the same translation
// unit.  But I know enough to know I don't know for sure.
// See:  https://stackoverflow.com/a/52391447/989586

#pragma once
#ifndef __GNUC__
#error "This header relies on GNUC Vector Extensions.  It is unusable/meaningless without them"
#endif

//#pragma GCC diagnostic push
//#pragma GCC diagnostic ignored "-Wpsabi"
#include "threefry.hpp"

using uint64_tx8 = uint64_t __attribute__ ((__vector_size__ (64))); // aka __v8du in <immintrin.h>
using uint64_tx4 = uint64_t __attribute__ ((__vector_size__ (32))); // aka __v4du in <immintrin.h>
using uint64_tx2 = uint64_t __attribute__ ((__vector_size__ (16))); // aka __v2du? (not in <immintrin.h>)
using uint32_tx8 = uint32_t __attribute__ ((__vector_size__ (32))); // aka __v8su in <immintrin.h>
using uint32_tx4 = uint32_t __attribute__ ((__vector_size__ (16))); // aka __v4su in <immintrin.h>
using uint32_tx2 = uint32_t __attribute__ ((__vector_size__ (8)));  // aka __v2su? (not in <immintrin.h>)

namespace core123{
template<>
inline
uint64_tx8 rotl<uint64_tx8>(uint64_tx8 x, unsigned s){
    return (x<<s) | (x>>(64u-s));
}
template<>
inline
uint64_tx4 rotl<uint64_tx4>(uint64_tx4 x, unsigned s){
    return (x<<s) | (x>>(64u-s));
}
template<>
inline
uint64_tx2 rotl<uint64_tx2>(uint64_tx2 x, unsigned s){
    return (x<<s) | (x>>(64u-s));
}
template<>
inline
uint32_tx8 rotl<uint32_tx8>(uint32_tx8 x, unsigned s){
    return (x<<s) | (x>>(32u-s));
}
template<>
inline
uint32_tx4 rotl<uint32_tx4>(uint32_tx4 x, unsigned s){
    return (x<<s) | (x>>(32u-s));
}
template<>
inline
uint32_tx2 rotl<uint32_tx2>(uint32_tx2 x, unsigned s){
    return (x<<s) | (x>>(32u-s));
}
//#pragma GCC diagnostic pop

// There's really a lot of repetition here... Fix it?
template<>
struct threefry_constants<2, uint64_tx2> : public threefry_constants<2, uint64_t>{
    static constexpr uint64_tx4 KS_PARITY = {threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<2, uint64_tx4> : public threefry_constants<2, uint64_t>{
    static constexpr uint64_tx4 KS_PARITY = {threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<2, uint64_tx8> : public threefry_constants<2, uint64_t>{
    static constexpr uint64_tx8 KS_PARITY = {threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
                                             threefry_constants<2, uint64_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<4, uint64_tx2> : public threefry_constants<4, uint64_t>{
    static constexpr uint64_tx4 KS_PARITY = {threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<4, uint64_tx4> : public threefry_constants<4, uint64_t>{
    static constexpr uint64_tx4 KS_PARITY = {threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<4, uint64_tx8> : public threefry_constants<4, uint64_t>{
    static constexpr uint64_tx8 KS_PARITY = {threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
                                             threefry_constants<4, uint64_t>::KS_PARITY,
    };
};

template<>
struct threefry_constants<2, uint32_tx2> : public threefry_constants<2, uint32_t>{
    static constexpr uint32_tx4 KS_PARITY = {threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<2, uint32_tx4> : public threefry_constants<2, uint32_t>{
    static constexpr uint32_tx4 KS_PARITY = {threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<2, uint32_tx8> : public threefry_constants<2, uint32_t>{
    static constexpr uint32_tx8 KS_PARITY = {threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
                                             threefry_constants<2, uint32_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<4, uint32_tx2> : public threefry_constants<4, uint32_t>{
    static constexpr uint32_tx4 KS_PARITY = {threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<4, uint32_tx4> : public threefry_constants<4, uint32_t>{
    static constexpr uint32_tx4 KS_PARITY = {threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
    };
};
template<>
struct threefry_constants<4, uint32_tx8> : public threefry_constants<4, uint32_t>{
    static constexpr uint32_tx8 KS_PARITY = {threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
                                             threefry_constants<4, uint32_t>::KS_PARITY,
    };
};

}
