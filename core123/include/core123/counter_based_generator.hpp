// counter_based_generator: An adapter that turns a 'psuedo random
// function' (e.g., core123::threefry or core123::philox) into a bona
// fide standard C++ "Uniform Random Bit Generator", i.e., something
// you can use with the "Random Number Distributions" in <random>.
//
// Usage:
//    
//    // Initial setup:
//    // Construct a keyed psuedo-random function:
//    using prf_t = threefry<4, uint64_t>;
//    prf_t::key_type k = {1234};
//    prf_t myprf(k);
//    ...
//
//    // Repeatedly, in the body of your code:
//
//    prf_t::domain_type iv = ...; // application-dependent
//    // Create a "limited-use" uniform random bit generator
//    auto gen = make_counter_based_generator<8>(myprf, c0);
//    // Use gen as you would any other generator (except
//    // don't call it more than gen.sequence_length() times (1024 in this case)
//    std::normal_distribution nd<float>;
//    float gaussian = nd(gen);
//
// The C++ standard states requirements for "Uniform Random
// Bit Generators" in 29.6.1.3 [rand.req.urng],
// "Random Number Distributions" in 29.6.2.6 [ rand.req.dist].
//
// Generators have a result_type, an operator()() and min() and max()
// methods.  Distributions take bit generators (not engines!) as
// arguments.
//
// A counter_based_generator (defined here) satisfies the requirements
// of a Uniform Random Bit Generator.  Therefore, a counter_based_generator
// may be used with any of the standard 'Distributions' (normal_distribution,
// uniform_distribution, poisson_distribution, etc.)
//
// The C++ standard further specifies requirements for "Random Number
// Engines" in 29.6.2.4 [rand.req.eng].  Engines must satisfy all the
// requirements of generators, and, in addition, are required to
// support seeding and construction, equality-comparison, discard,
// reproducibility, and stream insertion and extraction.
//
// A counter_based_generator (defined here) supports all of the
// same requirements *except* for seeding and construction.
//
// Instead, counter_based_generators are constructed from a
// pseudo-random function (prf) and an initial value in the
// prf's domain.
//
// One can think of the the pseudo-random function's 'key' as a
// 'master seed', and the 'initial value' as a separate 'per-sequence seed'
// that defines an uncorrelated sequence of 2^BITS random values.
// In the example above, there are 2^250 permissible values for the
// 'initial value', iv, each of which defines a sequence of length
// 1024.  
//
// In general, a counter_based_generator only produces:
//    2^BITS * PRF::range_size
// random values.  The static member function sequence_length()
// returns this value if it is representable as an unsigned long long,
// and returns the maximum unsigned long long otherwise.  It is an
// error (reported by throwing out_of_range) to request more than the
// maximum number of random values (either by operator() or by
// discard()).
//
// A member function: navail() returns (at least) the number of values
// that are still available to be generated.  If the number exceeds
// the maximum value representable by unsigned long long, then the
// maximum unsigned long long value is returned.
//
// The reinit() member function can be used to restart a new sequence
// with a new initial value.

#include "strutils.hpp"
#include <limits>
#include <stdexcept>
#include <array>
#include <iostream>

namespace core123{

// Details (these could have been done differently, but they all
//   have to be consistent):
// - The constructor does *not* call the prf.
// - The prf is called to 'refill' the array of randoms (r) when
//   the generator (operator()()) is called the first time, and
//   then again on the Ndomain'th time, the 2*Ndomain'th time, etc.
// - In between calls to 'refill', values are returned from the
//   randoms (r) array.  The index of the next value to return
//   is stored in r[0] (saving one word of memory).
// - The first time the prf is called, the high BITS of the counter
//   are zero.
// - The counter is incremented immediately after
//   the prf is called.  Thus, the 'r' member is *not* the prf
//   of the 'c' member.
// - The sequence is exhausted when we try to 'refill' the 2^BIT'th
//   time, at which point, the high bits of the counter have wrapped
//   around to zero.  We distinguish this from the very first
//   invocation of refill by initializing the structure with an
//   otherwise impossible value of r[0].  This adds some special-case
//   logic to operator>>, discard, navail, etc., but the constructor
//   and operator() are reasonably clean.
// - sequence_length() and navail() are tricky because the "correct"
//   answer may not fit in an unsigned long long.  

template <typename PRF, unsigned BITS>
struct counter_based_generator{
private:
    using domain_type = typename PRF::domain_type;
    using range_type = typename PRF::range_type;
    using dv_type = typename domain_type::value_type;
    using rv_type = typename range_type::value_type;
    static constexpr int W = std::numeric_limits<dv_type>::digits;
    static_assert(BITS <= W, "BITS must be less than or equal to the PRF's domain value_type");
    static constexpr auto Ndomain = PRF::domain_size;
    static constexpr auto Nrange = PRF::range_size;
    // Shift operators are very weird in two ways:
    //  1 - they're undefined when the shift amount is equal
    //      to the width of the operand
    //  2 - they never return anything narrower than unsigned int.
    // There are several places in the code below where we'd
    // naturally write n << (W-BITS) or n >> (W-BITS).  But
    // with BITS==0, those expressions would be undefined.
    // A ternary conditional avoids the undefined behavior.
    // Wrapping it in a function coerces the returned value
    // back to dv_type and also *seems* to silence spurious
    // -Wshift-count-overflow warnings.
    static constexpr dv_type LeftWB(dv_type d){
        return BITS ? d<<(W-BITS) : 0;
    }
    static constexpr dv_type RightWB(dv_type d){
        return BITS ? d>>(W-BITS) : 0;
    }
    static constexpr dv_type ctr_mask = LeftWB(~dv_type(0));
    static constexpr dv_type incr = LeftWB(dv_type(1));
    PRF prf;
    domain_type c;
    range_type r;
    rv_type refill(){
        // check for overflow, call the prf, increment c
        // swap r with prf[0], and return what was in prf[0].
        if((c.back() & ctr_mask) == 0 &&
           (r[0] != Nrange+2)){
            r[0] = Nrange;      // reset to throw if called again
            throw std::out_of_range("counter_based_generator::refill:  out of counters");
        }
        r = prf(c);
        c.back() += incr;
        auto ret = r[0];
        r[0] = 1;
        return ret;
    }

