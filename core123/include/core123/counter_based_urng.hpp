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

#include <stdexcept>
#include <cstdint>
#include "detail/stdarray_from_seedseq.hpp"

namespace core123{
template<typename Prf, size_t CtrBits=32>
class counter_based_urng{
    // The counter_based_urng is a Uniform Random Number Engine
    // that will deliver :
    //    (2^CtrBits - 1)* W
    // random values, where:
    //    W = Prf::range_type.size()

    // For the threefry and philox prfs, this is only(?) a few x
    // 2^32.  When the range_type is an array of chars, this is
    // several hundred, which should still be sufficient for many
    // practical purposes because it's so easy to create billions of
    // distinct cburngs.  However, it's also easy to accidentally
    // generate and consume billions of random values, so rather than
    // "wrapping around", operator()() and discard() throw an
    // out_of_range error rather when the counter is exhausted.

    // WARNING: counter_based_urngs with different values of the
    // CtrBits template parameter are not guaranteed to be
    // non-overapping.

    // ADVICE: Don't use a non-default value for CtrBits.  If you
    // must, then make absolutely sure that every cburng in your
    // project uses the *same* value of CtrBits.

    // We abandoned CtrBits in Random123 because of the risk of
    // collision.  Consider:
    //
    //   counter_based_urng<threefry<2,uint32_t>, 2> u2;
    //   counter_based_urng<threefry<2,uint32_t>, 3> u3;
    //
    //   u2.setdomain({1, 2, 3, 1});
    //   u3.setdomain({1, 2, 3, 0});
    //
    // These "look" different: they've been initialized with clearly
    // different domains.

    // BUT, since they have different CtrBits, after 8
    // invocations, u3 starts giving the same values as the initial
    // values from u2.

    // ISSUES:
    //  - is 'domain_type' really a better term than 'counter_type'?
    //  - TESTING!!!!   The CtrBits is completely untested!
    //    Overflows aren't well-tested either.

    // PRF Requirements:
    //  FIXME - THIS IS ALL WAY TOO COMPLICATED!  We should either
    //  concede that we only deal with std::arrays of unsigned
    //  integral types -or- we should have an actual use case with
    //  something else (e.g., ARS with something like r123::m128i).
    //  This pseudo-standardese is a waste of time!
    //
    //  The counter_based_engine should work with any PRF that
    //  "satisfies the requirements for a Pseudo-Random Function".
    //  Those requirements are:
    //
    //   PRF must be Equality-Comparable.
    //   PRF must be Stream-Insertable and Stream-Extractable
    //    
    //   Let RType be the type named by PRF's associated range_type
    //
    //   RType::value_type returns an unsigned integer type, called
    //     rvtype
    // 
    //   std::tuple_size<RType>::value returns an unsigned integer.
    //
    //   RType::value_type returns the same type as PRF::result_type
    //
    //   Let rvtype be the type named by RType::value_type
    //
    //   Let r be a value of Rtype and n be an integer in the range 0.. std::tuple_size<RType>-1
    // 
    //   r[n] returns a reference to a value of rvtype
    //
    //   std::begin(r) and std::end(r) are forward iterators
    //      with a value_type of rvtype
    //  
    //   r.back() returns the same reference as r[std::tuple<RType>-1]
    //
    //   Let DType be the type named by PRF's associated domain_type
    //
    //   Let d be a value of type DType.
    //
    //   d.back() returns a reference to an integral type.
    //
    //   p(d) returns a value of type PRF::range_type
    //   

public:
    typedef Prf prf_type;
    typedef typename prf_type::domain_type domain_type;
    typedef typename prf_type::key_type key_type;

    // This is the recommended constructor: args are a Prf and a
    // counter (optional).  This particular constructor is unique to
    // counter_based_urng, and is not required by any standardized
    // concepts.
    counter_based_urng(prf_type _b, domain_type _c0 = domain_type())
        : b(_b), c(_c0)
    {
        fix_cback();
        setnth(0);
    }

    // The standard convention is that if there's a constructor,
    // then there's a corresponding 'seed' method.  Let's not
    // buck convention.
    void seed(prf_type _b, domain_type _c0 = domain_type()){
        *this = counter_based_urng(_b, _c0);
    }
        
    // It's easy and fast(!) to change the domain of a cburng.
    void setdomain(domain_type cnew){
        c = cnew;
        fix_cback();
        setnth(0);
    }

    domain_type getdomain() const{
        return unfix_cback();
    }

    //--------------------- URNG methods ----------------------//
    // result_type, operator()(), min() and max() satisfy the
    // requirements of a Uniform Random Bit Generator (29.6.1.3
    // [rand.req.urng]
    typedef typename prf_type::range_type::value_type result_type; 
    // default_seed isn't required, but all the standard Engines
    // have one.  We might as well have one too...
    static constexpr result_type default_seed = 321741185;

    result_type operator()(){
        if(range_type_size == 1){
            auto ret = b(c)[0];
            c.back()++;
            return ret;
        }
            
        // nth() returns a reference to the index of last rdata value
        // returned.  If it's 0, restock rdata by calling the prf
        // again.
        if(nth()-- == 0){
            c.back()++;
            if((c.back() & CtrMask) == 0)
                throw std::out_of_range("too many requests for counter_based_urng");
            rdata = b(c);
            auto ret = nth();
            nth() = range_type_size-1;
            return ret;
        }
        return rdata[nth()];
    }

    constexpr static result_type min () { return Prf::range_array_min(); } // per 29.6.1.3 [rand.req.urng]
    constexpr static result_type max () { return Prf::range_array_max(); } // per 29.6.1.3 [rand.req.urng]

