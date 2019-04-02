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
#include "core123/intutils.hpp"
#include <cassert>
#include <iostream>
#include <typeinfo>
#include <tuple>
#include <random>

using std::uniform_int_distribution;
using std::mt19937;
using core123::mulhilo;
using core123::mulhilo_halfword;

int FAIL = 0;
#define Assert(P) do{ if(!(P)){ std::cerr << "Assertion failed on line " << __LINE__ << ": "  #P << std::endl; FAIL++; } } while(0)

//#define BOOST_TEST_MAIN
//#include <boost/test/unit_test.hpp>
#define BOOST_AUTO_TEST_CASE(f) void f()
#define BOOST_CHECK_NE(a, b) Assert(a!=b)
#define BOOST_CHECK_EQUAL(a, b) Assert(a == b)
#define BOOST_CHECK_GE(a, b) Assert(a>=b)
#define BOOST_CHECK_LE(a, b) Assert(a<=b)
#define BOOST_CHECK(expr) Assert(expr)

template <typename UINT>
void doit(){
    uniform_int_distribution<UINT> D;
    mt19937 mt;
    for(int i=0; i<1000000; ++i){
        UINT a = D(mt);
        UINT b = D(mt);

        UINT hi, lo;
        std::tie(lo, hi) = mulhilo(a, b);
        BOOST_CHECK_EQUAL(lo, (UINT)(a*b) );
        // Can't we say something about hi/a and b 
        // and hi/b and a?

        UINT hi_hw, lo_hw;
        std::tie(lo_hw, hi_hw) = mulhilo_halfword(a, b);
        BOOST_CHECK_EQUAL(lo_hw, lo);
        BOOST_CHECK_EQUAL(hi_hw, hi);
        //std::cout << a << " * " << b << " = " << hi << "." << lo << "\n";
    }
}

BOOST_AUTO_TEST_CASE(test_mulhilo)
{
    doit<uint8_t>();
    doit<uint16_t>();
    doit<uint32_t>();
    doit<uint64_t>();
}

int main(int, char **){
    test_mulhilo();
    std::cout << FAIL << " Failed tests" << std::endl;
    return !!FAIL;
}
