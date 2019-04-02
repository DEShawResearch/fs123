// Demonstrate that stats_t is polymorphic, and that we
// can have two different derived types:  counters_t
// and timers_t.  This file is included in: ut_stats.hpp
// and ut_stats2.hpp, both of which update the statistics
// in t and c.

#include <core123/stats.hpp>

#define STATS_INCLUDE_FILENAME "teststats_c"
#define STATS_STRUCT_TYPENAME counters_t
#include <core123/stats_struct_builder>

#define STATS_INCLUDE_FILENAME "teststats_t"
#define STATS_STRUCT_TYPENAME timers_t
#include <core123/stats_struct_builder>

extern timers_t t;  // defined in ut_stats.cpp
extern counters_t c;// defined in ut_stats.cpp
void fizzbuzz(int); // defined in ut_stats2.cpp
