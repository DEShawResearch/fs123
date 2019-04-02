/* This file is a derivative work.  The original file was part of the
 * Boost system, http://www.boost.org/.  The original header, and the
 * corresponding LICENSE_1_0.txt are reproduced at the bottom of this
 * file.
 */

// In addition, this file is subject to the following:
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


//#include "concepts.hpp"
#include <random>
#include <sstream>
#include <typeinfo>

#define BOOST_TEST_MAIN
//#include <boost/test/unit_test.hpp>
#define BOOST_AUTO_TEST_CASE(f) void f()
#define BOOST_CHECK_NE(a, b) Assert(a!=b)
#define BOOST_CHECK_EQUAL(a, b) Assert(a == b)
#define BOOST_CHECK_GE(a, b) Assert(a>=b)
#define BOOST_CHECK_LE(a, b) Assert(a<=b)
#define BOOST_CHECK(expr) Assert(expr)

//using boost::random::test::RandomNumberEngine;
//BOOST_CONCEPT_ASSERT((RandomNumberEngine< BOOST_RANDOM_RNE >));

typedef BOOST_RANDOM_RNE::result_type result_type;
typedef std::seed_seq seed_type;

#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable:4244)
#endif

template<class Converted, class RNE, class T>
void test_seed_conversion(RNE & rne, const T & t)
{
    Converted c = static_cast<Converted>(t);
    if(static_cast<T>(c) == t) {
        RNE rne2(c);
        std::ostringstream msg;
        msg << "Testing seed: type " << typeid(Converted).name() << ", value " << c;
        BOOST_CHECK_MESSAGE(rne == rne2, msg.str());
        rne2.seed(c);
        BOOST_CHECK_MESSAGE(rne == rne2, msg.str());
    }
}

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

BOOST_AUTO_TEST_CASE(test_default_seed)
{
    BOOST_RANDOM_RNE rne;
    BOOST_RANDOM_RNE rne2;
    rne2();
    BOOST_CHECK_NE(rne, rne2);
    rne2.seed();
    BOOST_CHECK_EQUAL(rne, rne2);
}

BOOST_AUTO_TEST_CASE(test_seed_seq_seed)
{
    std::seed_seq q;
    BOOST_RANDOM_RNE rne(q);
    BOOST_RANDOM_RNE rne2;
    BOOST_CHECK_NE(rne, rne2);
    rne2.seed(q);
    BOOST_CHECK_EQUAL(rne, rne2);
}

template<class CharT>
void do_test_streaming(const BOOST_RANDOM_RNE& rne)
{
    BOOST_RANDOM_RNE rne2;
    std::basic_ostringstream<CharT> output;
    output << rne;
    BOOST_CHECK_NE(rne, rne2);
    // restore old state
    std::basic_istringstream<CharT> input(output.str());
    input >> rne2;
    BOOST_CHECK_EQUAL(rne, rne2);
}

BOOST_AUTO_TEST_CASE(test_streaming)
{
    BOOST_RANDOM_RNE rne;
    rne.discard(9307);
    do_test_streaming<char>(rne);
#if !defined(BOOST_NO_STD_WSTREAMBUF) && !defined(BOOST_NO_STD_WSTRING)
    do_test_streaming<wchar_t>(rne);
#endif
}

BOOST_AUTO_TEST_CASE(test_discard)
{
    BOOST_RANDOM_RNE rne;
    BOOST_RANDOM_RNE rne2;
    BOOST_RANDOM_RNE rne3;
    BOOST_CHECK_EQUAL(rne, rne2);
    for(int i = 0; i < 9307; ++i){
        rne();
        rne3.seed();
        BOOST_CHECK_NE(rne, rne3);
        rne3.discard(i+1);
        BOOST_CHECK_EQUAL(rne, rne3);
    }
    BOOST_CHECK_NE(rne, rne2);
    rne2.discard(9307);
    BOOST_CHECK_EQUAL(rne, rne2);
}

