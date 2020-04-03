#pragma once

#include "intutils.hpp"
#include <cinttypes>
#include <array>
#include <string>
#include <iostream>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <utility>

// DOCUMENTATION_BEGIN

// threeroe - A 128-bit non-cryptographic hash function loosely based
// on threefry, and therefore, by inheritance, threefish and skein.
// Basic usage is to construct a threeroe object, possibly call the
// update method to add data to it, and finally obtain a hash digest
// of the data with one of the methods:
//   hashpair64() -> std::pair<uint64_t, uint64_t>
//   hash64()     -> uint64_t
//   digest()     -> std::array<unsigned char, 16>
//   hexdigest()  -> std::string  // length 32
//   bytedigest() -> std::array<std::byte, 16> // only if __cpp_lib_byte is defined
//
// These produce different representations of the same 128 bits.  hash64()
// is equivalent to hashpair64().first.  The
// first 8 bytes of digest() are the bytes of hashpair64().first in
// bigendian order; the last 8 bytes of digest are the bytes of
// hashpair64.second in bigendian order.  The bytes in digest() are
// identical to those in bytedigest() and they correspond to pairs of
// characters in the hexdigest().
//
// threeroe.hpp is header-only code.  Just #include and go.  E.g.,
//
//    #include <core123/threeroe.hpp>
//
//    void *data = ...;
//    size_t len = ...;
//    auto hash = threeroe(data, len).hashpair64();
//
// The constructor may be called with a single argument, as long
// as that argument has data() and size() methods.  E.g.,
//
//    std::vector<T> vdata;
//    threeroe tr1(vdata);
//    std::string stringdata;
//    threeroe tr2(stringdata);
//    std::array<int, 99> adata;
//    threeroe tr3(adata);
//
// If you don't have all (or even any) of the data at construction
// time, more data can be added with the update() method, e.g.,
//
//    threeroe hasher;
//    hasher.update(data, len);
//    hasher.update(moredata, morelen);
//
// Like the constructor, update() has overloads for any type
// that supplies data() and size() methods.
//
// The output functions (hashpair64, digest, hash64, hexdigest,
// bytedigest) are all const, so it's permissible to call them more
// than once or to add more data with update after calling them.
// E.g.,
//
//    auto hp = hasher.hashpair64();  // std::pair<uint64_t, uint64_t>
//    auto d = hasher.digest();       // array<uchar,16> "same" bits as hp
//    uint64_t u1 = hasher.hash64();  // equal to hp.first
//    std::string hd = hasher.hexdigest(); // len=32, "same" bits as d
//    hasher.update(yetmoredata, len); 
//    auto d2 = hasher.digest();   // different from d
// 

// Quality:
//  - threeroe passes all of SMHasher's tests.
//  - Jenkins' froggy, running with HLEN=6, LMMM=15, LARRAY=18,
//    THREADS=32, BYTES=400, BITS=5 finds no collisions through
//    count=2^45 (pairs=2^78).

// Performance: 
//   ut/ut_threeroe.cpp reports that threeroe::update does bulk
//   conversion of 1MB data blocks at more than 16GiB/s, i.e., about
//   0.2 cycles per byte on a 3.50GHz Xeon ES-1650 (Sandy Bridge).
//   Short strings (0-32 bytes) take 10-16ns or about 35 to 50 cycles
//   per hash.

