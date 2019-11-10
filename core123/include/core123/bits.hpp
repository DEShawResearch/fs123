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
    WORD *b_;
    size_t sz_, szbits_;
    // magic constant for output serialization, used to check size
    // and endianness for deserialization, so ensure it is different
    // with different endiannness.
    // The chars in the magic constant (hi to lo) correspond to
    // ASCII "zoYN8-Eg" which will become the netstring
    // 8:gE-8NYoz, from a x86_64 (little-endian) machine.
    static constexpr WORD magic_() { return 0x7a6f594e382d4567; } 
    // returns array index, mask and current value
    inline std::tuple<WORD,WORD,WORD> getidx_(size_t i) const {
        auto k = std::ldiv(i, WORDBITS);
        WORD mask = ((WORD)1 << k.rem);
        return std::make_tuple(k.quot, mask, b_[k.quot] & mask);
    }
public:
    typedef WORD value_type;
    typedef size_t size_type;
    static constexpr const size_t WORDBYTES = sizeof(WORD);
    static constexpr const size_t WORDBITS = WORDBYTES*8u;
    
    // default constructor creates an unsized bitmap, will
    // need to call init() or load it from a stream before use.
    bits() : b_{nullptr}, sz_{0}, szbits_{0} {}
    // create a new, cleared bitvector of the specified size

    bits(size_t nbits) : bits() {
        init(nbits);
    }
    ~bits() { destroy(); }

    // wipe out the bitvector, return to same state as
    // a default-constructed one
    void destroy() {
        if (b_) {
            delete [] b_;
            b_ = nullptr;
            sz_ = szbits_ = 0u;
        }
    }

    // slightly tricky: either allocates a new empty bitvector
    // (deletes any previous one) or swaps in the data from
    // a caller-provided buffer (used when reading from a stream)
    void init(size_t nbits, size_t newdatasizebytes = 0,
              std::unique_ptr<char[]>* newdatap = nullptr) {
        auto nsz = (nbits + WORDBITS - 1)/ WORDBITS; // roundup!
        if (newdatasizebytes && nsz*WORDBYTES != newdatasizebytes)
            throw std::runtime_error("incorrect size passed to create, expected "
                                     +std::to_string(nsz*WORDBYTES)+" got "
                                     +std::to_string(newdatasizebytes)+" bits="
                                     +std::to_string(nbits));
        destroy();
        szbits_ = nbits;
        sz_ = nsz;
        if (newdatap) {
            b_ = (WORD *)newdatap->release();
        } else {
            b_ = new WORD[sz_];
            clear();
        }
    }

    void clear() {
        if (b_)
            for (auto p = b_; p < b_ + sz_; p++)
                *p = 0;
    }
    // like vector<bool>, this is not really a container type,
    // so we avoid most of the container-like method names,
    // so that one does not accidentally use container idioms.
    // e.g. size() or data() would be confusing, does one
    // mean bits or bytes or words?
    size_type sizebits() const { return szbits_; }
    size_type sizebytes() const { return sz_*WORDBYTES; }
    const uint8_t* databytes() const { return (const uint8_t*) b_; }

    // set, unset, get return true if old value was set, false if it was unset
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
	for (size_t i = 0u; i < sz_; i++)
	    n +=__builtin_popcountl(b_[i]);
	return n;
    }
    std::ostream& sput(std::ostream& os) {
        os << sz_ << " words, " << szbits_ << " bits";
        for (size_t i = 0u; i < sz_; i++) {
            if ((i % 4) == 0) {
                os << fmt("\n%010zx:", i);
            }
            os << ' ' << tohex(b_[i]);
        }
	return os;
    }

    friend std::ostream& operator<<(std::ostream& out, const bits& b) {
        out << std::dec << b.sizebits() << ' ';
        auto m = magic_();
        sput_netstring(out, {(char *)&m, sizeof(m)});
        str_view sv((const char *)b.databytes(), b.sizebytes());
        sput_netstring(out, sv);
        sput_netstring(out, threeroe(sv).hexdigest());
        return out;
    }

    friend std::istream& operator>>(std::istream& inp, bits& b) {
        size_t sz, szbits;
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
        // XXX It would be nice to have an sget_netstring that returns a
        // unique_ptr<char[]> and size instead of a string, or a buffer,size pair?
        // We read this ourselves because we want to hand the buffer we read to
        // init() rather than allocate and copy a new one, since we intend this
        // for very large (multi-gigabit to terabit) bitmaps in bloom filters.
        inp >> sz;
        if (!inp.good()) throw std::runtime_error("Bits istream error after reading Bits length "+std::to_string(sz));
        char c;
        inp >> c;
        if (!inp.good()) throw std::runtime_error("Bits istream error after reading length delim after "+std::to_string(sz));
        if (c != ':') throw std::runtime_error("Bits istream error, did not get expected colon after length "+std::to_string(sz)+" got "+std::to_string(c));
        std::unique_ptr<char[]> nb(new char[sz]);
        inp.read((char *)&nb[0], sz);
        if (!inp.good()) throw std::runtime_error("Bits istream error while reading data, length "+std::to_string(sz));
        inp >> c;
        if (!inp.good()) throw std::runtime_error("Bits istream error after reading end delim after data, length "+std::to_string(sz));
        if (c != ',') throw std::runtime_error("Bits istream error, did not get expected comma after data, length "+std::to_string(sz)+" got "+std::to_string(c));
        std::string h;
        if (!sget_netstring(inp, &h))
            throw std::runtime_error("Bits istream error while reading data hash, length "+std::to_string(sz));
        auto ch = threeroe(nb.get(), sz).hexdigest();
        if (h != ch) throw std::runtime_error("Bits istream error, file has trsum "+h+", calc on netstring says "+ch);
        b.init(szbits, sz, &nb);
        return inp;
    }
};

} // namespace core123