BOOST_AUTO_TEST_CASE(test_copy)
{
    BOOST_RANDOM_RNE rne;
    rne.discard(9307);
    {
        BOOST_RANDOM_RNE rne2 = rne;
        BOOST_CHECK_EQUAL(rne, rne2);
    }
    {
        BOOST_RANDOM_RNE rne2(rne);
        BOOST_CHECK_EQUAL(rne, rne2);
    }
    {
        BOOST_RANDOM_RNE rne2;
        rne2 = rne;
        BOOST_CHECK_EQUAL(rne, rne2);
    }
}

BOOST_AUTO_TEST_CASE(test_min_max)
{
    BOOST_RANDOM_RNE rne;
    for(int i = 0; i < 10000; ++i) {
        result_type value = rne();
        BOOST_CHECK_GE(value, (BOOST_RANDOM_RNE::min)());
        BOOST_CHECK_LE(value, (BOOST_RANDOM_RNE::max)());
    }
}

BOOST_AUTO_TEST_CASE(test_comparison)
{
    BOOST_RANDOM_RNE rne;
    BOOST_RANDOM_RNE rne2;
    BOOST_CHECK(rne == rne2);
    BOOST_CHECK(!(rne != rne2));
    rne();
    BOOST_CHECK(rne != rne2);
    BOOST_CHECK(!(rne == rne2));
}

BOOST_AUTO_TEST_CASE(validate)
{
    BOOST_RANDOM_RNE rne;
    for(int i = 0; i < 9999; ++i) {
        rne();
    }
    if( BOOST_RANDOM_VALIDATION_VALUE == 0 )
        std::cout << "Suggested BOOST_RANDOM_VALIDATION_VALUE: " << rne() << "\n";
    else    
        BOOST_CHECK_EQUAL(rne(), BOOST_RANDOM_VALIDATION_VALUE);
}

BOOST_AUTO_TEST_CASE(validate_seed_seq)
{
    std::seed_seq seed;
    BOOST_RANDOM_RNE rne(seed);
    for(int i = 0; i < 9999; ++i) {
        rne();
    }
    // std::cout << rne() << "\n";
    if( BOOST_RANDOM_SEED_SEQ_VALIDATION_VALUE == 0 )
        std::cout << "Suggested BOOST_RANDOM_SEED_SEQ_VALIDATION_VALUE: " <<  rne() << "\n";
    else
        BOOST_CHECK_EQUAL(rne(), BOOST_RANDOM_SEED_SEQ_VALIDATION_VALUE);
}

// counter-based engines can do discards in O(1) time.  Let's
// check that many such "random" discards add up correctly.
void do_test_big_discard(uintmax_t bigjump){
    BOOST_RANDOM_RNE rne;
    BOOST_RANDOM_RNE rne2;
    BOOST_RANDOM_RNE rne3;

    BOOST_CHECK_EQUAL(rne, rne2);
    uintmax_t n = 0;
    rne2.discard(bigjump);
    while(n < bigjump){
        BOOST_CHECK_NE(rne, rne2);
        std::uniform_int_distribution<uintmax_t> d(1, 1+(bigjump-n)/73);;
        uintmax_t smalljump = d(rne3);
        rne.discard(smalljump);
        n += smalljump;
    }
    BOOST_CHECK_EQUAL(rne, rne2);
}

BOOST_AUTO_TEST_CASE(test_big_discard)
{
    do_test_big_discard((std::numeric_limits<uint32_t>::max)());
    do_test_big_discard(((std::numeric_limits<uint32_t>::max)()>>1) + 1);
    // Note that these  aren't really 'huge' discards.  We're
    // still limited to 2^32, and even that will fail if
    // our underlying Uint is smaller than 32 bits.  We'll
    // have to fix this test if we ever develop an 8x16
    // prf.
}        

/* test_generator.ipp
 *
 * Copyright Steven Watanabe 2011
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * $Id: test_generator.ipp 71018 2011-04-05 21:27:52Z steven_watanabe $
 *
 */

/*
Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/
