// counter_based_engine: An adapter that turns a 'psuedo random
// function' (e.g., core123::threefry or core123::philox) into a bona
// fide standard C++ "Random Number Engine", satisfying the
// requirements in [rand.req.eng] for a result_type, constructors,
// seed methods, discard, equality and inequality operators, stream
// insertion and extraction, etc., and also providing an "extended
// API" that exposes desirable features of the counter-based paradigm.
//
// counter_based_engine<BITS>::operator()() can be called
// 2^BITS*PRF::range_size times, after which it throws an out_of_range
// exception.  A large value of BITS (e.g., 64) allows for a generator
// that is practically infinite.  Smaller value of BITS are useful
// when using the extended API.
//
// The 'extended API allows a counter_based_engine to be
// constructed from a user-supplied pseudo-random function (prf) and
// an initial value in the prf's domain.  E.g.,
//
//    threefry<4, uint64_t> prf({... master seed...});
// 
//    for(...){
//       ...
//       counter_based_engine<threefry<4, uint64_t>, 8> eng(prf, {... per-obj-seed...});
//       // or equivalently, with less typing:
//       auto eng = make_counter_based_engine<8>(prf, {... per-obj-seed ...});
//       std::normal_distribution<float> nd;
//       gaussian = nd(eng)
//       ...
//    }
//
// When constructed this way, one can think of the the pseudo-random
// function's 'key' as a 'master seed', and the counter_based_engine's
// 'initial value', iv, as a separate 'per-object seed' that uniquely
// specifies a finite uncorrelated sequence of result_types.  The most
// significant BITS bits of the iv.back() must be zero.  (This is why
// one might want BITS<64).  In the example above, there are 2^256
// possible prfs, and 2^248 permissible values for the iv, each of
// which defines a sequence of length 1024.
//
// IMPORTANT: If the pseudo-random function is "strong", then all
// sequences generated by counter_based_engine, with differently
// keyed prf's and/or different values of the iv are UNIFORMLY
// DISTRIBUTED AND ARE STATISTICALLY INDEPENDENT FROM ONE ANOTHER.
// This is true EVEN IF ONLY BIT IS DIFFERENT IN THE PRF's KEYS OR THE
// INITIAL VALUES.  For strong pseudo-random functions, THERE IS NO
// NEED TO "RANDOMIZE" THE KEYS OR THE IVS.  IT IS SUFFICIENT FOR THEM
// TO BE UNIQUE.  The pseudo-random functions in core123 (threefry,
// philox and chacha) are all strong in this sense.
//
// Extended API:
//  Informational member functions
//
//   unsigned long long sequence_length() - returns
//    2^BITS*PRF::range_size if it is representable as an unsigned
//    long long, and returns the maximum unsigned long long otherwise.
//    It is an error (reported by throwing out_of_range) to request
//    more than the maximum number of random values (either by
//    operator() or by discard()).
//
//   unsigned long long navail() - returns (at least) the number of
//    values that are still available to be generated.  If the number
//    exceeds the maximum value representable by unsigned long long,
//    then the maximum unsigned long long value is returned.
//
//  Accessor member functions:
//
//   prf() - returns a copy of the pseudo-random function
//     currently in use by the engine.
//   iv()- returns the initial value that began the sequence
//     currently being generated.
//
//  Seeding and reinitialization:
//
//   seed(const PRF& prf, const PRF::domain_type& iv) - resets the
//     generator to one equivalent to counter_based_engine(prf(), iv()).
//
//   reinit(const PRF::domain_type& iv) - resets the generator
//     to one equivalent to counter_based_engine(prf(), iv).  I.e., 
//     it leaves the prf in place, but resets the initial value to iv.
//
// counter_based_engines that have been constructed or seeded using
// the conventional API (via the default constructor, the result_type
// constructor, the "seed sequence" constructor, or the analogous
// seed() members) will have a "randomly" keyed PRF and a "random" iv.
// The "random" key and iv will be constructed deterministically by
// calling the generate member of a "seed sequence" (either one that
// was provided, or std::seed_seq).

#include "strutils.hpp"
#include "detail/stdarray_from_seedseq.hpp"
#include "detail/prf_common.hpp"
#include <limits>
#include <stdexcept>
#include <array>
#include <iostream>
#include <type_traits>