// Portability:
//
//   threeroe.hpp is standard, portable C++14.
//
//   It compiles cleanly with g++ and clang++ -Wall, but it has not
//   undergone serious portability testing.
//
//   threeroe uses no "fancy" instructions.  It requires a good
//   compiler with aggressive inlining to get good performance.  It
//   benefits from an architecture with a "rotate bits" instruction,
//   and with ILP for integer ops like xor and add (i.e., Intel).
//
//   It is not x86-specific, but it *is* untested on any other
//   architecture.
//
//   A static constant uint32_t member threeroe::SMHasher_Verifier is
//   set to the value computed by SMHasher's 'VerificationTest'.  (see
//   ut/ut_threeroe.cpp for details).  Any changes to the algorithm
//   will result in changes to the output of the verifier, so it can
//   be thought of as an "algorthmic fingerprint".
//
// Endian sensitivity:
//
//   The threeroe hash is defined to map two unsigned 64-bit seeds and
//   a sequence of bytes to two 64-bit unsigned integers, i.e., the
//   value returned by digest<uint64_t>.  Two correct implementations
//   of threeroe will produce the same numerical 64-bit values when
//   given the same inputs, regardless of the endianness of the
//   hardware they run on.
//
//   In addition, threeroe defines a 'digest<unsigned char>' form and
//   a 'hexdigest' form that are also endian-agnostic.  The
//   digest<unsigned char> form consists of the 8 bytes of
//   digest<uint64_t>()[0], followed by the 8 bytes of
//   digest<uint64_t>[1] *both in big-endian order*.  The result of
//   digest<unsigned char> is always the same, regardless of the
//   hardware endianness, but a high-performance implementation of
//   digest will probably use instructions or intrinsics that are
//   aware of the hardware's endianness to convert from
//   hardware-native unsigned 64-bit integers to bytes.
//
//   The hexdigest form is digest<unsigned char> form mapped to an
//   ascii string.  The 2*j'th and 2*j+1'st characters of the
//   hexdigest are the hex digits ('0'-'9', 'a'-'f') corresponding to
//   the j'th byte of the digest<unsigned char> form.  The result of
//   hexdigest is always the same, regardless of hardware endianness.
//
//   Finally, note that passing the address of anything other than a
//   char or byte buffer to threeroe will make the result dependent on
//   the endianness of the data stored at that address.  E.g., given
//
//     std::vector<int>  vi{3, 1, 4, 1, 5};
//     auto h = threeroe(vi).digest();
//
//   h will depend on the endian-ness of the hardware's int.  But
//   given:
//
//     std::vector<char> vc{'\3', '\1', '\4', '\1', '\5'};
//     auto h2 = threeroe(vc).diges();
//
//   h2 will be different from h, but it will not depend on the
//   endian-ness of the hardware.
//
//     John Salmon  Jun 6, 2014
//       modified   Dec, 2018,
//       modified   Sep, 2019
// DOCUMENTATION_END

namespace core123{
class threeroe{
public:
    struct result_type;
private:
    // Everything "unique" to threeroe is in the private methods:
    // mixoneblock() and finish() and the constants.  Everything else
    // is just boilerplate to support the API.

    // Rotation constants from threefry.h.  See
    // http://deshawresearch.com/resources_random123.html and also
    // skein.h from
    // http://csrc.nist.gov/groups/ST/hash/sha-3/Round3/documents/Skein_FinalRnd.zip
    enum  {
        /* These are the R_256 constants from the Threefish reference sources
           with names changed to R_64x4... */
        R_64x4_0_0=14, R_64x4_0_1=16,
        R_64x4_1_0=52, R_64x4_1_1=57,
        R_64x4_2_0=23, R_64x4_2_1=40,
        R_64x4_3_0= 5, R_64x4_3_1=37,
        R_64x4_4_0=25, R_64x4_4_1=33,
        R_64x4_5_0=46, R_64x4_5_1=12,
        R_64x4_6_0=58, R_64x4_6_1=22,
        R_64x4_7_0=32, R_64x4_7_1=32
    };

