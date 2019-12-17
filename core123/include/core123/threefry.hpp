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
    static constexpr unsigned Rotations[8] =
        {13, 15, 26, 6, 17, 29, 16, 24};
};
constexpr unsigned
threefry_constants<2, uint32_t>::Rotations[8];

// 4x32 contants
template <>
struct threefry_constants<4, uint32_t>{
    static constexpr uint32_t KS_PARITY = UINT32_C(0x1BD11BDA);
    static constexpr unsigned Rotations0[8] = 
        {10, 11, 13, 23, 6, 17, 25, 18};
    static constexpr unsigned Rotations1[8] = 
        {26, 21, 27, 5, 20, 11, 10, 20};
};
constexpr unsigned
threefry_constants<4, uint32_t>::Rotations0[8];
constexpr unsigned
threefry_constants<4, uint32_t>::Rotations1[8];

// 2x64 constants
template <>
struct threefry_constants<2, uint64_t>{
    static constexpr uint64_t KS_PARITY = UINT64_C(0x1BD11BDAA9FC1A22);
    static constexpr unsigned Rotations[8] =
        {16, 42, 12, 31, 16, 32, 24, 21};
};
constexpr unsigned
threefry_constants<2, uint64_t>::Rotations[8];

// 4x64 constants
template <>
struct threefry_constants<4, uint64_t>{
    static constexpr uint64_t KS_PARITY = UINT64_C(0x1BD11BDAA9FC1A22);
    static constexpr unsigned Rotations0[8] = 
        {14, 52, 23, 5, 25, 46, 58, 32};
    static constexpr unsigned Rotations1[8]  = 
        {16, 57, 40, 37, 33, 12, 22, 32};
};
constexpr unsigned
threefry_constants<4, uint64_t>::Rotations0[8];
constexpr unsigned
threefry_constants<4, uint64_t>::Rotations1[8];

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
    
    static inline void round(_ctr_type& c, unsigned r){
        c[0] += c[1]; c[1] = rotl(c[1],Constants::Rotations[r%8]); c[1] ^= c[0];
    }
    static inline void keymix(_ctr_type& c, Uint* ks, unsigned r){
        unsigned r4 = r>>2;
        c[0] += ks[r4%3]; 
        c[1] += ks[(r4+1)%3] + r4;
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
        ks[0] = this->k[0]; ks[2] ^= this->k[0]; c[0] += this->k[0];
        ks[1] = this->k[1]; ks[2] ^= this->k[1]; c[1] += this->k[1];
        // Surprisingly(?), gcc (through gcc8) doesn't unroll the
        // loop.  If we unroll it ourselves, it's about twice as fast.
        if(R>0) round(c,0);
        if(R>1) round(c,1);
        if(R>2) round(c,2);
        if(R>3) round(c,3);
        if(R>3) keymix(c, ks, 4);

        if(R>4) round(c,4);
        if(R>5) round(c,5);
        if(R>6) round(c,6);
        if(R>7) round(c,7);
        if(R>7) keymix(c, ks, 8);

        if(R>8) round(c,8);
        if(R>9) round(c,9);
        if(R>10) round(c,10);
        if(R>11) round(c,11);
        if(R>11) keymix(c, ks, 12);
        
        if(R>12) round(c,12);
        if(R>13) round(c,13);
        if(R>14) round(c,14);
        if(R>15) round(c,15);
        if(R>15) keymix(c, ks, 16);

        if(R>16) round(c,16);
        if(R>17) round(c,17);
        if(R>18) round(c,18);
        if(R>19) round(c,19);
        if(R>19) keymix(c, ks, 20);
        for(unsigned r=20; r<R; ){
            round(c, r);
            ++r;
            if((r&3)==0){
                keymix(c, ks, r);
            }
        }
        return c; 
    }
};

// specialize threefry<4, Uint, R>
template<typename Uint, unsigned R, typename Constants>
struct threefry<4, Uint, R, Constants> : public detail::prf_common<4, 4, 4, Uint>{
private:
    typedef detail::prf_common<4, 4, 4, Uint> common_type;
    typedef typename common_type::domain_type _ctr_type;
    typedef typename common_type::key_type _key_type;
    static inline void round(_ctr_type& c, unsigned r){
        if((r&1)==0){
            c[0] += c[1]; c[1] = rotl(c[1],Constants::Rotations0[r%8]); c[1] ^= c[0];
            c[2] += c[3]; c[3] = rotl(c[3],Constants::Rotations1[r%8]); c[3] ^= c[2];
        }else{
            c[0] += c[3]; c[3] = rotl(c[3],Constants::Rotations0[r%8]); c[3] ^= c[0];
            c[2] += c[1]; c[1] = rotl(c[1],Constants::Rotations1[r%8]); c[1] ^= c[2];
        }
    }

    static inline void keymix(_ctr_type& c, Uint* ks, unsigned r){
        unsigned r4 = r>>2;
        c[0] += ks[(r4+0)%5]; 
        c[1] += ks[(r4+1)%5];
        c[2] += ks[(r4+2)%5];
        c[3] += ks[(r4+3)%5] + r4;
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

    _ctr_type operator()(_ctr_type c) const { 
        Uint ks[5];
        ks[4] = Constants::KS_PARITY;
        ks[0] = this->k[0]; ks[4] ^= this->k[0]; c[0] += this->k[0];
        ks[1] = this->k[1]; ks[4] ^= this->k[1]; c[1] += this->k[1];
        ks[2] = this->k[2]; ks[4] ^= this->k[2]; c[2] += this->k[2];
        ks[3] = this->k[3]; ks[4] ^= this->k[3]; c[3] += this->k[3];

        // Surprisingly(?), gcc (through gcc8) doesn't unroll the
        // loop.  If we unroll it ourselves, it's about twice as fast.
        if(R>0) round(c,0);
        if(R>1) round(c,1);
        if(R>2) round(c,2);
        if(R>3) round(c,3);
        if(R>3) keymix(c, ks, 4);

        if(R>4) round(c,4);
        if(R>5) round(c,5);
        if(R>6) round(c,6);
        if(R>7) round(c,7);
        if(R>7) keymix(c, ks, 8);

        if(R>8) round(c,8);
        if(R>9) round(c,9);
        if(R>10) round(c,10);
        if(R>11) round(c,11);
        if(R>11) keymix(c, ks, 12);
        
        if(R>12) round(c,12);
        if(R>13) round(c,13);
        if(R>14) round(c,14);
        if(R>15) round(c,15);
        if(R>15) keymix(c, ks, 16);

        if(R>16) round(c,16);
        if(R>17) round(c,17);
        if(R>18) round(c,18);
        if(R>19) round(c,19);
        if(R>19) keymix(c, ks, 20);
        for(unsigned r=20; r<R; ){
            round(c, r);
            ++r;
            if((r&3)==0){
                keymix(c, ks, r);
            }
        }
        return c; 
    }
};

} // namespace core123
