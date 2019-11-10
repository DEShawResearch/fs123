#include <core123/bloomfilter.hpp>
#include <core123/ut.hpp>
#include <core123/diag.hpp>
#include <iostream>
#include <string>

namespace {
const auto _main = core123::diag_name("main");

unsigned testfalse(const core123::bloomfilter& bf, unsigned nf,
		   core123::str_view t) {
    std::string s(t);
    auto n = 0u;
    for (auto i = 0u; i < nf; i++) {
	s += ' ';
	if (bf.check(s)) n++;
    }
    DIAG(_main, n << " false positives out of " << nf);
    return n;
}

}

int main(int argc, char **argv) {
    const unsigned nfalse = 1000u;
    core123::bloomfilter bf(10, 10./nfalse);
    const char *tests[] = {"hello", "world", ""};
    for (const auto& t: tests) {
	bf.add(t);
	CHECK(bf.check(t));
	CHECK(testfalse(bf, nfalse, t) == 0);
    }
    CHECK(testfalse(bf, nfalse*10, "xxx") == 12);
    DIAG(_main, bf.popcount() << " bits set");
    CHECK(bf.popcount() == 17);
    return utstatus();
}

