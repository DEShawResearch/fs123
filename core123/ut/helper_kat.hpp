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
#include <string>
#include <core123/svto.hpp>

template <typename Prf>
void dokat(const std::string& s){
    typename Prf::domain_type ctr;
    typename Prf::key_type key;
    typename Prf::range_type answer;
    core123::str_view sv(s);
#if 0
    // Sigh... svscan scans with base=0, not base=16.
    auto off = core123::svscan(s, ctr.begin(), ctr.end());
    off = core123::svscan(s, key.begin(), key.end(), off);
    off = core123::svscan(s, answer.begin(), answer.end(), off);
#else
    size_t off = 0;
    for(auto& c : ctr)
        off = core123::scanint<typename std::remove_reference<decltype(c)>::type, 16>(sv, &c, off);
    for(auto& k : key)
        off = core123::scanint<typename std::remove_reference<decltype(k)>::type, 16>(sv, &k, off);
    for(auto& a : answer)
        off = core123::scanint<typename std::remove_reference<decltype(a)>::type, 16>(sv, &a, off);
#endif
    
    typename Prf::range_type computed = Prf(key)(ctr);
    // N.B.  These produce unhelpful errors when they fail...
    // But they shouldn't fail, and if they do, go
    // straight to gdb.
    Assert(computed == answer);
    Prf prf(key);
    Assert(prf(ctr) == answer);
}

