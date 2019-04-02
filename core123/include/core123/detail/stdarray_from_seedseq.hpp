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

#include <random>

    // The more "interesting" constructors and seed methods require us
    // to manufacture a key_type (a std::array) from seed_seq.
    // There's no easy way to do it, so we provide these static
    // methods.  It could plausibly go in a separate, 'detail'
    // header file
    
namespace core123{
namespace detail{

template<typename AType, typename SeedSeq>
inline static AType
stdarray_from_seedseq(SeedSeq& seq){
    using AV_t = typename AType::value_type;
    static_assert(std::is_unsigned<AV_t>::value, "stdarray_seed_seq's AType must have an unsigned value_type");
    static_assert(std::numeric_limits<AV_t>::radix == 2, "C'mon.  You didn't really thing this would work with non base-2 integers, did you");
    static const size_t w = std::numeric_limits<AV_t>::digits;
    static_assert(w>0, "Huh?  An unsigned type with 0 digits???");
    constexpr size_t u32_per_value = (w - 1)/32 + 1;
    const size_t Ngen = std::tuple_size<AType>::value*u32_per_value;
    uint32_t u32[Ngen];                                 
    uint32_t *p32 = &u32[0];                            
    seq.generate(&u32[0], &u32[Ngen]);                  
    AType ret;
    for(auto& e : ret){
        e = *p32++;
        for(size_t j=1; j<u32_per_value; ++j)
            e |= (AV_t(*p32++)) << (32*j);
    }
    return ret;
}

} // namespace detail
} // namespace core123
