#include "core123/sew.hpp"
#include "core123/datetimeutils.hpp"
#include "core123/strutils.hpp"
#include <iostream>
#include <vector>
#include <utility>

using core123::str;
using core123::timet_to_httpdate;
using core123::httpdate_to_timet;
namespace sew = core123::sew;

int
main(int, char **)
{
    struct timespec now1, now2, now3;
    sew::clock_gettime(CLOCK_REALTIME, &now1);
    auto s1 = str(now1);
    now2 = now1;
    auto s2 = str(now2);
    if (now1 == now2) {
        if (s1 != s2)
            throw std::runtime_error("timespec string mismatch \""+s1+"\" != \""+s2+"\"");
        sew::clock_gettime(CLOCK_REALTIME, &now3);
        auto s3 = str(now3);
        if (now1 != now3) {
            if (now1 < now3) {
                std::cout << "OK, timespec \"" << s1 << "\" < \"" << s3 << "\"\n";
            } else {
                throw std::runtime_error("timespec failed lt \"" + s1 + "\" \"" + s3 + "\"\n");
            }
        } else {
            throw std::runtime_error("timespec failed ne \"" + s1 + "\" \"" + s3 + "\"\n");
        }
    } else {
        throw std::runtime_error("timespec fail eq \""+s1+"\" \""+s2+"\"");
    }
    
    // Enhancement request:  this should work
    // str("the time is now: ", std::chrono::system_clock::now());
    // but it doesn't.  We have to str-ify the time_point.  
    str("the time is now: ", str(std::chrono::system_clock::now()));
    // The fix is *possibly* a partial specialization of
    // 'core123::detail::insertone' for time_point (and duration), but
    // it seems to require a bit of re-engineering of insertone.

    // date -u '%s %c' => 1524747193 Thu 26 Apr 2018 12:53:13 PM UTC
    const std::vector<std::pair<time_t,std::string> > tests{
        {1524747193, "Thu, 26 Apr 2018 12:53:13 GMT"},
        {0, "Thu, 01 Jan 1970 00:00:00 GMT"},
        {1, "Thu, 01 Jan 1970 00:00:01 GMT"},
        {86400, "Fri, 02 Jan 1970 00:00:00 GMT"},
        {INT32_MAX, "Tue, 19 Jan 2038 03:14:07 GMT"},
    };
    for (const auto &t : tests) {
        auto s = timet_to_httpdate(t.first);
        if (s != t.second) {
            std::cerr << "ERROR: timet_to_httpdate mismatch, got " << s << " for " << t.first << '\n';
            std::cerr << "            expected " << t.second << std::endl;
            return 1;
        }
        auto tt = httpdate_to_timet(t.second.c_str());
        if(tt != t.first) {
            std::cerr << "ERROR: httpdate_to_timet mismatch, got " << s << " for " << t.first << '\n';
            std::cerr << "            expected " << t.second << std::endl;
            return 1;
        }
    }
    bool caught = false;
    try{
        httpdate_to_timet("19 Jan 2038 03:14:07");
    }catch(std::system_error& se){
        caught = true;
        std::cout << "OK, intentionally bad call to httpdate_timet threw: what:" << se.what() << "\n";
    }
    if(!caught){
        std::cerr << "ERROR:  expected httpdate_to_timet to throw";
        return 1;
    }
    
    std::cout << "OK, " << tests.size() << " tests passed" << std::endl;
    return 0;
}
