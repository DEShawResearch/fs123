// N.B.  the 'ins' and 'str' methods are tested in ut_inserters.cpp
// even though they're implemented in streamutils.hpp
#include <core123/streamutils.hpp>
#include <core123/ut.hpp>
#include <core123/timeit.hpp>
#include <sstream>

using namespace std;
using namespace core123;
int main(int, char **){
    // Do the savers work?
    std::ostringstream oss;
    // stream_precision_saver
    float x = 0.5f - 0x1.p-7f; // 0.4921875, exactly, no rounding
    oss << setprecision(7);
    {
        stream_precision_saver sps(oss);
        oss.str("");
        oss.precision(5);
        oss << x;
        EQSTR(oss.str(), "0.49219");
    }
    // precision is back to 7
    oss.str("");
    oss << x;
    EQSTR(oss.str(), "0.4921875");

    // stream_width_saver:
    oss.str("");
    oss.width(12);
    // Don't do oss<<x here.  It clears the width!
    {
        stream_width_saver sws(oss);
        oss.str("");
        oss.width(16);
        oss << x;
        EQSTR(oss.str(), "       0.4921875");
    }
    oss.str("");
    oss << x;
    EQSTR(oss.str(), "   0.4921875");
        
    // stream_flags_saver:
    oss.str("");
    {
        stream_flags_saver sfs(oss);
        oss << hex;
        oss << 99;
        EQSTR(oss.str(), "63");
    }
    oss.str("");
    oss << 99;
    EQSTR(oss.str(), "99");
    
    timeit_result r;
    nullstream1 ns1;
    r = timeit(chrono::seconds(2),
               [&](){
                   ns1 << 123456;
               });
    cout << "nullstream 1 discarded " << r.iter_per_sec() << " integers per sec\n";

    nullstream2 ns2;
    r = timeit(chrono::seconds(2),
               [&](){
                   ns2 << 123456;
               });
    cout << "nullstream 2 discarded " << r.iter_per_sec() << " integers per sec\n";

    nullstream3 ns3;
    r = timeit(chrono::seconds(2),
               [&](){
                   ns3 << 123456;
               });
    cout << "nullstream 3 discarded " << r.iter_per_sec() << " integers per sec\n";

    nullstream4 ns4;
    r = timeit(chrono::seconds(2),
               [&](){
                   ns4 << 123456;
               });
    cout << "nullstream 4 discarded " << r.iter_per_sec() << " integers per sec\n";

    // nullstream is the same as nullstream2.
    nullstream ns;
    r = timeit(chrono::seconds(2),
               [&](){
                   ns << 123456;
               });
    cout << "nullstream discarded " << r.iter_per_sec() << " integers per sec\n";
    // ns2 and ns3 intrinsically have badbit set.
    CHECK(!ns2);
    CHECK(!ns3);
    // ns4 really does the formatting, and doesn't set badbit.
    CHECK(!!ns4);
    return utstatus();

}
