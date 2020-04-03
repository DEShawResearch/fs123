/** @page LICENSE
Copyright 2010-2017, D. E. Shaw Research.
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

#include <cstdint>
#include "detail/prf_common.hpp"
#include "intutils.hpp"  // for rotl

// threefry_constants is an abstract template that will be
// specialized with the KS_PARITY and Rotation constants
// of the threefry generators.  These constants are carefully
// chosen to achieve good randomization.  
//  threefry_constants<2, uint32_t>
//  threefry_constants<2, uint64_t>
//  threefry_constants<4, uint32_t>
//  threefry_constants<4, uint64_t>
// The constants here are from Salmon et al <FIXME REF>.
//
// See Salmon et al, or Schneier's original work on Threefish <FIXME
// REF> for information about how the constants were obtained.
namespace core123{
template <unsigned _N, typename Uint>
struct threefry_constants{
};

// 2x32 constants
template <>
struct threefry_constants<2, uint32_t>{
    static constexpr uint32_t KS_PARITY = UINT32_C(0x1BD11BDA);
    static constexpr unsigned Rotations(int r){
        switch(r%8){
        case 0: return 13;
        case 1: return 15;
        case 2: return 26;
        case 3: return 6;
        case 4: return 17;
        case 5: return 29;
        case 6: return 16;
        default: return 24;
        }
    }
};

// 4x32 contants
template <>
struct threefry_constants<4, uint32_t>{
    static constexpr uint32_t KS_PARITY = UINT32_C(0x1BD11BDA);
    static constexpr unsigned Rotations0(int r){
        switch(r%8){
        case 0: return 10;
        case 1: return 11;
        case 2: return 13;
        case 3: return 23;
        case 4: return 6;
        case 5: return 17;
        case 6: return 25;
        default: return 18;
        }
    }
    static constexpr unsigned Rotations1(int r){
        switch(r%8){
        case 0: return 26;
        case 1: return 21;
        case 2: return 27;
        case 3: return 5;
        case 4: return 20;
        case 5: return 11;
        case 6: return 10;
        default: return 20;
        }
    }
};

// 2x64 constants
template <>
struct threefry_constants<2, uint64_t>{
    static constexpr uint64_t KS_PARITY = UINT64_C(0x1BD11BDAA9FC1A22);
    static constexpr unsigned Rotations(int r){
        switch(r%8){
        case 0: return 16;
        case 1: return 42;
        case 2: return 12;
        case 3: return 31;
        case 4: return 16;
        case 5: return 32;
        case 6: return 24;
        default: return 21;
        }
    }
};

// 4x64 constants
template <>
struct threefry_constants<4, uint64_t>{
    static constexpr uint64_t KS_PARITY = UINT64_C(0x1BD11BDAA9FC1A22);
    static constexpr unsigned Rotations0(int r){
        switch(r%8){
        case 0: return 14;
        case 1: return 52;
        case 2: return 23;
        case 3: return 5;
        case 4: return 25;
        case 5: return 46;
        case 6: return 58;
        default: return 32;
        }
    }
    static constexpr unsigned Rotations1(int r){
        switch(r%8){
        case 0: return 16;
        case 1: return 57;
        case 2: return 40;
        case 3: return 37;
        case 4: return 33;
        case 5: return 12;
        case 6: return 22;
        default: return 32;
        }
    }
};

template <unsigned N, typename Uint, unsigned R=20, typename Constants=threefry_constants<N, Uint> >
struct threefry {
    static_assert( N==2 || N==4, "N must be 2 or 4" );
    // should never be instantiated.
    // Only the specializations on N=2 and 4 are
    // permitted/implemented.
};

// specialize threefry<2, Uint, R>
template<typename Uint, unsigned R, typename Constants>
struct threefry<2, Uint, R, Constants> : public detail::prf_common<2, 2, 2, Uint>{
private:
    typedef detail::prf_common<2, 2, 2, Uint> common_type;
    typedef typename common_type::domain_type _ctr_type;
    typedef typename common_type::key_type _key_type;
    
    static inline void round(Uint& c0, Uint& c1, int r){
        c0 += c1; c1 = rotl(c1,Constants::Rotations(r)); c1 ^= c0;
    }
    template <unsigned r>
    static inline void round(Uint& c0, Uint& c1){
        c0 += c1; c1 = rotl(c1,Constants::Rotations(r)); c1 ^= c0;
    }
    static inline void keymix(Uint& c0, Uint& c1, Uint* ks, unsigned r){
        unsigned r4 = r>>2;
        c0 += ks[r4%3]; 
        c1 += ks[(r4+1)%3] + r4;
    }

public:
    threefry() : common_type(){ 
    }
    threefry(_key_type _k) : common_type(_k){ 
    }

    threefry(const threefry& v) : common_type(v){
    }

    _ctr_type operator()(_ctr_type c) const { 
        Uint ks[3];
        ks[2] = Constants::KS_PARITY;
        Uint c0, c1;
        ks[0] = this->k[0]; ks[2] ^= this->k[0]; c0 = c[0] + this->k[0];
        ks[1] = this->k[1]; ks[2] ^= this->k[1]; c1 = c[1] + this->k[1];
        // Surprisingly(?), gcc (through gcc8) doesn't unroll the
        // loop.  If we unroll it ourselves, it's about twice as fast.
        if(R>0) round<0>(c0, c1);
        if(R>1) round<1>(c0, c1);
        if(R>2) round<2>(c0, c1);
        if(R>3) round<3>(c0, c1);
        if(R>3) keymix(c0, c1, ks, 4);

        if(R>4) round<4>(c0, c1);
        if(R>5) round<5>(c0, c1);
        if(R>6) round<6>(c0, c1);
        if(R>7) round<7>(c0, c1);
        if(R>7) keymix(c0, c1, ks, 8);

        if(R>8) round<8>(c0, c1);
        if(R>9) round<9>(c0, c1);
        if(R>10) round<10>(c0, c1);
        if(R>11) round<11>(c0, c1);
        if(R>11) keymix(c0, c1, ks, 12);
        
        if(R>12) round<12>(c0, c1);
        if(R>13) round<13>(c0, c1);
        if(R>14) round<14>(c0, c1);
        if(R>15) round<15>(c0, c1);
        if(R>15) keymix(c0, c1, ks, 16);

        if(R>16) round<16>(c0, c1);
        if(R>17) round<17>(c0, c1);
        if(R>18) round<18>(c0, c1);
        if(R>19) round<19>(c0, c1);
        if(R>19) keymix(c0, c1, ks, 20);
        for(unsigned r=20; r<R; ){
            round(c0, c1, r);
            ++r;
            if((r&3)==0){
                keymix(c0, c1, ks, r);
            }
        }
        return {c0, c1}; 
    }
};

// specialize threefry<4, Uint, R>
template<typename Uint, unsigned R, typename Constants>
struct threefry<4, Uint, R, Constants> : public detail::prf_common<4, 4, 4, Uint>{
private:
    typedef detail::prf_common<4, 4, 4, Uint> common_type;
    typedef typename common_type::domain_type _ctr_type;
    typedef typename common_type::key_type _key_type;
    static void round(Uint& c0, Uint& c1, Uint& c2, Uint& c3, unsigned r){
        if((r&1)==0){
            c0 += c1; c1 = rotl(c1,Constants::Rotations0(r)); c1 ^= c0;
            c2 += c3; c3 = rotl(c3,Constants::Rotations1(r)); c3 ^= c2;
        }else{
            c0 += c3; c3 = rotl(c3,Constants::Rotations0(r)); c3 ^= c0;
            c2 += c1; c1 = rotl(c1,Constants::Rotations1(r)); c1 ^= c2;
        }
    }
    // N.B.  icc 2018 produces terrible code if we call:
    //    round(c0, c1, c2, c3, R)
    // with a literal constant for R, but it produces perfectly fine
    // code if we call:
    //    round<R>(c0, c1, c2, c3).
    template <unsigned r>
    static void round(Uint& c0, Uint& c1, Uint& c2, Uint& c3){
        if((r&1)==0){
            c0 += c1; c1 = rotl(c1,Constants::Rotations0(r)); c1 ^= c0;
            c2 += c3; c3 = rotl(c3,Constants::Rotations1(r)); c3 ^= c2;
        }else{
            c0 += c3; c3 = rotl(c3,Constants::Rotations0(r)); c3 ^= c0;
            c2 += c1; c1 = rotl(c1,Constants::Rotations1(r)); c1 ^= c2;
        }
    }

    static void keymix(Uint& c0, Uint& c1, Uint& c2, Uint& c3, Uint* ks, unsigned r){
        unsigned r4 = r>>2;
        c0 += ks[(r4+0)%5]; 
        c1 += ks[(r4+1)%5];
        c2 += ks[(r4+2)%5];
        c3 += ks[(r4+3)%5] + r4;
    }
public:
    threefry() : common_type(){ 
    }
    threefry(_key_type _k) : common_type(_k){ 
    }

    threefry(const threefry& v) : common_type(v){
    }

    CORE123_DETAIL_OSTREAM_OPERATOR(os, threefry, f){
        return os << static_cast<const common_type&>(f);
    }

    CORE123_DETAIL_ISTREAM_OPERATOR(is, threefry, f){
        return is >> static_cast<common_type&>(f);
    }

    // N.B.  when #include-ed in simd_threefry.hpp, gcc says this, even with warnings disabled:
    // ../include/core123/threefry.hpp: In member function ‘core123::threefry<4, Uint, R, Constants>::_ctr_type core123::threefry<4, Uint, R, Constants>::operator()(core123::threefry<4, Uint, R, Constants>::_ctr_type) const [with Uint = __vector(8) long unsigned int; unsigned int R = 12; Constants = core123::threefry_constants<4, __vector(8) long unsigned int>]’:
    // ../include/core123/threefry.hpp:293:15: note: the ABI for passing parameters with 64-byte alignment has changed in GCC 4.6
    // AFAIK, this is a non-issue, but I don't know how to silence it.
    _ctr_type operator()(_ctr_type c) const { 
        Uint ks[5];
        Uint c0, c1, c2, c3;
        ks[4] = Constants::KS_PARITY;
        ks[0] = this->k[0]; ks[4] ^= this->k[0]; c0 = c[0] + this->k[0];
        ks[1] = this->k[1]; ks[4] ^= this->k[1]; c1 = c[1] + this->k[1];
        ks[2] = this->k[2]; ks[4] ^= this->k[2]; c2 = c[2] + this->k[2];
        ks[3] = this->k[3]; ks[4] ^= this->k[3]; c3 = c[3] + this->k[3];

        // Surprisingly(?), gcc (through gcc8) doesn't unroll the
        // loop.  If we unroll it ourselves, it's about twice as fast.
        if(R>0) round<0>(c0, c1, c2, c3);
        if(R>1) round<1>(c0, c1, c2, c3);
        if(R>2) round<2>(c0, c1, c2, c3);
        if(R>3) round<3>(c0, c1, c2, c3);
        if(R>3) keymix(c0, c1, c2, c3, ks, 4);

        if(R>4) round<4>(c0, c1, c2, c3);
        if(R>5) round<5>(c0, c1, c2, c3);
        if(R>6) round<6>(c0, c1, c2, c3);
        if(R>7) round<7>(c0, c1, c2, c3);
        if(R>7) keymix(c0, c1, c2, c3, ks, 8);

        if(R>8) round<8>(c0, c1, c2, c3);
        if(R>9) round<9>(c0, c1, c2, c3);
        if(R>10) round<10>(c0, c1, c2, c3);
        if(R>11) round<11>(c0, c1, c2, c3);
        if(R>11) keymix(c0, c1, c2, c3, ks, 12);
        
        if(R>12) round<12>(c0, c1, c2, c3);
        if(R>13) round<13>(c0, c1, c2, c3);
        if(R>14) round<14>(c0, c1, c2, c3);
        if(R>15) round<15>(c0, c1, c2, c3);
        if(R>15) keymix(c0, c1, c2, c3, ks, 16);

        if(R>16) round<16>(c0, c1, c2, c3);
        if(R>17) round<17>(c0, c1, c2, c3);
        if(R>18) round<18>(c0, c1, c2, c3);
        if(R>19) round<19>(c0, c1, c2, c3);
        if(R>19) keymix(c0, c1, c2, c3, ks, 20);
        for(unsigned r=20; r<R; ){
            round(c0, c1, c2, c3, r);
            ++r;
            if((r&3)==0){
                keymix(c0, c1, c2, c3, ks, r);
            }
        }
        return {c0, c1, c2, c3}; 
    }
};

} // namespace core123