    enum  {
        /*
        // Output from skein_rot_search: (srs64_B64-X1000)
        // Random seed = 1. BlockSize = 128 bits. sampleCnt =  1024. rounds =  8, minHW_or=57
        // Start: Tue Mar  1 10:07:48 2011
        // rMin = 0.136. #0325[*15] [CRC=455A682F. hw_OR=64. cnt=16384. blkSize= 128].format   
        */
        R_64x2_0_0=16,
        R_64x2_1_0=42,
        R_64x2_2_0=12,
        R_64x2_3_0=31,
        R_64x2_4_0=16,
        R_64x2_5_0=32,
        R_64x2_6_0=24,
        R_64x2_7_0=21
        /* 4 rounds: minHW =  4  [  4  4  4  4 ]
        // 5 rounds: minHW =  8  [  8  8  8  8 ]
        // 6 rounds: minHW = 16  [ 16 16 16 16 ]
        // 7 rounds: minHW = 32  [ 32 32 32 32 ]
        // 8 rounds: minHW = 64  [ 64 64 64 64 ]
        // 9 rounds: minHW = 64  [ 64 64 64 64 ]
        //10 rounds: minHW = 64  [ 64 64 64 64 ]
        //11 rounds: minHW = 64  [ 64 64 64 64 ] */
    };

    void mixoneblock(const char *ks){
        mixoneblock(ks, state);
    }

    // mixoneblock - inject one block (4x64-bits) of data
    //   into the state, s and do some mixing:
    //
    //   Inject the data into the state with +=.
    //   Do the first round of ThreeFry4x64
    //   reinject, rotated with += again
    //   Mix s[0] into s[3] and s[2] into s[1] with xor
    static void mixoneblock(const char *data, uint64_t s[4]){
        static const size_t s64 = sizeof(uint64_t);
        uint64_t k0; memcpy(&k0, data+0*s64, s64); k0 = letonative(k0);
        uint64_t k1; memcpy(&k1, data+1*s64, s64); k1 = letonative(k1);
        uint64_t k2; memcpy(&k2, data+2*s64, s64); k2 = letonative(k2);
        uint64_t k3; memcpy(&k3, data+3*s64, s64); k3 = letonative(k3);

        uint64_t k4=rotl(k3,R_64x4_2_0), k5=rotl(k2,R_64x4_1_0); 
        uint64_t k6=rotl(k1,R_64x4_2_1), k7=rotl(k0,R_64x4_1_1); 

        s[0] += k0; s[1] += k1; s[2] += k2; s[3] += k3; 

        s[0] += s[1]; s[1] = rotl(s[1],R_64x4_0_0); s[1] ^= s[0]; 
        s[2] += s[3]; s[3] = rotl(s[3],R_64x4_0_1); s[3] ^= s[2]; 

        s[0] += k4; s[1] += k5; s[2] += k6; s[3] += k7;
        s[3] ^= s[0];
        s[1] ^= s[2];
    }

    // finish - this is just barely enough to pass the
    // SMHasher tests for collisions of short strings.
    // It's a hybrid of rounds of Threefry2x64 and
    // Threefry4x64, which seems to do *just* enough
    // mixing.  It may be wise to pay a slight penalty
    // in short-string performance to get a little
    // more mixing here...
    std::pair<uint64_t, uint64_t>
    finish() const{
        // finish is const, so make a copy of this->state
        uint64_t s[] = {state[0], state[1], state[2], state[3]};
        // mix any deferred bytes into s.
        if(bytes_deferred){
            char zeros[CHARS_PER_INBLK] = {};
            memcpy(zeros, deferred, bytes_deferred);
            mixoneblock(zeros, s);
        }

        uint64_t s0 = s[0]+s[2];
        uint64_t s1 = s[1]+s[3];
        uint64_t s2 = s[2];
        uint64_t s3 = s[3]+len;

        s0 += s1; s1 = rotl(s1,R_64x4_0_0); s1 ^= s0; 
        s2 += s3; s3 = rotl(s3,R_64x4_0_1); s3 ^= s2; 

        s0 += s1; s1 = rotl(s1,R_64x2_0_0); s1 ^= s0;
        s0 += s1; s1 = rotl(s1,R_64x2_1_0); s1 ^= s0;

        s0 += s3; s1 += s2;

        s0 += s1; s1 = rotl(s1,R_64x2_2_0); s1 ^= s0;
        s0 += s1; s1 = rotl(s1,R_64x2_3_0); s1 ^= s0;

        s0 += s2; s1 += s3;

        s0 += s1; s1 = rotl(s1,R_64x2_0_0); s1 ^= s0;
        s0 += s1; s1 = rotl(s1,R_64x2_1_0); s1 ^= s0;

        s0 += s3; s1 += s2;

        s0 += s1; s1 = rotl(s1,R_64x2_2_0); s1 ^= s0;
        s0 += s1; s1 = rotl(s1,R_64x2_3_0); s1 ^= s0;

        return {s0, s1};
    }

public:
    // threeroe - the constructor takes two optional seed arguments.
    threeroe(uint64_t seed1=0, uint64_t seed2 = 0){
        state[0] = seed1;
        state[1] = 0x3243F6A8885A308Dull; // pi<<60
        state[2] = seed2;
        state[3] = 0;
        bytes_deferred = 0;
        len = 0;
    }

