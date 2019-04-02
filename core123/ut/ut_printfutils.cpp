// unit tests for fmt (formerly stringprintf)
#include "core123/strutils.hpp"
#include "core123/datetimeutils.hpp"
#include <limits>
#include <string>
#include <sstream>
#include <cstring>
//#include <boost/test/minimal.hpp>
//#define  ASSERT BOOST_REQUIRE
//#define  MAIN test_main
#include <cassert>
#define  ASSERT assert
#define  MAIN main
#include <cstdlib>

using namespace std;
using core123::fmt;
using core123::str;
using core123::nanos;

// FIXME - test more, e.g., especially things going out of scope.
// A wider variety of printf formats would be nice, but do we
// really think that the underlying vsnprintf is broken?
int MAIN(int argc, char **argv){

    string s;
    char buf[8192];
    stringstream ss;

    s = fmt("%f", 3.1415);
    ASSERT(s == "3.141500");
    
#pragma GCC diagnostic ignored "-Wformat-zero-length"
    s = fmt("");
    ASSERT(s == "");

    s = fmt("%34s", "hello world");
    sprintf(buf, "%34s", "hello world");
    ASSERT(s == buf);

    // The initial buffer in printfutils.hpp is
    // 512 bytes long.  Let's try 512, then 511
    // and then 513.
    s = fmt("%512s", "hello world");
    sprintf(buf, "%512s", "hello world");
    ASSERT(s == buf);
    
    s = fmt("%511s", "hello world");
    sprintf(buf, "%511s", "hello world");
    ASSERT(s == buf);

    s = fmt("%513s", "hello world");
    sprintf(buf, "%513s", "hello world");
    ASSERT(s == buf);
    
    s = fmt("%5000s", "hello world");
    sprintf(buf, "%5000s", "hello world");
    ASSERT(s == buf);
    ASSERT(s.size() == 5000);
                  
    // strns and fmtdur are handy formatters for integer nanosecond
    // counters and chrono::durations.  Let's exercise them a bit:
    s = str(nanos(0));
    using std::chrono::nanoseconds;
    std::string durs;
    durs = str(nanoseconds(0));
    ASSERT(durs == s);
    ASSERT(s == "0.000000000");

    s = str(nanos(-1));
    durs = str(nanoseconds(-1));
    ASSERT(durs == s);
    ASSERT(s == "-0.000000001");

    s = str(nanos(1234567890));
    durs = str(nanoseconds(1234567890));
    ASSERT(durs == s);
    ASSERT(s == "1.234567890");

    s = str(nanos(std::numeric_limits<long long>::min()));
    ASSERT(s == "-9223372036.854775808"); // assumes 64-bit long long
    return 0;
}