    //----------------------- Engine methods --------------------------//
    // The remainder of the public interface is to satisfy the requirements of
    // a Uniform Random Number Engine (29.6.1.4 [rand.req.eng])

    void discard(unsigned long long skip){
        size_t nelem = range_type_size;
        size_t rem = skip%nelem;
        skip /= nelem;
        auto newnth = nth();
        if (newnth < rem) {
            newnth += nelem;
	    skip++;
        }
        newnth -= rem;
        
        auto before = (c.back() & CtrMask);
        c.back() += skip;
        if( (skip & CtrMask) != skip ||
            (c.back()&CtrMask) < before )
            throw std::out_of_range("too many discards for counter_based_urng");
        setnth(newnth);
    }

    // copy constructor
    counter_based_urng(const counter_based_urng& e) : b(e.b), c(e.c){
        setnth(e.nth());
    }
    
    // We need a non-const copy constructor to preclude the SeedSeq template
    // matching a constructor called with a non-const arg.
    counter_based_urng(counter_based_urng& e)
        : counter_based_urng(const_cast<const counter_based_urng&>(e)){
    }
    
    // result_type constructor -  I think this is misguided, but it's
    // required :-(
    counter_based_urng(result_type r = default_seed)
        // Use a lambda because:
        //  : counter_based_urng(detail::stdarray_from_seed_seq<key_type>(std::seed_seq{r}))
        // calls stdarray_seed_seq *const* reference, but it needs a
        // non-const reference.
        : counter_based_urng([r](){ std::seed_seq ss={r}; return detail::stdarray_from_seedseq<key_type>(ss); }()){
    }

    // seed_seq constructor - 
    template<class SeedSeq>
    explicit counter_based_urng(SeedSeq& seq, typename std::enable_if<!std::is_convertible<SeedSeq, result_type>::value && !std::is_convertible<SeedSeq, key_type>::value>::type* = 0) : 
        counter_based_urng(detail::stdarray_from_seedseq<key_type>(seq)) {
    }
        
    // seed methods - it's less error-prone, and only minimally slower
    // to call the constructor and copy-assignment.  Note that prf's
    // might have a time-consuming key-based constructors (e.g., AES),
    // but the the copy constructor should always be quick.
    void seed(result_type r = default_seed){
        *this = counter_based_urng(r);
    }
    
    template<class SeedSeq>
    void seed(SeedSeq& seq, typename std::enable_if<!std::is_convertible<SeedSeq, result_type>::value>::type* = 0){
        *this = counter_based_urng(seq);
    }

    counter_based_urng& operator=(const counter_based_urng& rhs){
        b = rhs.b;
        c = rhs.c;
        setnth(rhs.nth());
        return *this;
    }

    CORE123_DETAIL_EQUALITY_OPERATOR(counter_based_urng, lhs, rhs){
        return lhs.c==rhs.c && 
            lhs.nth() == rhs.nth() && 
            lhs.b == rhs.b; 
    }

    CORE123_DETAIL_INEQUALITY_OPERATOR(counter_based_urng)

    CORE123_DETAIL_OSTREAM_OPERATOR(os, counter_based_urng, f){
        // FIXME - We should be temporarily setting os.fmtflags and
        // fill character??
        os << (f.nth());
        for(typename domain_type::const_iterator p=f.c.begin(); p!=f.c.end(); ++p)
            os << ' ' << *p;
        os << ' ' << f.b;
        return os;
    }

    CORE123_DETAIL_ISTREAM_OPERATOR(is, counter_based_urng, f){
        // FIXME - we should be temporarily setting is.fmtflags
        // and we should not damage f if there's a read error.
        size_t n;
        is >> n;
        for(typename domain_type::iterator p=f.c.begin(); p!=f.c.end(); ++p)
            is >> *p;
        is >> f.b;
        if(n >= f.range_type_size)
            throw std::out_of_range("n too large for this counter");
        f.setnth(n);
        return is;
    }

protected:
    using range_type = typename prf_type::range_type;
    prf_type b;
    domain_type c;
    range_type rdata;

    using domain_value_type = typename domain_type::value_type;
    static constexpr size_t DomainWidth = std::numeric_limits<domain_value_type>::digits;
    static_assert(CtrBits <= DomainWidth, "the counter must fit in one domain_type::counter_type");
    static_assert(CtrBits > 0, "It doesn't make sense to have a 0-bit counter");
    static constexpr bool full_width_counter = (CtrBits == DomainWidth);
    static constexpr domain_value_type CtrMask = (~domain_value_type(0))>>(DomainWidth-CtrBits);
    static constexpr size_t range_type_size = std::tuple_size<range_type>::value;

    result_type const& nth() const { return rdata.back(); }
    result_type& nth() { return rdata.back(); }

    void setnth(size_t n){
        if( n != 0 ){
            rdata = b(c);
        }
        rdata.back() = n;
    }

    void fix_cback(){
        if(full_width_counter){
            if(c.back())
                throw std::out_of_range("Initial value of counter_based_urng's counter must be 0");
        }else{
            auto before = c.back();
            c.back() <<= CtrBits;
            if(c.back() >> CtrBits != before)
                throw std::out_of_range("Initial value of counter_based_urng's counter must leave the top CtrBits clear");
        }
    }

    domain_type unfix_cback() const {
        domain_type ret = c;
        if(full_width_counter)
            ret.back() = 0;
        else
            ret.back() >>= CtrBits;
        return ret;
    }
};
} // namespace core123