namespace core123{

namespace detail{
template <typename SeedSeq, typename EngType, typename ResultType>
using enable_if_seed_seq = typename std::enable_if<!std::is_convertible<SeedSeq, ResultType>::value &&
                                                   !std::is_same<std::remove_reference_t<std::remove_cv_t<SeedSeq>>, EngType>::value
                                                   >::type;
}

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
struct counter_based_engine{
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
    // There are several places in the code below where we'd naturally
    // write n << (W-BITS) or n >> (W-BITS).  But with BITS==0, those
    // expressions would be undefined.  A ternary conditional avoids
    // the undefined behavior.  Wrapping it in a function coerces the
    // returned value back to dv_type and also *seems* to silence
    // bogus (because the dodgy code is unreachable)
    // -Wshift-count-overflow warnings.
    static constexpr dv_type LeftWB(dv_type d){
        return BITS ? d<<(W-BITS) : 0;
    }
    static constexpr dv_type RightWB(dv_type d){
        return BITS ? d>>(W-BITS) : 0;
    }
    static constexpr dv_type ctr_mask = LeftWB(~dv_type(0));
    static constexpr dv_type incr = LeftWB(dv_type(1));
    PRF f;
    domain_type c;
    range_type r;
    rv_type refill(){
        if((c.back() & ctr_mask) == 0 &&
           (r[0] != Nrange+2)){
            r[0] = Nrange;      // reset to throw if called again
            throw std::out_of_range("counter_based_engine::refill:  out of counters");
        }
        r = f(c);
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
    counter_based_engine(const PRF& prf_, const typename PRF::domain_type& iv)
    {
        seed(prf_, iv);
    }
    
    // The seed-oriented constructors all call seed.
    template <class SeedSeq, typename = detail::enable_if_seed_seq<SeedSeq, counter_based_engine, result_type>>
    explicit counter_based_engine(SeedSeq &q)
    {
        seed(q);
    }

    explicit counter_based_engine(result_type s)
    {
        seed(s);
    }
        
    counter_based_engine()
    {
        seed();
    }

    // The seed() members all forward to seed(SeedSeq), which
    // initializes both the PRF and the IV from two calls to
    // q.generate() (N.B., this is strictly a violation of the
    // requirements for a Random Number Engine because Table 92 in
    // [rand.req.eng] says we should make "one call to q.generate".
    void seed(result_type s){
        static constexpr unsigned N32 = (sizeof(result_type)-1) / sizeof(uint32_t) + 1;
        std::array<uint32_t, N32> a;
        for(unsigned i=0; i<N32; ++i){
            a[i] = s&0xffffffff;
            s >>= 32;
        }
        std::seed_seq q(a.begin(), a.end());
        seed(q);
    }
    void seed(){
        // The seed_seq is initialized with little-endian("core123::default")
        // (which should be different from anything initialized with either
        // a 32-bit or 64-bit result-type).
        std::seed_seq q({0x65726f63, 0x3a333231, 0x6665643a, 0x746c756});
        seed(q);
    }
    template <class SeedSeq, typename = detail::enable_if_seed_seq<SeedSeq, counter_based_engine, result_type>>
    void seed(SeedSeq &q){
        f = PRF{detail::stdarray_from_seedseq<typename PRF::key_type>(q)};
        auto c = detail::stdarray_from_seedseq<typename PRF::domain_type>(q);
        c.back() &= ~ctr_mask;
        reinit(c);
    }
        
    // A non-required seed function corresponding to our non-required constructor:
    void seed(const PRF& prf_, const typename PRF::domain_type& iv){
        f = prf_;
        reinit(iv);
    }

    result_type operator()(){
        auto idx = r[0]++;
        if(idx >= Nrange)
            return refill();
        return r[idx];
    }

    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

    // accessors for the prf and the iv.
    PRF prf() const{
        return f;
    }
    typename PRF::domain_type iv() const {
        auto ret = c;
        ret.back() &= ~ctr_mask;
        return ret;
    }
        
    // reinit is a mutator that changes the iv but leaves the prf
    // alone.  There's a seed() member that changes both.  There is no
    // mutator that changes the key and leaves the iv alone.  Should
    // reinit be called seed()?  If so, we need some extra enable_if
    // logic to avoid ambiguity with seed(SeedSeq).
    void reinit(const typename PRF::domain_type& iv_){
        if( hibitsdirty(iv_.back())  )
            throw std::invalid_argument("cbgenerator<BITS>::reset:  high BITS of counter not zero");
        c = iv_;
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
    
    friend bool operator==(const counter_based_engine& lhs, const counter_based_engine& rhs) {
        return lhs.c == rhs.c && lhs.r[0] == rhs.r[0] && lhs.f == rhs.f;
    }

    friend bool operator!=(const counter_based_engine& lhs, const counter_based_engine& rhs) {
        return !(lhs==rhs);
    }

    CORE123_DETAIL_OSTREAM_OPERATOR(os, counter_based_engine, g){
        os << g.f;
        for(const auto e : g.c)
            os << " " << e;
        return os << " " << g.r[0];
    }
    
    CORE123_DETAIL_ISTREAM_OPERATOR(is, counter_based_engine, g){
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
            g.f = std::move(inprf);
            inc.back() -= incr; // backwards!
            g.r = g.f(inc);
            g.r[0] = inr0;
        }
        return is;
    }
};

template <unsigned BITS, typename PRF>
auto make_counter_based_engine(const PRF& prf, const typename PRF::domain_type& c0){
    return counter_based_engine<PRF, BITS>(prf, c0);
}
    
} // namespace core123
