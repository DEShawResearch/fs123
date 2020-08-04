#include "core123/counter_based_engine.hpp"
#include "core123/threefry.hpp"
#include "core123/philox.hpp"
#include "core123/ut.hpp"
#include "core123/complaints.hpp"
#include <type_traits>
#include <sstream>

using namespace core123;

// N.B. there's *a lot* more testing of the counter_based_engine in
// ut_threefry2x64.cpp and ut_philox4x64.cpp, via helper_engine.ipp.

int main(int, char **) try {
    bool threw;
    EQUAL((counter_based_engine<threefry<4, uint64_t>, 0>().sequence_length()), 4);
    EQUAL((counter_based_engine<threefry<4, uint64_t>, 1>().sequence_length()), 8);
    EQUAL((counter_based_engine<threefry<4, uint64_t>, 2>().sequence_length()), 16);
    auto ullmax = std::numeric_limits<unsigned long long>::max();
    EQUAL((counter_based_engine<threefry<4, uint64_t>, 61>().sequence_length()), uint64_t(1)<<63);
    EQUAL((counter_based_engine<threefry<4, uint64_t>, 62>().sequence_length()), ullmax);
    EQUAL((counter_based_engine<threefry<4, uint64_t>, 63>().sequence_length()), ullmax);
    EQUAL((counter_based_engine<threefry<4, uint64_t>, 64>().sequence_length()), ullmax);
    
    using prf_t = philox<2, uint64_t>;
    prf_t::key_type k = {99};
    prf_t::domain_type c0 = {};
    using g_t = counter_based_engine<prf_t, 5>;
    auto g = make_counter_based_engine<5>(prf_t(k), c0);

    CHECK((std::is_same<g_t, decltype(g)>::value));
    // Default-construct g1.  
    decltype(g) g1;
    EQUAL(g1.navail(), 64);
    CHECK(g1 != g);
    
    // Check that we can't construct a cbg with high bits set in the counter:
    threw = false;
    try{
        prf_t::domain_type d0 = {};
        d0.back() = 1ull<<60;
        auto g2 = make_counter_based_engine<4>(prf_t(k), d0);
        CHECK(false); // can't get here!
        g2();
    }catch(std::invalid_argument& e){
        threw = true;
    }
    CHECK(threw);

    // Assign into g1 and check equality
    std::stringstream ss;
    ss << g;
    ss >> g1;
    EQUAL(g, g1);
    
    auto first_value = g();
    std::cout << first_value << "\n";
    auto second_value = g();
    std::cout << second_value << "\n";
    std::cout << "Value from g1: " << g1() << "\n";
    // WARNING:  don't call stateful functions like
    // g() or g1() inside the UNIT_TEST macros.  The
    // macros evaluate their arguments multiple times,
    // and when the test fails, the arguments get evaluated
    // *more*, which leads to spurious downstream failures.
    {auto x = g1();
        CHECK(second_value == x);}
    
    // Advance g by a few:
    static int FEW = 11;
    for(int i=0; i<FEW; ++i)
        g();
    // reassign g1 to g
    ss.str({}); ss.clear();
    ss << g;
    ss >> g1;
    // g1 is the same as g.  They've both consumed FEW+2.
    // Check that they're still tracking
    EQUAL(g, g1);
    for(int i=0; i<FEW; ++i){
        auto x = g();
        auto y = g1();
        EQUAL(x, y);
    }
    
    // g and g1 have now both consumed 2*FEW + 2
    // Let's jump to the end:
    auto nav = g.navail();
    EQUAL(nav, (unsigned)(64 - 2*FEW - 2));
    // Incrementally with g1:
    for(unsigned i=0; i<nav-1; i++)
        g1();
    // And using discard with g:
    g.discard(nav-1);
    { auto x = g(), y=g1(); 
        EQUAL(x, y);}
    EQUAL(g, g1);
    EQUAL(g.navail(), 0);
    EQUAL(g1.navail(), 0);
    threw = false;
    try{
        g1();
    }catch(std::out_of_range& e){
        threw = true;
    }
    CHECK(threw);
    // And again!
    threw = false;
    try{
        g1();
    }catch(std::out_of_range& e){
        threw = true;
    }
    CHECK(threw);
    
    // Let's check on navail when the numbers are really large:
    {
    auto gx = make_counter_based_engine<64>(threefry<4, uint64_t>({1, 2, 3, 4}), {});
    
    unsigned long long bigly = std::numeric_limits<unsigned long long>::max();
    EQUAL(gx.navail(), bigly); 
    // It's still bigly if we call the generator once:
    gx();
    EQUAL(gx.navail(), bigly); 
    // And even if we jump ahead by bigly three more times:
    gx.discard(bigly);
    EQUAL(gx.navail(), bigly); 
    gx.discard(bigly);
    EQUAL(gx.navail(), bigly); 
    gx.discard(bigly);
    EQUAL(gx.navail(), bigly); 
    gx(); gx(); gx();
    EQUAL(gx.navail(), bigly); 
    // And finally, we've consumed enough of them that we can count what's left 
    gx();
    EQUAL(gx.navail(), bigly-1); 
    }

    // Let's check that the weird corner case with BITS=0 "works"
    {
        auto gx = make_counter_based_engine<0>(threefry<4, uint64_t>({1,2,3,4}), {});
        EQUAL(gx.navail(), 4);
        auto r1 = gx();
        EQUAL(gx.navail(), 3);
        gx.discard(2);
        EQUAL(gx.navail(), 1);
        auto r2 = gx();
        std::cout << r1 << " " << r2 << "\n";
        threw = false;
        try{
            r2 = gx();
        }catch(std::out_of_range& e){
            threw = true;
        }
        CHECK(threw);
    }

    // Does discard work from all states?
    // Does discard work on an a reinit-ed generator?
    for(int nshort = 0; nshort <= 64; ++nshort){
        try{
        g1.reinit({});
        EQUAL(g1.navail(), 64);
        g1.discard(64-nshort);
        EQUAL(g1.navail(), (unsigned)nshort);
        threw = 0;
        g1.discard(nshort); // no problem
        }catch(std::exception&){
            CHECK(false);
        }
        try{
            g1.discard(1);
        }catch(std::out_of_range&){
            threw = true;
        }
        CHECK(threw);
    }

    return utstatus();
 }catch(std::exception& e){
    complain(e, "Caught exception in main");
 }
