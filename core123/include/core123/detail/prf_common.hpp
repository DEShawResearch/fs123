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

#include <iostream>
#include <array>
#include <limits>

 // Because these are friends, we need the same declarations in each
 // of the derived classes, i.e., the "real" prfs: threefry, philox,
 // etc.  So make the macros to avoid typos and skew.
#define CORE123_DETAIL_OSTREAM_OPERATOR(OS, Self, arg) \
 template <class CharT, class Traits> \
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& OS, const Self& arg)

#define CORE123_DETAIL_ISTREAM_OPERATOR(IS, Self, arg)          \
 template <class CharT, class Traits> \
 friend std::basic_istream<CharT, Traits>& operator>>(std::basic_istream<CharT, Traits>& IS, Self& arg)

#define CORE123_DETAIL_EQUALITY_OPERATOR(Self, lhs, rhs) \
 friend bool operator==(const Self& lhs, const Self& rhs)

#define CORE123_DETAIL_INEQUALITY_OPERATOR(Self) \
 friend bool operator!=(const Self& lhs, const Self& rhs){      \
        return !(lhs==rhs);                              \
    }

namespace core123{
namespace detail{

template <unsigned Ndomain, unsigned Nrange, unsigned Nkey, typename DUint, typename RUint=DUint, typename KUint=DUint>
struct prf_common{
    typedef std::array<DUint, Ndomain> domain_type;
    typedef std::array<RUint, Nrange> range_type;
    typedef std::array<KUint, Nkey> key_type ;
    static constexpr unsigned domain_size = Ndomain;
    static constexpr unsigned range_size = Nrange;
    static constexpr unsigned key_size = Nkey;
    constexpr static typename domain_type::value_type domain_array_min()  { return std::numeric_limits<DUint>::min(); }
    constexpr static typename domain_type::value_type domain_array_max()  { return std::numeric_limits<DUint>::max(); }

    constexpr static typename range_type::value_type range_array_min()  { return std::numeric_limits<RUint>::min(); }
    constexpr static typename range_type::value_type range_array_max()  { return std::numeric_limits<RUint>::max(); }

    key_type k;
    prf_common(key_type _k) : k(_k){
        //std::cerr << "prf_common(key_type)\n";
    }
    void setkey(key_type _k){ k = _k; }
    key_type getkey() const { return k; }

    prf_common() { k = key_type(); }

    CORE123_DETAIL_OSTREAM_OPERATOR(os, prf_common, f){
        // FIXME - save the state of the os, and fiddle with things
        // like fill, flags, etc. to make sure we're doing the right
        // thing.
        for(auto e: f.k)
            os << ' ' << e;
        return os;
    }

    CORE123_DETAIL_ISTREAM_OPERATOR(is, prf_common, f){
        // FIXME - save the state of the is, and fiddle with things
        // like skipws, flags, etc, to make sure we're doing the right
        // thing.
        for(auto& eref : f.k)
            is >> eref;
        return is;
    }

    CORE123_DETAIL_EQUALITY_OPERATOR(prf_common, lhs, rhs){
        return lhs.k == rhs.k;
    }

    CORE123_DETAIL_INEQUALITY_OPERATOR(prf_common)
};


template<typename AType, typename Iter>
inline static Iter
stdarray_from_u32(AType& ret, Iter p32, Iter e32){
    using AV_t = typename AType::value_type;
    static_assert(std::is_unsigned<AV_t>::value, "stdarray_from_u32's AType must have an unsigned value_type");
    static_assert(std::numeric_limits<AV_t>::radix == 2, "C'mon.  You didn't really thing this would work with non base-2 integers, did you");
    constexpr size_t w = std::numeric_limits<AV_t>::digits;
    static_assert(w>0, "Huh?  An unsigned type with 0 digits???");
    constexpr size_t u32_per_value = (w - 1)/32 + 1;
    for(auto& e : ret){
        if(p32 == e32) throw std::out_of_range("stdarray_from_u32");
        e = uint32_t(*p32++);
        for(size_t j=1; j<u32_per_value; ++j){
            if(p32 == e32) throw std::out_of_range("stdarray_from_u32");
            e |= (AV_t(uint32_t(*p32++))) << (32*j);
        }
    }
    return p32;
}

template <typename AType>
inline static constexpr size_t
u32_for(){
    using AV_t = typename AType::value_type;
    static_assert(std::is_unsigned<AV_t>::value, "u32_for's AType must have an unsigned value_type");
    static_assert(std::numeric_limits<AV_t>::radix == 2, "C'mon.  You didn't really thing this would work with non base-2 integers, did you");
    constexpr size_t w = std::numeric_limits<AV_t>::digits;
    static_assert(w>0, "Huh?  An unsigned type with 0 digits???");
    constexpr size_t u32_per_value = (w - 1)/32 + 1;
    return std::tuple_size<AType>::value*u32_per_value;
}

} // namespace detail
} // namespace core123
