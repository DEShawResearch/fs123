// Test scanint for all integral sizes and a few corner cases
// Mark Moraes. D. E. Shaw Research
#include "core123/scanint.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>

using namespace std;
using core123::scanint;
using core123::str_view;

// The compiler sees overflows in this code, but they're not
// really there because scanint (should) throw before we try
// to do comparisons with overflowing constant expresions.
#pragma GCC diagnostic ignored "-Woverflow"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wbool-compare"
#pragma GCC diagnostic ignored "-Wint-in-bool-context"

int FAIL = 0;
#define Assert(P) do{ if(!(P)){ std::cerr << "Assertion failed: "  #P << std::endl; FAIL++; } } while(0)

// FIXME - CHECK non-zero 'start' argument!!!
template <typename T, int BASE>
void test_real(T N){
    std::ostringstream oss; 
    char baseminus1;        
    if(BASE==16){ 
        baseminus1='f';                        
        if(N>=0) 
            oss << std::hex << "0x" << N;           
        else                                      
            oss << std::hex << "-0x" << -N; 
    }else if(BASE==-16){
        // -16 is just a hack to exercise upper-case hex.
        // It's not meant to "make sense"
        baseminus1='F';
        if(N>=0) 
            oss << std::hex << std::uppercase << "0X" << N;           
        else                                      
            oss << std::hex << std::uppercase << "-0X" << -N; 
    }else if(BASE==8){                      
        baseminus1 = '7';                   
        if(N>=0){                           
            oss << std::oct << "0" << N;     
        }else{                              
            oss << std::oct << "-0" << -N;   
        }                                   
    }else{                                      
        baseminus1 = '9';                       
        oss << N;                               
    }                                           
    auto sz = oss.str().size(); 
    char *p = (char *)::malloc(sz);     
    ::memcpy(p, oss.str().c_str(), sz); 
    core123::str_view s(p, sz); 
    T x;
    scanint(s, &x);
    cout << " str \"" << s << "\" scanint " << x << std::endl;
    Assert(x==static_cast<T>(N));                                   
    char& c = const_cast<char&>(s.back());                          
    char oc = c;
    T sgn = ( s[0] == '-' ) ? -1 : 1; 
    if (oc != '0') {
	c = oc-1; 
	try { 
            T x;
	    scanint(s, &x);
	    cout << " str \"" << s << "\" scanint " << x << std::endl;
            Assert(x==static_cast<T>(N-sgn));                           
	} catch (std::invalid_argument& e) {
	    cout << " ok, got invalid_argument error as expected for \"" << s << "\" :\n  " << e.what() << endl;
	}
    }
    if (oc != baseminus1) {
	c = oc+1; 
	try { 
            T x;
	    scanint(s, &x);
	    cout << " str \"" << s << "\" scanint " << x << std::endl;
            Assert(x==static_cast<T>(N+sgn)); 
	} catch (std::invalid_argument& e) {
	    cout << " ok, got invalid_argument error as expected for \"" << s << "\" :\n  " << e.what() << endl;
	}
    }
    c = oc; 
    p = (char*)::realloc(p, sz+1); 
    p[sz] = '0'; 
    s = core123::str_view(p, sz+1); 
    try { 
        T x;
	scanint(s, &x);
	cout << " str \"" << s << "\" scanint " << x << std::endl;
        Assert(x == static_cast<T>((BASE?std::abs(BASE):10)*N));                        
    } catch (std::invalid_argument& e) {
	cout << " ok, got invalid_argument error as expected for \"" << s << "\" :\n  " << e.what() << endl;
    }
    char &nc = const_cast<char&>(s.back());     
    nc = '-'; 
    size_t q = scanint(s, &x);
    cout << " str \"" << s << "\" scanint " << x << std::endl;
    Assert(x == static_cast<T>(N));              
    Assert(q == s.size()-1);                 
    ::free(p);                                      
    cout << std::endl;
}

#define TEST_REAL(T, N, BASE) {                         \
    cout << #T << " " << N << " base=" << BASE << " :" << std::endl; \
    test_real<T, BASE>(static_cast<T>(N)); \
    }

#define TESTB(T,BASE) {                                         \
        TEST_REAL(T, numeric_limits<T>::min(), BASE);        \
        TEST_REAL(T, numeric_limits<T>::max(), BASE);           \
    if (numeric_limits<T>::min() != 0) {\
	TEST_REAL(T, 0, BASE);               \
	TEST_REAL(T, -2, BASE);                 \
    }\
    cout << endl; \
}

#define TEST(T) { \
        TESTB(T, 0); \
                 TESTB(T, 8); \
                 TESTB(T, 10); \
                 TESTB(T, 16); \
                 TESTB(T, -16); \
                 }\

