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
// on threefy, and therefore, by inheritance, threefish and skein.
// The public API was initially inspired by Jenkins' Spooky hash, but
// it has evolved significantly.  Basic usage is to construct a
// threeroe object, possibly call Update to add data to it, and call
// Final() to obtain the hash (as a pair uint64_t).  threeroe.hpp is
// header-only code.  Just #include and go.  E.g.,
//
//    #include <core123/threeroe.hpp>
//
//    void *data = ...;
//    size_t len = ...;
//    auto hash = threeroe(data, len).Final();
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
// time, more data can be added with the Update() method, e.g.,
//
//    threeroe hasher;
//    hasher.Update(data, len);
//    hasher.Update(moredata, morelen);
//
// Like the constructor, Update() has overloads for any type
// that supplies data() and size() methods.
//
// The hash itself is obtained by calling Final().  Final() is const,
// so it's permissible to call it more than once or to add more data
// with Update after calling Final.  E.g.,
//
//    auto h2 = hasher.Final();     // same as hash, above
//    hasher.Update(yetmoredata, len); 
//    auto h3 = hasher.Final();   // different from h2
// 
// Final() returns a threeroe::result_type
//
// The result_type is publicly derived from std::pair<uint64_t, uint64_t>.
// Callers can simply use the first and second members:
//
//    uint64_t a = h3.first;
//    uint64_t b = h3.second.
//
// which are independent "random" (not cryptographic!) hashes of the
// inputs.
//
// The result_type::digest() methods format *this as 16
// endian-independent bytes:
//
//    unsigned char hash[16];
//    h3.digest(hash);
// or
//    std::array<16, unsigned char> ahash = h3.digest();
//
// The result_type::hexdigest() methods format *this as an
// endian-independent string:
//
//    char buf[33];
//    h3.hexdigest(buf);
// or
//    std::string hexdig = h3.hexdigest();
//
// The ostream insertion operator sends the output of hexdigest to the
// stream:
//
//    oss << h3;
//
// so the result_type also "works" with core123::str:
//
// Calling Init on an existing threeroe object leaves the object as if
// it had been newly constructed.  Init() has the same overloads as
// the constructor.
//

// Quality:
//  - threeroe passes all of SMHasher's tests.
//  - Jenkins' froggy, running with HLEN=6, LMMM=15, LARRAY=18,
//    THREADS=32, BYTES=400, BITS=5 finds no collisions through
//    count=2^45 (pairs=2^78).

// Performance: 
//   ut/ut_threeroe.cpp reports that threeroe::Update does bulk
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
//   architecture.  In order to avoid surprises, there are #ifdefs
//   that prevent it from building on anything other than x86_64.  It
//   wouldn't be hard to make it endian-agnostic, but I don't have the
//   need, nor do I have the machines to test on.  So that effort is
//   deferred till another day.
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
//   a sequence of bytes to two 64-bit unsigned integers.  Two correct
//   implementations of threeroe will produce the same numerical
//   64-bit values when given the same inputs, regardless of the
//   endianness of the hardware they run on.
//
//   In addition, threeroe defines a 'digest' form and a 'hexdigest'
//   form that are also endian-agnostic.  The digest form consists of
//   the 8 bytes of result.first, followed by the 8 bytes of
//   result.second, *both in big-endian order*.  The result of digest
//   is always the same, regardless of the hardware endianness, but a
//   high-performance implementation of digest will probably use
//   instructions or intrinsics that are aware of the hardware's
//   endianness to convert from hardware-native unsigned 64-bit
//   integers to bytes.
//
//   The hexdigest form is digest form mapped to an ascii string.  The
//   2*j'th and 2*j+1'st characters of the hexdigest are the hex
//   digits ('0'-'9', 'a'-'f') corresponding to the j'th byte of the
//   digest form.  The result of hexdigest is always the same,
//   regardless of hardware endianness.
//
//   Finally, note that passing the address of anything other than a
//   char buffer to threeroe will make the result dependent on the
//   endianness of the data stored at that address.  E.g., given
//
//     std::vector<int>  vi{3, 1, 4, 1, 5};
//     auto h = threeroe(vi).Final();
//
//   h will depend on the endian-ness of the hardware's int.  But
//   given:
//
//     std::vector<char> vc{'\3', '\1', '\4', '\1', '\5'};
//     auto h2 = threeroe(vc).Final();
//
//   h2 will be different from h, but it will not depend on the
//   endian-ness of the hardware.
//
//     John Salmon  Jun 6, 2014
//       modified   Dec, 2018
// DOCUMENTATION_END

