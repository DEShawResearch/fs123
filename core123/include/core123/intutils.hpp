/** @page LICENSE
Copyright 2010-2019, D. E. Shaw Research.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above copyright
  notice, this list of conditions, and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions, and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

* Neither the name of D. E. Shaw Research nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
o
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

// intutils.hpp - a grab-bag of utilities for working with integers:

//   rotl(u, n) - rotate u left by n.  Templated over unsigned integral
//                 types for u.  n must be >0 and < the number of bits
//                 in decltype(u)
//   mulhilo(ua, ub) - returns a pair, consisting of the hi and low bits
//                   of the products of the unsigned values ua and ub.
//   bitmask<T>(nsetbits, startbits) - return an integral type T with
//                 'nsetbits' contiguous bits starting at 'startbits'
//                 set.
//   enum class endian - members:  big, little, native (similar to C++20)
//
//   byteswap(u) - works on 16, 32 and 64-bit unsigned. Similar to C++20.
//
//   betonative(u), nativetobe(u), letonative(u), nativetole(u) -
//                  work on 16, 32 and 64-bit unsigned, converting
//                  from native to and from bigendian and littlendian.
//
//   popcount(n) - as close as possible to std::popcount in C++20.
//
//   clip(low, x, high) - return x, clipped from below by low and from
//                  above by high.  Arguments must be comparable with
//                  operator: x<low and high<x.  The type of x must be
//                  copy-constructable and constructable from const
//                  refs to the types of low and high.  N.B.  this
//                  isn't just for integers, but this seemed like the
//                  best place to put it.

#include <cinttypes>
#include <limits>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <stdexcept>
#if __has_include(<bit>)
#include <bit>
#endif
#include <climits>

namespace core123{

// First, we implement hilo multiplication with "half words".  This is
// the "reference implementation" which should be correct for any
// binary unsigned integral UINT with an even number of bits.  In
// practice, we try to avoid this implementation because it is so slow
// (4 multiplies plus about a dozen xor, or, +, shift, mask and
// compare operations.
template <typename Uint>
inline std::pair<Uint, Uint>
mulhilo_halfword(Uint a, Uint b){ 
    static_assert(std::numeric_limits<Uint>::is_specialized &&
                  std::numeric_limits<Uint>::is_integer &&
                  !std::numeric_limits<Uint>::is_signed &&
                  std::numeric_limits<Uint>::radix == 2 &&
                  std::numeric_limits<Uint>::digits%2 == 0,
                  "you can't do half-word multiplies with this type"
                  );
    const unsigned WHALF = std::numeric_limits<Uint>::digits/2;
    const Uint LOMASK = ((Uint)(~(Uint)0)) >> WHALF;
    Uint lo = a*b;
    Uint ahi = a>>WHALF;
    Uint alo = a& LOMASK;
    Uint bhi = b>>WHALF;
    Uint blo = b& LOMASK;
                                                                   
    Uint ahbl = ahi*blo;
    Uint albh = alo*bhi;
                                                                   
    Uint ahbl_albh = ((ahbl&LOMASK) + (albh&LOMASK));
    Uint hi = (ahi*bhi) + (ahbl>>WHALF) +  (albh>>WHALF);
    hi += ahbl_albh >> WHALF;
    /* carry from the sum with alo*blo */                               
    hi += ((lo >> WHALF) < (ahbl_albh&LOMASK));
    return {lo, hi};
}

// We can formulate a much faster implementation if we can use
// integers of twice the width of Uint (e.g., DblUint).  Such types
// are not always available (e.g., when Uint is uintmax_t), but when
// they are, we find that modern compilers (gcc, MSVC, Intel) pattern
// match the structure of the mulhilo below and turn it into an
// optimized instruction sequence, e.g., mulw or mull.
//
// However, the alternative implementation, which we want to use
// when there *is* a DblUint would be ambiguous without some enable_if
// hackery.  To support that, we need the twice_as_wide type trait.
//
template<class T>
struct twice_as_wide{ static constexpr bool specialized = false; };