template <class TYPE, int base>
void testcorner(const std::string& s, TYPE first, long second){
    // Copy p into a tightly malloc'ed buffer so
    // that valgrind can detect any attempt to read
    // past the end.
    std::cout << "testcorner \"" << s << "\"\n";
    char *p = (char*)::malloc(s.size());
    ::memcpy(p, s.data(), s.size());
    bool caught = false;
    TYPE result;
    size_t off = 0;
    try{
        off = scanint<TYPE, base> (str_view(p, s.size()), &result);
        std::cout << " got " << result  << ", " << off << "\n";
    }catch(std::invalid_argument& e){
        caught = true;
        std::cout << "caught: " << e.what() << "\n";
    }
    if(second < 0){
        Assert(caught);
    }else{
        Assert(!caught);
        Assert( first == result );
        Assert( size_t(second) == off );
    }

    free(p);
}

int
main(int, char **) {
    TEST(short);
    TEST(unsigned short);
    TEST(int);
    TEST(unsigned int);
    TEST(long);
    TEST(unsigned long);
    TEST(long long);
    TEST(unsigned long long);
    TEST(bool);
    // test a few bad strings
    {
        int i;
        // ':' is the next  character after '9' in ASCII.
        // Make sure we're not confused by that!
        auto nxt = scanint<int, 11>("2:", &i, 0);
        Assert(nxt == 1 && i == 2);
    }
    for (auto s : {"", "-", "--", "a"}) {
	try {
            int x;
	    scanint<int>(s, &x);
	    cout << " str \"" << s << "\" scanint " << x << std::endl;
	} catch (std::invalid_argument& e) {
	    cout << " ok, got invalid_argument error as expected for \"" << s << "\" :\n  " << e.what() << endl;
	}
	try {
            unsigned long x;
	    scanint(s, &x);
	    cout << " str \"" << s << "\" scanint " << x << std::endl;
	} catch (std::invalid_argument& e) {
	    cout << " ok, got invalid_argument error as expected for \"" << s << "\" :\n  " << e.what() << endl;
	}
    }

    // Check true and false.  
    testcorner<bool, 10>("true", 1, 4);
    testcorner<bool, 10>(" truedat", 1, 5);
    testcorner<bool, 10>("True", 1, 4);
    testcorner<bool, 10>("TruE", 1, 4);
    testcorner<bool, 3>("false", 0, 5);
    testcorner<bool, 10>("FalsE", 0, 5);
    testcorner<bool, 10>("False", 0, 5);
    testcorner<bool, 16>("falseish", 0, 5);
    testcorner<bool, 16>("0x0xyzzy", 0, 3);
    testcorner<bool, 16>("0x0abc", 0, -1); // overflow
    testcorner<bool, 10>("000000", 0, 6);
    testcorner<bool, 10>("0x1", 0, 1);
    testcorner<bool, 16>("0x1", 1, 3);

    testcorner<int, 0>("0x", 0, 1);
    testcorner<int, 0>("-0x", 0, 2);
    testcorner<unsigned, 0>("-0x", 0, -1);
    testcorner<unsigned, 10>("-0", 0, -1);
    testcorner<unsigned, 16>("-0x", 0, -1);
    testcorner<int, 16>("0x", 0, 1);
    testcorner<int, 16>(" -0x", 0, 3);
    testcorner<int, 0>("-0x", 0, 2);
    testcorner<int, 0>(" -0x", 0, 3);
    testcorner<int, 0>("  0xgoo", 0, 3);
    testcorner<unsigned, 0>("   0xgoo", 0, 4);
    testcorner<unsigned, 0>("  -0xgoo", 0, -1);
    testcorner<int, 8>("  0xgoo", 0, 3);
    testcorner<unsigned, 8>("   0xgoo", 0, 4);
    testcorner<unsigned, 8>("  -0xgoo", 0, -1);
    testcorner<int, 16>("  0Xgoo", 0, 3);
    testcorner<unsigned, 16>("   0xgoo", 0, 4);
    testcorner<unsigned, 16>("  -0xgoo", 0, -1);
    testcorner<unsigned, 36>("   0xgoo", 1561272/*24 + 36*(24 + 36*(16 + 36*33))*/, 8);
    
    // Try a bunch of combinations of signs and whitespace
    // that should all fail.
    testcorner<int, 0>("++19", 0, -1);
    testcorner<int, 0>("-+19", 0, -1);
    testcorner<int, 0>("+-19", 0, -1);
    testcorner<int, 0>("--19", 0, -1);
    testcorner<int, 0>("- 3", 0, -1);
    testcorner<int, 10>("- 3", 0, -1);
    testcorner<int, 0>("  + 3", 0, -1);
    testcorner<int, 10>("  + 3", 0, -1);
    testcorner<int, 0>("  + -3", 0, -1);
    testcorner<int, 10>("  + -3", 0, -1);
    
    cout << FAIL << " Failed tests" << endl;
    return !!FAIL;
}
