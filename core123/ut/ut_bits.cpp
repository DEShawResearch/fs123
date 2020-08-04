// test program for bits class
// Mark Moraes, D. E. Shaw Research
#include <core123/bits.hpp>
#include <core123/ut.hpp>

#include <vector>
#include <utility>
#include <unordered_set>
#include <iostream>
#include <core123/diag.hpp>
#include <core123/strutils.hpp>

using namespace std;

namespace {
const auto _main = core123::diag_name("main");

void check(core123::bits& b, unordered_set<size_t>& testidx) {
    for (auto j = 0ul; j < b.sizebits(); j++) {
        if (testidx.find(j) == testidx.end()) {
	    CHECK(!b.get(j));
        } else {
	    CHECK(b.get(j));
        }
    }
}
} // namespace <anon>

int main(int, char **) {
    core123::bits b(300);
    auto bstr = str(b);
    DIAG(_main, "initial bits " << core123::cstr_encode(bstr));

    std::pair<size_t,size_t> testranges[] = {{0,5}, {58,66}, {296,b.sizebits()}};
    unordered_set<size_t> testidx;

    check(b, testidx);
    
    for (const auto& r : testranges) {
        for (auto i = r.first; i < r.second; i++) {
            testidx.insert(i);
            auto x = b.set(i);
            DIAG(_main, "setting bit " << i << " got " << x << ' ' << b.popcount() << " bits set; " << core123::cstr_encode(str(b)));
	    CHECK(!x);
            check(b, testidx);
        }
    }

    core123::bits bnew;

    stringstream oss1;
    oss1 << b;
    auto oss1str = oss1.str();
    DIAG(_main, "oss1 is: " << core123::cstr_encode(oss1str));
    oss1 >> bnew;
    check(bnew, testidx);

    core123::bits bx(std::move(bnew));
    check(bx, testidx);
    EQUAL(str(bx), oss1str);
    bx.clear();
    EQUAL(str(bx), bstr);
    
    for (const auto& r : testranges) {
        for (auto i = r.first; i < r.second; i++) {
            testidx.erase(i);
            auto x = b.unset(i);
            DIAG(_main, "unsetting bit " << i << " got " << x << ' ' << b.popcount() << " bits set; " << core123::cstr_encode(str(b)));
            check(b, testidx);
        }
    }

    stringstream oss2;
    oss2 << b;
    auto oss2str = oss2.str();
    EQUAL(oss2str, bstr);
    DIAG(_main, "oss2 is: " << core123::cstr_encode(oss2str));
    oss2 >> bnew;
    check(bnew, testidx);
    auto bnstr = str(bnew);
    EQUAL(bnstr, oss2str);
    EQUAL(bnstr, bstr);

    auto bm = std::move(b);
    EQUAL(str(bm), bstr);
    check(bm, testidx);

    return utstatus();
}
