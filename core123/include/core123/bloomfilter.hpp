#pragma once
// Bloom filter using Bits class, so it can be checkpointed and restored
// via iostreams.  uses threeroe for hash.
// B. H. Bloom, Space/time trade-offs in hash coding with allowable errors, CACM v13i7 p422-426 1970
// https://dl.acm.org/citation.cfm?id=362692
// TODO: Would be nice to implement Scalable (or stackable) Bloom Filters per
// http://gsd.di.uminho.pt/members/cbm/ps/dbloom.pdf
// Mark Moraes, D. E. Shaw Research

#include <core123/threeroe.hpp>
#include <core123/diag.hpp>
#include <core123/strutils.hpp>
#include <core123/bits.hpp>
#include <cmath>

namespace core123 {
const static auto _bloom = diag_name("bloom");

// utility free functions to estimate various parameters from others
inline size_t bloom_estimate_entries(size_t nbits, size_t nhashes,
					       double falseprob) {
    return ceil(nbits/-(nhashes/log(1. - exp(log(falseprob)/nhashes))));
}
inline double bloom_estimate_falseprob(size_t nbits, size_t nhashes,
						 size_t nentries) {
    return pow(1. - exp(-(double)nhashes/((double)nbits/nentries)), nhashes);
}
inline size_t bloom_estimate_bits(size_t nentries, double falseprob) {
    return ceil( (nentries*log(falseprob))/log(1./pow(2.,log(2.))) );
}

inline size_t bloom_estimate_hashes(size_t nbits, size_t nentries) {
    return round(((double) nbits / nentries) * log(2.));
}

class bloomfilter {
private:
    bits bits_;
    size_t nhashes_, nentries_;
    static constexpr const char* magic_ = "desres_bloom";
    // default-constructed bloomfilter sets nhashes=1 so it will call
    // a default-constructed bits_ which will null-dereference
    // (whereas 0 would appear to work silently, never set anything and
    // check would always return true)  Assumption is that a default
    // bloomfilter will always be read-into.
    static const auto defhashes_ = 1u;
public:
    // need to load from stream or call init() after default constructor!
    bloomfilter() : nhashes_{defhashes_}, nentries_{0} {}
    // ready for use after this, though can still be overwritten from stream!
    bloomfilter(size_t nvals, double fprob, size_t maxbits = 0) { init(nvals, fprob, maxbits); }
    void init(size_t nvals, double fprob, size_t maxbits = 0) {
        if (nvals == 0 || fprob == 0.)
            throw std::runtime_error("bloomfilter needs non-zero nvals, and desired false probability, not "+
                                     std::to_string(nvals)+","+std::to_string(fprob));
	auto nbits = bloom_estimate_bits(nvals, fprob);
	auto nh = bloom_estimate_hashes(nbits, nvals);
	DIAG(_bloom, "estimated " << nbits << " bits, " << nh << " hashes for "
	     << nvals << " entries with desired falseprob " << fprob);
	if (maxbits && nbits > maxbits)
	    throw std::runtime_error("bloomfilter estimated bits "+
				     std::to_string(nbits)+" > maxbits "+
				     std::to_string(maxbits)+" for nvals="+
				     std::to_string(nvals)+" fprob="+
				     std::to_string(fprob));
        bits_.init(nbits);
        nhashes_ = nh;
        nentries_ = 0; // internal counter
        DIAG(_bloom, "initialized bloom for " << nvals << " values, " <<
             bits_.sizebits() << " sizebits, " << bits_.sizebytes() <<
             " bytes, " << nhashes_ << " hashes");
    }
    // hashes len bytes pointed to by data and sets the resulting bits.
    // returns previous value for the same hash as this data.
    // derive nth new hash from threeroe hash pair h using simple mul-add-mod
    // (LCG-style) double-hashing https://en.wikipedia.org/wiki/Double_hashing
    // shown to get pretty optimal results for bloom filters in
    // https://onlinelibrary.wiley.com/doi/abs/10.1002/rsa.20208
    bool add(const uint8_t *data, std::size_t len) {
        auto h = threeroe(data, len).hashpair64();
        bool ret = true;

        nentries_++;
        for (auto n = 0u; n < nhashes_; n++) {
            auto i = h.first % bits_.sizebits();
	    h.first += h.second;
	    auto b = bits_.get(i);
            ret &= b;
	    DIAG(_bloom>1, "n " << n << " i=" << i << " bit=" << b << " ret=" << ret);
            bits_.set(i);
        }
        return ret;
    }
    bool add(const str_view sv) {
        return add((const uint8_t *)sv.data(), sv.size());
    }
    // returns true of the hash of this data is found in bits
    bool check(const uint8_t *data, std::size_t len) const {
        auto h = threeroe(data, len).hashpair64();

        for (auto n = 0u; n < nhashes_; n++) {
            auto i = h.first % bits_.sizebits();
	    h.first += h.second;
	    auto b = bits_.get(i);
	    DIAG(_bloom>1, "n " << n << " i=" << i << " bit=" << b);
            if (!b) return false;
        }
        return true;
    }
    bool check(const str_view sv) const {
        return check((const uint8_t *)sv.data(), sv.size());
    }
    size_t popcount() const { return bits_.popcount(); }
    const uint8_t* databytes() const { return bits_.databytes(); }
    size_t sizebytes() const { return bits_.sizebytes(); }
    size_t sizebits() const { return bits_.sizebits(); }
    size_t numhashes() const { return nhashes_; }
    size_t numentries() const { return nentries_; }
    // probability of false positive
    double falseprob() const {
        return bloom_estimate_falseprob(bits_.sizebits(), nhashes_, nentries_);
    }
    friend std::ostream& operator<<(std::ostream& out, const bloomfilter& b) {
        out << bloomfilter::magic_ << ' ' << std::dec << b.nhashes_ << ' '
            << b.nentries_ << ' ' << b.bits_;
        return out;
    }
    // IMPORTANT: stream *overwrites* the bloomfilter, it does NOT add into it!=
    // but destroys and replaces the existing bits, and nhashes.
    friend std::istream& operator>>(std::istream& inp, bloomfilter& b) {
        std::string s;
        if (!inp.good()) throw std::runtime_error("error on input stream reading bloom filter data");
        inp >> s >> std::ws;
        DIAG(_bloom, "read magic \"" << s << '"');
        if (!inp.good())
            throw std::runtime_error("error on input stream reading bloom filter magic");
        if (s != bloomfilter::magic_)
            throw std::runtime_error("error on input stream did not get bloom filter magic, got "+s);
        size_t ne, nh;
        inp >> std::dec >> nh >> std::ws >> ne >> std::ws;
        DIAG(_bloom, "read nh=" << nh << " ne=" << ne);
        if (!inp.good())
            throw std::runtime_error("error on input stream reading bloom filter nhashes");
        inp >> b.bits_;
        if (!inp.good())
            throw std::runtime_error("error on input stream reading bloom filter bits");
        b.nhashes_ = nh;
        b.nentries_ = ne;
        return inp;
    }
};

} // namespace core123