    // Taking inspiration from python's hashlib, the constructor
    // also takes initial data.
    threeroe(const void *data, size_t n, uint64_t seed1=0, uint64_t seed2=0) : threeroe(seed1, seed2){
        update(data, n);
    }

    // Templated constructor and and Init methods that also take
    // an argument of any type, V, with data() and size() members.
    //
    template<typename V, typename _dummy=decltype(std::declval<V>().data())>
    threeroe(const V& v, uint64_t seed1=0, uint64_t seed2 = 0) : threeroe(seed1, seed2){
        update<V>(v);
    }

    // update - add N bytes of data to the state.
    //  Return a reference to this, to
    //  facilitate chaining, e.g., h.update('he').update('llo')
    threeroe& update(const void *data, size_t n){
        const char *cdata = (const char *)data;
        const char *end = cdata+n;
        cdata = catchup(cdata, end);
        if( end-cdata >= ptrdiff_t(sizeof(deferred)) )
            cdata = bulk(cdata, end);
        defer(cdata, end);
        len += n;
        return *this;
    }

    // Too-clever-by-half? - A templated update method that works for
    // any type, V, with data() and size() members, e.g., std::vector,
    // std::string, std::array.
    template<typename V>
    threeroe& update(const V& v){
        return update(reinterpret_cast<const void*>(v.data()), v.size()*sizeof(*v.data()));
    }    

    // Output functions and types

    typedef std::pair<uint64_t, uint64_t> hashpair64_type;
    hashpair64_type
    hashpair64() const {
        return finish();
    }

    typedef uint64_t hash64_type;
    hash64_type
    hash64() const {
        return finish().first;
    }

    typedef std::array<unsigned char, 16> digest_type;
    digest_type
    digest() const{
        union{
            uint64_t u64[2];
            digest_type d;
        } u;
        static_assert(sizeof(u.u64) == sizeof(u.d), "Uh oh.  This is *very* surprising");
        auto f = finish();
        u.u64[0] = nativetobe(f.first);
        u.u64[1] = nativetobe(f.second);
        return u.d;
    }

#if __cpp_lib_byte >= 201603 // std::byte is a C++17-ism
    typedef std::array<std::byte, 16> bytedigest_type;
    bytedigest_type
    bytedigest() const{
        union{
            digest_type d;
            bytedigest_type bd;
        } u{digest()};
        static_assert(sizeof(u.bd) == sizeof(u.d), "Uh oh.  This is *very* surprising");
        return u.bd;
    }
#endif

