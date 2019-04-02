/** @page LICENSE
Copyright 2010-2012, D. E. Shaw Research.
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
#include <core123/philox.hpp>
#include <cstdint>

using core123::philox;

int FAIL = 0;
#define Assert(P) do{ if(!(P)){ std::cerr << "Assertion failed on line " << __LINE__ << ": "  #P << std::endl; FAIL++; } } while(0)

#include "helper_kat.hpp"

#define BOOST_AUTO_TEST_CASE(f) void f()

// The KAT vectors are cut-and-pasted from the kat_vectors file
// in the original Random123 distribution.  The generators that
// produce these known answers have been extensively tested and are
// known to be Crush-Resistant (see Salmon et al, "Parallel Random
// Numbers:  As Easy as 1, 2, 3")
BOOST_AUTO_TEST_CASE(test_kat_philox2x32)
{
    dokat<philox<2, uint32_t, 7> > ("243f6a88 85a308d3 13198a2e   bedbbe6b e4c770b3");
    dokat<philox<2, uint32_t, 7> > ("00000000 00000000 00000000   257a3673 cd26be2a");
    dokat<philox<2, uint32_t, 7> > ("ffffffff ffffffff ffffffff   ab302c4d 3dc9d239");
    dokat<philox<2, uint32_t, 10> >("00000000 00000000 00000000   ff1dae59 6cd10df2");
    dokat<philox<2, uint32_t, 10> >("ffffffff ffffffff ffffffff   2c3f628b ab4fd7ad");
    dokat<philox<2, uint32_t, 10> >("243f6a88 85a308d3 13198a2e   dd7ce038 f62a4c12");
}

BOOST_AUTO_TEST_CASE(test_kat_philox2x64)
{
    dokat<philox<2, uint64_t, 7>  >("0000000000000000 0000000000000000 0000000000000000   b41da69fbfefc666 511e9ce1a5534056 ");
    dokat<philox<2, uint64_t, 7>  >("ffffffffffffffff ffffffffffffffff ffffffffffffffff   a4696cc04462015d 724782dae17169e9 ");
    dokat<philox<2, uint64_t, 7>  >("243f6a8885a308d3 13198a2e03707344 a4093822299f31d0   98ed1534392bf372 67528b1568882fd5 ");
    dokat<philox<2, uint64_t, 10> >("0000000000000000 0000000000000000 0000000000000000   ca00a0459843d731 66c24222c9a845b5");
    dokat<philox<2, uint64_t, 10> >("ffffffffffffffff ffffffffffffffff ffffffffffffffff   65b021d60cd8310f 4d02f3222f86df20");
    dokat<philox<2, uint64_t, 10> >("243f6a8885a308d3 13198a2e03707344 a4093822299f31d0   0a5e742c2997341c b0f883d38000de5d");
}

BOOST_AUTO_TEST_CASE(test_kat_philox4x32)
{
    dokat<philox<4, uint32_t, 7> > ("00000000 00000000 00000000 00000000 00000000 00000000   5f6fb709 0d893f64 4f121f81 4f730a48 ");
    dokat<philox<4, uint32_t, 7> > ("ffffffff ffffffff ffffffff ffffffff ffffffff ffffffff   5207ddc2 45165e59 4d8ee751 8c52f662 ");
    dokat<philox<4, uint32_t, 7> > ("243f6a88 85a308d3 13198a2e 03707344 a4093822 299f31d0   4dfccaba 190a87f0 c47362ba b6b5242a ");
    dokat<philox<4, uint32_t, 10> >(" 00000000 00000000 00000000 00000000 00000000 00000000   6627e8d5 e169c58d bc57ac4c 9b00dbd8");
    dokat<philox<4, uint32_t, 10> >(" ffffffff ffffffff ffffffff ffffffff ffffffff ffffffff   408f276d 41c83b0e a20bc7c6 6d5451fd");
    dokat<philox<4, uint32_t, 10> >(" 243f6a88 85a308d3 13198a2e 03707344 a4093822 299f31d0   d16cfe09 94fdcceb 5001e420 24126ea1");
}

BOOST_AUTO_TEST_CASE(test_kat_philox4x64)
{
    dokat<philox<4, uint64_t, 7>  >("0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000   5dc8ee6268ec62cd 139bc570b6c125a0 84d6deb4fb65f49e aff7583376d378c2 ");
    dokat<philox<4, uint64_t, 7>  >("ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff   071dd84367903154 48e2bbdc722b37d1 6afa9890bb89f76c 9194c8d8ada56ac7 ");
    dokat<philox<4, uint64_t, 7>  >("243f6a8885a308d3 13198a2e03707344 a4093822299f31d0 082efa98ec4e6c89 452821e638d01377 be5466cf34e90c6c   513a366704edf755 f05d9924c07044d3 bef2cb9cbea74c6c 8db948de4caa1f8a ");
    dokat<philox<4, uint64_t, 10> >(" 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000   16554d9eca36314c db20fe9d672d0fdc d7e772cee186176b 7e68b68aec7ba23b");
    dokat<philox<4, uint64_t, 10> >(" ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff   87b092c3013fe90b 438c3c67be8d0224 9cc7d7c69cd777b6 a09caebf594f0ba0");
    dokat<philox<4, uint64_t, 10> >(" 243f6a8885a308d3 13198a2e03707344 a4093822299f31d0 082efa98ec4e6c89 452821e638d01377 be5466cf34e90c6c   a528f45403e61d95 38c72dbd566e9788 a5a1610e72fd18b5 57bd43b5e52b7fe6");
}

int  main(int, char **){
    test_kat_philox2x32();
    test_kat_philox4x32();
    test_kat_philox2x64();
    test_kat_philox4x64();
    std::cout << FAIL << " Failed tests" << std::endl;
    return !!FAIL;
}