template<> struct twice_as_wide<uint8_t>{ typedef uint16_t dwtype; typedef uint8_t type;};
template<> struct twice_as_wide<uint16_t>{ typedef uint32_t dwtype; typedef uint16_t type;};
template<> struct twice_as_wide<uint32_t>{ typedef uint64_t dwtype; typedef uint32_t type;};
template<> struct twice_as_wide<int8_t>{ typedef int16_t dwtype; typedef int8_t type;};
template<> struct twice_as_wide<int16_t>{ typedef int32_t dwtype; typedef int16_t type;};
template<> struct twice_as_wide<int32_t>{ typedef int64_t dwtype; typedef int32_t type;};

template <typename Uint>
inline std::pair<Uint, Uint>
mulhilo(Uint a, Uint b, typename std::enable_if<!twice_as_wide<Uint>::specialized>::type* = 0){
    return mulhilo_halfword(a, b);
}

template <typename Uint>
inline std::pair<Uint, Uint>
mulhilo(Uint a, Uint b, typename twice_as_wide<Uint>::type* = 0){
    typedef typename twice_as_wide<Uint>::dwtype DblUint;
    DblUint product = ((DblUint)a)*((DblUint)b);
    return {product, product>>std::numeric_limits<Uint>::digits};
}

// Every ISA I know (x86, ppc, arm, CUDA) has an instruction that
// gives the hi word of the product of two uintmax_t's FAR more
// quickly and succinctly than a call to mulhilo_halfword.  Without
// them, philox<N, uintmax_t> would be impractically slow.
// Unfortunately, they require compiler-and-hardware-specific
// intrinsics or asm statements.
//
// FIXME - add more special cases here, e.g., MSVC intrinsics and
// asm for PowerPC and ARM.
#if defined(__GNUC__)
#if defined(__x86_64__)
// N.B.  We're mixing "plain old" function overloading with "template
// specialization" here, which explores some very deep corners of
// the C++ overload rules.  It "seems to work", but don't be surprised
// if there are mysterious issues with integral promotions, ambiguous
// overloads, etc.
inline std::pair<uint64_t, uint64_t>
mulhilo(uint64_t ax, uint64_t b){
    uint64_t dx;
    __asm__("\n\t"
        "mulq %2\n\t"
        : "=a"(ax), "=d"(dx)
        : "r"(b), "0"(ax)
        );
    return {ax, dx};
}
#endif // __x86_64__ 
#endif // __GNUC__ (for gnu-style asm)

// rotl - rotate left by s
//  We expect the compiler to turn this into a single 'rol'
//  instruction.  Gcc, clang and icc do.
//  WARNING - behavior is implementation-defined when s<=0 or
//   s>=number of digits in Uint.  "Fixing" this corner case would be
//   costly for non-const s and seems to impede the compiler's
//   ability to recognize this as a 'rol' instruction.  Since we
//   typically use this in performance-critical code (hashes and
//   random number generators), there are no "training wheels"
//   or guard-rails.  DO NOT CALL IT WITH s==0 OR s>=#UintBits.
template <typename Uint>
constexpr Uint rotl(Uint x, unsigned s){
    static_assert(std::is_unsigned<Uint>::value, "core123::rotl<Uint>:  Uint must be an unsigned integral type");
    return (x<<s) | (x>>(std::numeric_limits<Uint>::digits-s));
}

// bitmask - set 'nsetbits' contiguous bits, starting at 'startbits' in
//    an intgral type, T.
template<typename T>
constexpr T bitmask(unsigned nsetbits, unsigned startbit=0){
    static_assert(std::is_integral<T>::value, "core123::mask<T>:  T must be an integral type");
    static_assert(std::numeric_limits<T>::radix == 2, "core123::mask<T>:  T must have radix 2");
    using UT = typename std::make_unsigned<T>::type;
    if( startbit + nsetbits > std::numeric_limits<UT>::digits )
        throw std::overflow_error("core123::mask:  too many bits for return type");
    return static_cast<T>( (nsetbits==0) ? UT{} : (std::numeric_limits<UT>::max()>>(std::numeric_limits<UT>::digits - nsetbits))<<startbit );
}

// Endian hackery is an enormous hairball.  Help is coming in C++20,
// but let's not wait...