    // hexdigest - Bytes 2*i and 2*i+1 in the output of hexdigest()
    //   are the hex digits corresponding to byte i in the output of
    //   digest().
    std::string
    hexdigest() const{
        // premature optimization?  Using ostream takes about
        // 500ns on a 2018 x86 with gcc8.
        // sprintf("%016llx%016llx", ...)  takes about 150ns.
        // This takes about 37ns, doing it one nibble-at-a-time,
        // starting at the right.
        std::string ret(32, '\0');
        char *p = &ret[32];
        auto f = finish();
        for(auto v64 : {f.second, f.first}){
            for(int i=0; i<16; ++i){
                *--p = hexlownibble(v64);
                v64>>=4;
            }
        }
        return ret;
    }
        
    // In threeroe/0.08 we changed the way SMHasher calls threeroe: it
    // calls the new, endian-independent digest() method, rather than
    // doing endian-dependent type-punning of the values returned by
    // Final().  Since digest() is bigendian, but our hardware is
    // little-endian, this changed the value of the
    // 'SMHasher_Verifier' even though we didn't change any
    // "observable output" from threeroe::Final.
    //
    // FWIW, the old verifier value was 0x2527b9f0;
    static const uint32_t SMHasher_Verifier = 0x6CE2839D;

private:
    static const size_t WORDS_PER_INBLK = 4; // in uint64_t
    static const size_t CHARS_PER_INBLK = WORDS_PER_INBLK * 8;
    uint64_t state[4];
    uint64_t deferred[WORDS_PER_INBLK];
    unsigned int bytes_deferred;
    uint64_t len;

    // catchup - use the bytes in range(b,e) to catch up with any bytes
    //  that had previously been deferred.  If there is no pending
    //  deferral, do nothing.  If there are enough bytes in the range
    //  to fill the deferral block, then copy that many, and then
    //  consume the now-full deferral block.  Otherwise, copy all
    //  the bytes from the range to the deferral block.
    const char *catchup(const char *b, const char *e){
        if( bytes_deferred ){
            if( e-b >= ptrdiff_t(sizeof(deferred)-bytes_deferred) ){
                size_t ncpy = sizeof(deferred)-bytes_deferred;
                memcpy((char *)deferred + bytes_deferred, b, ncpy);
                mixoneblock((const char *)deferred);
                bytes_deferred = 0;
                return b+ncpy;
            }else{
                defer(b, e);
                return e;
            }
        }
        return b;
    }
                   
    // defer - Copy the range(b, e) to the deferral block.
    //   e-b must fit in the space available, equal to
    //   sizeof(deferred)-bytes_deferred.
    void defer(const char *b, const char *e){
        assert( (e-b) < ptrdiff_t(sizeof(deferred)-bytes_deferred) );
        memcpy((char *)deferred + bytes_deferred, b, e-b);
        bytes_deferred += (e-b);
    }

    // bulk - Do a bulk injection of an initial prefix of the data in
    //  range(b, e).  Return a pointer to the first byte that *was
    //  not* injected.  It is guaranteed that the unconsumed data is
    //  smaller than CHARS_PER_INBLK chars.
    char *bulk(const char *b, const char *e){
        const int UNROLL=8;
        while( b <= e-UNROLL*CHARS_PER_INBLK ){
            if(UNROLL>=1) mixoneblock(b+0*CHARS_PER_INBLK);
            if(UNROLL>=2) mixoneblock(b+1*CHARS_PER_INBLK);
            if(UNROLL>=3) mixoneblock(b+2*CHARS_PER_INBLK);
            if(UNROLL>=4) mixoneblock(b+3*CHARS_PER_INBLK);
            if(UNROLL>=5) mixoneblock(b+4*CHARS_PER_INBLK);
            if(UNROLL>=6) mixoneblock(b+5*CHARS_PER_INBLK);
            if(UNROLL>=7) mixoneblock(b+6*CHARS_PER_INBLK);
            if(UNROLL>=8) mixoneblock(b+7*CHARS_PER_INBLK);
            b += UNROLL*CHARS_PER_INBLK;
        }
        while( b <= e-CHARS_PER_INBLK){
            mixoneblock(b);
            b += CHARS_PER_INBLK;
        }
        return (char *)b;
    }
};

} // namespace core123