    static bool hibitsdirty(dv_type v){
        return v & ctr_mask;
    }

    static constexpr unsigned long long ullmax = std::numeric_limits<unsigned long long>::max();
    
public:
    using result_type = rv_type;
    counter_based_generator(const PRF& prf_, const typename PRF::domain_type& iv) :
        prf(prf_),
        c(iv)
    {
        reinit(iv);
    }
    
    counter_based_generator() : counter_based_generator({}, {}) {
        // Advance r[0] so that the next call to operator() throws an out_of_range
        r[0] = Nrange;
    }

    result_type operator()(){
        auto idx = r[0]++;
        if(idx >= Nrange)
            return refill();
        return r[idx];
    }

    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

    void reinit(const typename PRF::domain_type& iv){
        if( hibitsdirty(iv.back())  )
            throw std::invalid_argument("cbgenerator<BITS>::reset:  high BITS of counter not zero");
        c = iv;
        r[0] = Nrange+1;
    }

    void discard(unsigned long long n){
        if(n==0)
            return;
        auto nfill = n/Nrange;
        auto nextra = n%Nrange;
        auto r0 = (r[0]==Nrange+1) ? Nrange : r[0]; // unwind the tricky first-time special case
        r0 += nextra;
        if(r0 > Nrange){
            nfill++;
            r0 -= Nrange;
        }
        // Check for overflow.  Is nfill too big?
        if(nfill){
            dv_type dv_nfillm1 = nfill-1;
            if( RightWB(LeftWB(dv_nfillm1)) != nfill-1 ){
                // queue up an overflow if operator() is called again
                c.back() &= ~ctr_mask;
                r[0] = Nrange;       
                throw std::out_of_range("discard too large");
            }
            c.back() += dv_nfillm1*incr;
            r[0]++;             // more wacky first-time stuff.
            refill(); // if this throws, do we 
        }
        r[0] = r0;
    }

    unsigned long long navail() const {
        using ull = unsigned long long;
        if(r[0] == Nrange+1)
            return sequence_length();
        ull ctr = RightWB(c.back());
        if(ctr == 0) // we've wrapped.  What's in r[] is all that's left.
            return Nrange - r[0];
            
        ull cmaxm1 = RightWB(ctr_mask);
        ull cleft = cmaxm1 - ctr + 1;
        
        if( cleft > ullmax/Nrange )
            return ullmax;
        else
            return Nrange * cleft + (Nrange-r[0]);
    }

    static constexpr unsigned long long sequence_length(){
        unsigned long long cmax = RightWB(ctr_mask);
        if( cmax >= ullmax/Nrange )
            return ullmax;
        else
            return Nrange * (cmax+1);
    }
    
    bool operator==(const counter_based_generator& rhs) const {
        return c == rhs.c && r[0] == rhs.r[0] && prf == rhs.prf;
    }

    bool operator!=(const counter_based_generator& rhs) const {
        return !operator==(rhs);
    }

    friend std::ostream& operator<<(std::ostream& os, const counter_based_generator&g){
        return os << g.prf << " " << insbe(g.c) << " " << g.r[0];
    }
    
    friend std::istream& operator>>(std::istream& is, counter_based_generator&g){
        PRF inprf;
        range_type inc;
        result_type inr0;
        is >> inprf;
        for(auto& cv : inc)
            is >> cv;
        is >>  inr0;
        if(is){
            if(inr0 > Nrange+1 ||
               (inr0 == Nrange+1 && hibitsdirty(inc.back()))){
                is.setstate(std::ios::failbit);
                return is;
            }
            g.c = inc;
            g.prf = std::move(inprf);
            inc.back() -= incr; // backwards!
            g.r = g.prf(inc);
            g.r[0] = inr0;
        }
        return is;
    }
};

template <unsigned BITS, typename PRF>
auto make_counter_based_generator(const PRF& prf, const typename PRF::domain_type& c0){
    return counter_based_generator<PRF, BITS>(prf, c0);
}
    
} // namespace core123