namespace core123{
class threeroe{
public:
    struct result_type;
private:
    // Everything "unique" to threeroe is in the private methods:
    // mixoneblock() and finish() and the constants.  Everything else
    // is just boilerplate to support the API.

    // The public API, which follows Jenkins' Spooky is below.

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
    static result_type
    finish(uint64_t len, const uint64_t s[4]){
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
    // The public API was inspired by Jenkins' Spooky... with several
    // "improvements".

    // The methods from here on could easily be abstracted into a
    // common framework for a wide class of hash functions.

    // result_type - (almost) a pair of uint64_t, but with hexdigest
    //  members that format 32 hex digits ('0'-'9','a'-'f'), digest
    //  methods that return or write 16 bytes, and an ostream
    //  insertion operator<<() that uses hexdigest.
    struct result_type : public std::pair<uint64_t, uint64_t>{
        result_type(uint64_t f, uint64_t s) : std::pair<uint64_t, uint64_t>(f, s){}

        // digest_size - how big is the std::array returned by digest.  (16)
        static const size_t digest_size = 16;

        // digest - there are two overloads of the digest methods.
        //   When called with no arguments, it returns a
        //   std::array<unsigned char, digest_size>.  When called as
        //   digest(unsigned char* p, size_t sz), it writes
        //   std::min(sz, digest_size) bytes to p and returns the number of
        //   bytes written.
        //
        //   The first 8 bytes are the bigendian  bytes of first.
        //   The last 8 bytes are the bigendian bytes of second.
        //   The choice to use 'bigendian' makes digest() consistent
        //   with the hexdigest(), and the insertion operator<<(),
        //   which are, in turn, consistent with earlier versions of
        //   trsum.
        //
        //   FIXME - when we fully commit to C++17, digest should work
        //   with std::byte rather than unsigned char.

        size_t
        digest(unsigned char *p, size_t sz) const{
            auto a16 = digest();
            if(sz>digest_size)
                sz = digest_size;
            ::memcpy(p, a16.data(), sz);
            return sz;
        }
        
        std::array<unsigned char, digest_size>
        digest() const{
            union{
                uint64_t u64[2];
                std::array<unsigned char, digest_size> ac16;
            } u;
            static_assert(sizeof(u.u64) == sizeof(u.ac16), "Uh oh.  This is *very* surprising");
            u.u64[0] = nativetobe(first);
            u.u64[1] = nativetobe(second);
            return u.ac16;
        }        

        // hexdigest - there are two overloads of hexdigest.  When
        //   called with no arguments it returns a std::string of
        //   length 32.  When called as hexdigest(char* dest, size_t sz)
        //   it writes std::min(sz, 33) bytes to dest.  The
        //   destination is NUL-terminated only if sz>=33.  The number
        //   of bytes written is returned.
        //
        //   Bytes 2*i and 2*i+1 in the output of hexdigest() are the hex
        //   digits corresponding to byte i in the output of digest().
        size_t
        hexdigest(char *buf, size_t sz) const{
            // premature optimization?  Using ostream takes about
            // 500ns on a 2018 x86 with gcc8.
            // sprintf("%016llx%016llx", ...)  takes about 150ns.
            // This takes about 37ns, doing it one nibble-at-a-time,
            // starting at the right.
            char *p;
            unsigned i;
            if(sz>32){
                buf[32] = '\0';
                i = 32;
                sz = 33;
            }else{
                i = sz;
            }
            p = buf+i;
            if(i>16){ // avoid right-shift by negative!
                uint64_t v64 = second >> ((32-i)*4);
                for(; i>16; --i){
                    unsigned nibble = v64&0xf;
                    v64>>=4;
                    *--p = ((nibble>9)? ('a'-10) : '0') + nibble;
                }
            }
            uint64_t v64 = first >> ((16-i)*4);
            for(; i>0; --i){
                unsigned nibble = v64&0xf;
                v64>>=4;
                *--p = ((nibble>9)? ('a'-10) : '0') + nibble;
            }
            return sz;
        }
        
        std::string
        hexdigest() const{
            char buf[32];
            hexdigest(buf, 32);
            return {buf, 32};
        }

        friend std::ostream& operator<<(std::ostream& os, const result_type& v){
            char hd[32];
            v.hexdigest(hd, 32);
            return os.write(hd, 32);
        }
    };

    // threeroe - the constructor takes two optional seed arguments.
    threeroe(uint64_t seed1=0, uint64_t seed2 = 0){
        Init(seed1, seed2);
    }

    // Taking inspiration from python's hashlib, the constructor
    // also takes initial data.
    threeroe(const void *data, size_t n, uint64_t seed1=0, uint64_t seed2=0){
        Init(data, n, seed1, seed2);
    }

    // Init() is only needed to *re*-initialize a threeroe.
    // A newly constructed threeroe has already been Init'ed.
    // (Unlike Jenkins, Init returns *this, and there's an
    // overload that takes initial data. )
    threeroe& Init(uint64_t seed1 = 0, uint64_t seed2 = 0){
        state[0] = seed1;
        state[1] = 0x3243F6A8885A308Dull; // pi<<60
        state[2] = seed2;
        state[3] = 0;
        bytes_deferred = 0;
        len = 0;
        return *this;
    }

    // An overload of Init that also Update's the object with some initial data.
    threeroe& Init(const void *data, size_t n, uint64_t seed1=0, uint64_t seed2=0){
        Init(seed1, seed2);
        return Update(data, n);
    }

    // Update - add N bytes of data to the state.
    //  (unlike Jenkins, we return a reference to this, to
    //  facilitate chaining, e.g., h.Update('he').Update('llo')
    threeroe& Update(const void *data, size_t n){
        const char *cdata = (const char *)data;
        const char *end = cdata+n;
        cdata = catchup(cdata, end);
        if( end-cdata >= ptrdiff_t(sizeof(deferred)) )
            cdata = bulk(cdata, end);
        defer(cdata, end);
        len += n;
        return *this;
    }

    // Too-clever-by-half? - A templated Update method that works for
    // any type, V, with data() and size() members, e.g., std::vector,
    // std::string, std::array.
    template<typename V>
    threeroe& Update(const V& v){
        return Update((void*)v.data(), v.size()*sizeof(*v.data()));
    }    


    // Templated constructor and and Init methods that also take
    // an argument of any type, V, with data() and size() members.
    //
    template<typename V, typename _dummy=decltype(std::declval<V>().data())>
    threeroe(const V& v, uint64_t seed1=0, uint64_t seed2 = 0){
        Init<V>(v, seed1, seed2);
    }

    template<typename V, typename _dummy=decltype(std::declval<V>().data())>
    threeroe& Init(const V& v, uint64_t seed1=0, uint64_t seed2=0){
        Init(seed1, seed2);
        return Update<V>(v);
    }

    // Final() returns the hash, (a result_type) of the data Updated
    // so-far.  Final is const, so it can be called more than once
    // without suprise.  Note that it's a silent error to call
    // Jenkins' Spooky::Final more than once, or to call
    // Spooky::Update after Spooky::Final without an intervening Init.
    // Both are ok with threeroe.
    result_type
    Final() const{
        uint64_t s[] = {state[0], state[1], state[2], state[3]};
        // pad any deferred data with zeros and mix it in
        // with mixoneblock.
        if(bytes_deferred){
            char zeros[CHARS_PER_INBLK] = {};
            memcpy(zeros, deferred, bytes_deferred);
            mixoneblock(zeros, s);
        }
            
        return finish(len, s);
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
