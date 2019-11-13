// efficient bitvector implementation, with ability to dump to or load
// from a stream, needed for BloomFilter class
// XXX The format that is dumped to or loaded from streams
// is dependent on the host endiannness, cannot be loaded
// into a machine of a different endianness.  In principle,
// the magic string at the start of the bit vector could be
// used to byteswap for conversion, but we only currently need/use
// this on x86_64, so left as a futureproofing TBD...
// Mark Moraes, D. E. Shaw Research
#pragma once

#include <vector>
#include <tuple>
#include <iostream>
#include <core123/netstring.hpp>
#include <core123/threeroe.hpp>
#include <core123/strutils.hpp>

namespace core123 {

class bits {
private:
    typedef uint64_t WORD;
    size_t szbits_, szwords_;
    std::unique_ptr<WORD[]> b_;
    // magic constant for output serialization, used to check size
    // and endianness for deserialization, so ensure it is different
    // with different endiannness.
    // The chars in the magic constant (hi to lo) correspond to
    // ASCII "zoYN8-Eg" which will become the netstring
    // 8:gE-8NYoz, from a x86_64 (little-endian) machine.
    static constexpr WORD magic_() { return 0x7a6f594e382d4567; } 
    static constexpr const size_t WORDBYTES = sizeof(WORD);
    static constexpr const size_t WORDBITS = WORDBYTES*8u;
    // returns array index, mask and current value
    inline std::tuple<WORD,WORD,WORD> getidx_(size_t i) const {
        auto k = std::ldiv(i, WORDBITS);
        WORD mask = ((WORD)1 << k.rem);
        return std::make_tuple(k.quot, mask, b_[k.quot] & mask);
    }
    static inline auto bits2words_(size_t nbits) {
	return (nbits + WORDBITS - 1)/ WORDBITS; // roundup!
    }
    // just a common idiom for construction and assignment
    void init_(size_t nbits, size_t nwords, std::unique_ptr<WORD[]> wup) {
	szbits_ = nbits;
	szwords_ = nwords;
	b_ = wup ? std::move(wup) : std::make_unique<WORD[]>(szwords_); // make_unique is value-initialized
    }
    // XXX default copy will not work (would need deep copy of unique_ptr)
    bits(const bits& b) = delete;
    auto operator=(const bits& b) = delete;
public:
    typedef WORD value_type;
    typedef size_t size_type;
    
    // create a new, cleared bitvector of the specified size. nbits = 0 means
    // unusable, will need to be init(), >>, or copied into.
    bits(size_t nbits = 0) { init(nbits); }
    // move constructor/assignment is useful for bits, no real use case for copy
    bits(bits&&) = default;
    bits& operator=(bits&&) = default;
    void init(size_t nbits) {
	init_(nbits, bits2words_(nbits), {});
    }
    // like vector<bool>, this is not really a container type,
    // so we avoid most of the container-like method names,
    // so that one does not accidentally use container idioms.
    // e.g. size() or data() would be confusing, does one
    // mean bits or bytes or words?
    size_type sizebits() const { return szbits_; }
    size_type sizebytes() const { return szwords_*WORDBYTES; }
    const uint8_t* databytes() const { return (const uint8_t*) b_.get(); }

    void clear() {
	::memset(b_.get(), 0, sizebytes());
    }
    // set, unset, get return true if old value was set, false if it was unset
    // all are unchecked for bounds, so undefined behaviour if i is too large
    bool set(size_t i) {
        auto t = getidx_(i);
        b_[std::get<0>(t)] |= std::get<1>(t);
        return std::get<2>(t) != 0;
    }
    bool unset(size_t i) {
        auto t = getidx_(i);
        b_[std::get<0>(t)] &= ~std::get<1>(t);
        return std::get<2>(t) != 0;
    }
    bool get(size_t i) const {
        auto t = getidx_(i);
        return std::get<2>(t) != 0;
    }
    // convenient for debugging/diagnostics
    size_t popcount() const {
	size_t n = 0;
	for (size_t i = 0u; i < szwords_; i++)
#ifdef __GNUC__
	    n += __builtin_popcountl(b_[i]);
#else
#error "No code to do popcount outside __GNUC__, contact maintainers"
#endif
	return n;
    }
    friend std::ostream& operator<<(std::ostream& out, const bits& b) {
        out << std::to_string(b.sizebits()) << ' '; // ensure written as decimal!
        auto m = magic_();
        sput_netstring(out, {(char *)&m, sizeof(m)});
        str_view sv((const char *)b.databytes(), b.sizebytes());
        sput_netstring(out, sv);
        sput_netstring(out, threeroe(sv).hexdigest());
        return out;
    }

    friend std::istream& operator>>(std::istream& inp, bits& b) {
        size_t szbytes, szbits;
        if (!inp.good()) throw std::runtime_error("Bits istream error before reading Bits sizebits");
        inp >> szbits >> std::ws;
        if (!inp.good()) throw std::runtime_error("Bits istream error after reading sizebits "+std::to_string(szbits));
        std::string magic;
        if (!sget_netstring(inp, &magic) || magic.size() != sizeof(WORD))
            throw std::runtime_error("Bits istream error while reading magic after sizebits "+std::to_string(szbits)+" magicsize="+std::to_string(magic.size()));
        WORD m, refm = magic_();
        memcpy((void *)&m, magic.data(), sizeof(m));
        if (m != refm)
            throw std::runtime_error("Bits istream error, bad magic, read "+tohex(m)+", expected "+tohex(refm));
        // We allocate and read our into b_ here rather than init_() to
	// avoid extra copies since we intend this for very large
	// (multi-gigabit to terabit) bitmaps in bloom filters.
        inp >> szbytes;
        if (!inp.good()) throw std::runtime_error("Bits istream error after reading Bits length "+std::to_string(szbytes));
        char c;
        inp >> c;
        if (!inp.good()) throw std::runtime_error("Bits istream error after reading length delim after "+std::to_string(szbytes));
        if (c != ':') throw std::runtime_error("Bits istream error, did not get expected colon after length "+std::to_string(szbytes)+" got "+std::to_string(c));
	auto szw = bits2words_(szbits);
	auto expbytes = szw*WORDBYTES;
	if (expbytes != szbytes)
	    throw std::runtime_error("Bit sistream error, szbytes "+std::to_string(szbytes)+" != expbytes "+std::to_string(expbytes));
	std::unique_ptr<WORD[]> wup{new WORD[szw]}; // uninitialized!
        inp.read((char *)wup.get(), szbytes);
        if (!inp.good()) throw std::runtime_error("Bits istream error while reading data, length "+std::to_string(szbytes));
        inp >> c;
        if (!inp.good()) throw std::runtime_error("Bits istream error after reading end delim after data, length "+std::to_string(szbytes));
        if (c != ',') throw std::runtime_error("Bits istream error, did not get expected comma after data, length "+std::to_string(szbytes)+" got "+std::to_string(c));
        std::string h;
        if (!sget_netstring(inp, &h))
            throw std::runtime_error("Bits istream error while reading data hash, length "+std::to_string(szbytes));
        auto ch = threeroe(wup.get(), szbytes).hexdigest();
        if (h != ch) throw std::runtime_error("Bits istream error, file has trsum "+h+", calc on netstring says "+ch);
	b.init_(szbits, szw, std::move(wup));
        return inp;
    }
};

} // namespace core123