// class std::endian will probably be in <type_traits> in C++20
enum class endian{
                  // These __XX__ are all defined by gcc, clang and icc.
                  little = __ORDER_LITTLE_ENDIAN__,
                  big = __ORDER_BIG_ENDIAN__,
                  native = __BYTE_ORDER__
};
                      
// std::byteswap will probably be in C++20.
template <class IntegerType>
constexpr IntegerType byteswap(IntegerType value) noexcept;

// These look like a lot of operations, but gcc, clang and
// icc all boil them down to a bswap instruction.  Alternatively,
// we could try something linux-specific like using the
// macros provided by #include <byteswap.h>.
template<>
constexpr uint16_t byteswap(uint16_t x) noexcept {
    return (((x>>8) & 0xff) | (x&0xff)<<8);
}    
template<>
constexpr uint32_t byteswap(uint32_t x) noexcept {
    return
        ((x & 0xff000000)>>24) |
        ((x & 0x00ff0000)>>8) |
        ((x & 0x0000ff00)<<8) |
        ((x & 0x000000ff)<<24);
}
template<>
constexpr uint64_t byteswap(uint64_t x) noexcept {
    return
        ((x & 0xff00000000000000)>>56) |
        ((x & 0x00ff000000000000)>>40) |
        ((x & 0x0000ff0000000000)>>24) |
        ((x & 0x000000ff00000000)>>8) |
        ((x & 0x00000000ff000000)<<8) |
        ((x & 0x0000000000ff0000)<<24) |
        ((x & 0x000000000000ff00)<<40) |
        ((x & 0x00000000000000ff)<<56);
}    

// std::popcount will be in C++20, but until then, we use either GNUC's __builtin_popcountl,
// or the fancy horizontal summation trick.

// N.B.  popcount_nobuiltin *should* work with gcc's __uint128_t, but
// it doesn't, at least through gcc8.1. I suspect a problem with
// numeric_limits, but I'm not sure...
template <class T>
inline constexpr
typename std::enable_if<std::numeric_limits<T>::is_specialized && !std::numeric_limits<T>::is_signed, int>::type popcount_nobuiltin(T v) noexcept{
    // From https://graphics.stanford.edu/~seander/bithacks.html
    static_assert( std::numeric_limits<T>::digits <= 128, "popcount_nobuiltin only works for widths up to 128");
    v = v - ((v >> 1) & (T)~(T)0/3);                           // temp
    v = (v & (T)~(T)0/15*3) + ((v >> 2) & (T)~(T)0/15*3);      // temp
    v = (v + (v >> 4)) & (T)~(T)0/255*15;                      // temp
    return (T)(v * ((T)~(T)0/255)) >> (sizeof(T) - 1) * CHAR_BIT; // count
}

template <class T>
inline constexpr
typename std::enable_if<std::is_unsigned<T>::value, int>::type popcount(T v) noexcept{
#if __cplusplus > 201703L
    return std::popcount(v);
#elif __GNUC__
    static_assert(std::numeric_limits<long unsigned>::digits >= std::numeric_limits<T>::digits, "popcount:  type too long for __builtin_popcountl");
    return __builtin_popcountl(v);
#else
    return popcount_nobuiltin(v);
#endif
}

// I don't see any propsals for these in C++2x.  Am I missing something?
template <class IntegerType>
constexpr IntegerType nativetobe(IntegerType x) noexcept{
    return endian::native == endian::big ? x : byteswap(x);
}

template <class IntegerType>
constexpr IntegerType nativetole(IntegerType x) noexcept{
    return endian::native == endian::little ? x : byteswap(x);
}

template <class IntegerType>
constexpr IntegerType betonative(IntegerType x) noexcept{
    return nativetobe(x);
}

template <class IntegerType>
constexpr IntegerType letonative(IntegerType x) noexcept{
    return nativetole(x);
}

template <typename Tlow, typename Tx, typename Thigh>
Tx clip(const Tlow& low, const Tx& x, const Thigh& high){
    if( high<x ) return high;
    if( x<low ) return low;
    return x;
}
} // namespace core123
