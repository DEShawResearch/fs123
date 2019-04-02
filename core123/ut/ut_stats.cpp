#include "ut_stats.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <core123/envto.hpp>
#include <core123/scoped_nanotimer.hpp>

static std::vector<std::pair<core123::stats_t *,bool>> stvec;

void stats_register(core123::stats_t& st, bool notiming) {
    stvec.emplace_back(&st,notiming);
}

void stats_dump() {
    bool notiming = core123::envto<bool>("MAFS_TEST_NOTIMING", false);
    for (auto s : stvec) {
	if (!notiming || !s.second) {
	    std::cout << *s.first;
	}
    }
}
    
counters_t c;
timers_t t;

int
main(int, char **) {
    {
	STATS_COUNTED_NANOTIMER(t, total);
	auto k = core123::envto<int>("TESTSTATS_COUNT", 10000);
	stats_register(c, false);
	stats_register(t, true);
	for (auto n = 0; n < 3; n++) {
	    for (auto i = 1; i <= k; i++) {
		core123::atomic_scoped_nanotimer _t(&t.fb_sec);
		if ((i % 3) == 0) c.fizz++;
		if ((i % 5) == 0) c.buzz++;
	    }
	    stats_dump();
	    std::cout << '\n';
	}
        std::cout << "Now call fizzbuzz in ut_stats2.cpp:\n";
        fizzbuzz(k); // implemented in another compilation unit
        // asert that the counts look right:
        assert( (k/3)*4 == c.fizz );
        assert( (k/5)*4 == c.buzz );
        stats_dump();
    }
    stats_dump();
    return 0;
}
