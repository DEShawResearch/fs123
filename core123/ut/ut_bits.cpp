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

int main(int argc, char **argv) {
    core123::bits b(300);
    if (_main) b.sput(cout) << endl;

    std::pair<size_t,size_t> testranges[] = {{0,5}, {58,66}, {296,b.sizebits()}};
    unordered_set<size_t> testidx;

    check(b, testidx);
    
    for (const auto& r : testranges) {
        for (auto i = r.first; i < r.second; i++) {
            testidx.insert(i);
            auto x = b.set(i);
            DIAG(_main, "setting bit " << i << " got " << x << ' ' << b.popcount() << " bits set");
	    if (_main) b.sput(cout) << endl;
	    CHECK(!x);
            check(b, testidx);
        }
    }
    for (const auto& r : testranges) {
        for (auto i = r.first; i < r.second; i++) {
            testidx.erase(i);
            auto x = b.unset(i);
            DIAG(_main, "unsetting bit " << i << " got " << x << ' ' << b.popcount() << " bits set ");
	    if (_main) b.sput(cout) << endl;
            check(b, testidx);
        }
    }
    return utstatus();
}
