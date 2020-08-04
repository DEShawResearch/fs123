// This is a reimplementation of the 'Verification' test from smhasher,
// without all of smhasher's baggage.  

#include "core123/threeroe.hpp"
#include "core123/scanint.hpp"
#include "core123/strutils.hpp"
#include "core123/timeit.hpp"
#include "core123/datetimeutils.hpp"
#include "core123/ut.hpp"
#include <vector>
#include <chrono>
#include <cstdio>
#include <cassert>
#include <stdexcept>
#include <utility>
#include <numeric>

using core123::threeroe;
using core123::timeit;
using core123::dur2dbl;

void od(uint8_t *p, size_t n){
    for(size_t i=0; i<n; ++i){
        printf("%02x ", p[i]);
    }
    printf("\n");
}

uint32_t verifier ( )
{
  std::vector<uint8_t> key;
  std::vector<unsigned char> hashes(16*256);

  // The structure of this 'verifier' comes from the SMHahser
  // distribution.  It produces a 4-byte result that would be hard to
  // reproduce except by correctly performing a modest number (257)
  // hashes of a variety of keys of different lengths and seeds.
  
  // Hash input 'keys' of the form {0}, {0,1}, {0,1,2}... up to
  // N=255,using 256-N as the seed.  Concatenate all the hashes into
  // an array of length 256*16, and then hash that.  Return the first
  // four bytes as an unsigned little-endian 32-bit value.  The
  // "correct" value, in threeroe::SMHash_verifier, was established by
  // early released versions of threroe, and should *never* be
  // changed.
  
  threeroe h, h2;

  for(int i = 0; i < 256; i++)
  {
    h = threeroe(256-i);
    h.update(key);
    key.push_back(i);
    threeroe::digest_type* pi = (threeroe::digest_type*)&hashes[16*i];
    *pi = h.digest();
    //printf("i=%d ", i);
    //od((uint8_t*)&hashes[2*i], 16);

    // N.b.  the rest of the loop isn't part of the SHhasher verifier,
    // but it verifies a few threeroe member functions.

    // One day, we'll have extensive unit tests of the
    // Init/update/Final logic.  Until then, we'll satisfy ourselves
    // with some code here that confirms that one-big-update (above)
    // is the same as a-bunch-of-small-updates (below).
    int j=0;
    int seen=0;
    h2 = threeroe(256-i);
    while(seen + j < i){
        h2.update(&key[seen], j);
        seen += j;
        j += 1;
    }
    h2.update(&key[seen], i-seen);
    // Also verify that hexdigest and str() (which calls ostream<<(result_type))
    // look the same
    std::string hexdigest = h2.hexdigest();
    // and that the digits in hexdigest correspond to the bytes in hashes (from digest())
#if __cpp_lib_byte > 201603
    std::cout << "Testing cpp_lib_byte\n";
    std::array<std::byte, 16> bd = h2.bytedigest();
    if(!memcmp(bd.data(), pi->data(), 16))
        throw std::runtime_error("mismatch between bytedigest and digest");
#endif
    for(int k=0; k<16; ++k){
        // convert the k'th pair of hex digits in hexdigest into uc:
        unsigned char uc;
        core123::scanint<unsigned char, 16, false>(hexdigest.substr(2*k, 2), &uc);
        // compare uc with the k'th byte in hashes
        if(uc != hashes[16*i + k])
            throw std::runtime_error("mismatch testing update/hexdigest functionality!");
    }
  }

  // Then hash the result array.

  // Use the fancy C++11 constructor if we can:
  auto answer = threeroe(hashes).digest();
  //printf("Verifier final: 0x%016lx 0x%016lx\n", final, ignored);


  // The first four bytes of that hash, interpreted as a little-endian integer, is our
  // verification value.  For little-endian, we can't use the 'ntox' or 'xton'
  // functions that posix standardized.  Just do it ourselves...

  return answer[3]<<24 | answer[2]<<16 | answer[1]<<8 | answer[0];
}

void trbench(size_t nbytes){
    std::vector<unsigned char> v(nbytes);
    std::iota(v.begin(), v.end(), 0);

    uint64_t sum = 0;
    auto timing = timeit(std::chrono::seconds(1), [&](){
                                 auto h = threeroe(v).hashpair64();
                                 sum += h.first + h.second;
                             });
    if(sum==0)
        std::cout << "Surpise!  sum==0\n";
    auto sec = dur2dbl(timing.dur);
    auto nhash = timing.count;
    std::cout << std::setprecision(2) << std::fixed;
    std::cout << "threeroe(" << nbytes << " bytes): " << (nhash/1e6)/sec << " Mhashes/sec, "
              << (sec*1.e9)/nhash << "nsec/hash, " << (nbytes*nhash/1.e6)/sec << " Mbytes/sec\n";
}

int main(int,  char **){
    uint32_t v = verifier();
    int errs = 0;
    printf("computed SMHash_Verifier = 0x%08x ", v);
    if( v == threeroe::SMHasher_Verifier ){
        printf("PASS\n");
    }else{
        printf("FAIL expected 0x%08x\n", threeroe::SMHasher_Verifier);
        errs++;
    }

    trbench(0);
    trbench(0);
    trbench(1);
    trbench(2);
    trbench(16);
    trbench(32);
    trbench(64);
    trbench(128);
    trbench(1000);
    trbench(1000000);
    trbench(100000000);

    return utstatus();
}
