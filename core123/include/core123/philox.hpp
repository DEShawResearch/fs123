/** @page LICENSE
Copyright 2010-2012, D. E. Shaw Research.
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
#include <limits>
#include <tuple>
#include "intutils.hpp" // for mulhilo
#include "detail/prf_common.hpp"

namespace core123{
template <unsigned _N, typename Uint>
struct philox_constants{
    // specializations will hold the Mutlipliers: M0, M1
    // and the Weyl constants:  W0, W1
};

template <>
struct philox_constants<2, uint64_t>{
    static const uint64_t M0 = UINT64_C(0xD2B74407B1CE6E93);
    static const uint64_t W0 = UINT64_C(0x9E3779B97F4A7C15);
};

template <>
struct philox_constants<2, uint32_t>{
    static const uint32_t M0 = UINT32_C(0xD256D193);
    static const uint32_t W0 = UINT32_C(0x9E3779B9);
};

template <>
struct philox_constants<4, uint64_t>{
    static const uint64_t M0 = UINT64_C(0xD2E7470EE14C6C93);
    static const uint64_t M1 = UINT64_C(0xCA5A826395121157);
    static const uint64_t W0 = UINT64_C(0x9E3779B97F4A7C15);  /* golden ratio */
    static const uint64_t W1 = UINT64_C(0xBB67AE8584CAA73B);  /* sqrt(3)-1 */
};

template <>
struct philox_constants<4, uint32_t>{
    static const uint32_t M0 = UINT32_C(0xD2511F53);
    static const uint32_t M1 = UINT32_C(0xCD9E8D57);
    static const uint32_t W0 = UINT64_C(0x9E3779B9);  /* golden ratio */
    static const uint32_t W1 = UINT64_C(0xBB67AE85);  /* sqrt(3)-1 */
};

template <unsigned N, typename Uint, unsigned R=10, typename Constants = philox_constants<N, Uint> >
struct philox{
    static_assert( N%2 == 0 , "N must be even");
};

template <typename Uint, unsigned R, typename Constants>
struct philox<2, Uint, R, Constants> : public detail::prf_common<2, 2, 1, Uint>{
private:
    typedef detail::prf_common<2, 2, 1, Uint> common_type;
    typedef typename common_type::domain_type _ctr_type;
    typedef typename common_type::key_type _key_type;
    static inline void round(_ctr_type& ctr, _key_type& key){
        Uint lo, hi;
        std::tie(lo, hi) = mulhilo(Constants::M0, ctr[0]);
        _ctr_type out = {{hi^key[0]^ctr[1], lo}};
        ctr = out;
        key[0] += Constants::W0;
    }
public:
    philox() : common_type(){}
    philox(_key_type k) : common_type(k){}

    philox(const philox& v) : common_type(v){}

    _ctr_type operator()(_ctr_type c) const {
        _key_type kcopy = this->k;
        for(unsigned r=0; r<R; ++r)
            round(c, kcopy);
        return c;
    }
};

template<typename Uint, unsigned R, typename Constants>
struct philox<4, Uint, R, Constants> : public detail::prf_common<4, 4, 2, Uint>{
private:
    typedef detail::prf_common<4, 4, 2, Uint> common_type;
    typedef typename common_type::domain_type _ctr_type;
    typedef typename common_type::key_type _key_type;
    static inline void round(_ctr_type& ctr, _key_type& key){
        Uint lo0, hi0;
        Uint lo1, hi1;
        std::tie(lo0, hi0) = mulhilo(Constants::M0, ctr[0]);
        std::tie(lo1, hi1) = mulhilo(Constants::M1, ctr[2]);
        _ctr_type out = {{hi1^ctr[1]^key[0], lo1,
                                      hi0^ctr[3]^key[1], lo0}};
        ctr = out;
        key[0] += Constants::W0;
        key[1] += Constants::W1;
    }
public:
    philox() : common_type(){}
    philox(_key_type k) : common_type(k){}

    philox(const philox& v) : common_type(v){}

    _ctr_type operator()(_ctr_type c) const {
        _key_type kcopy = this->k;
        for(unsigned r=0; r<R; ++r)
            round(c, kcopy);
        return c;
    }
};

} // namespace core123
