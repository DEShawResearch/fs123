// Test scanint for all integral sizes and a few corner cases
// Mark Moraes. D. E. Shaw Research
#include "core123/svto.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <tuple>
#include <utility>

using namespace std;
using core123::svto;
using core123::svscan;
using core123::str_view;

// The compiler sees overflows in this code, but they're not
// really there because scanx (should) throw before we try
// to do comparisons with overflowing constant expresions.
#pragma GCC diagnostic ignored "-Woverflow"
#pragma GCC diagnostic ignored "-Wint-in-bool-context"

int FAIL = 0;
#define Assert(P) do{ if(!(P)){ std::cerr << "Assertion failed on line " << __LINE__ << ": "  #P << std::endl; FAIL++; } } while(0)

// FIXME - CHECK non-zero 'start' argument!!!

#define TEST_REAL(T, N) {\
    cout << #T << " " << N << " :" << std::endl;\
    auto s = std::to_string(N);\
    T x; \
    svscan(s.c_str(), &x);                                        \
    cout << " str \"" << s << "\" svscan " << x << std::endl;         \
    Assert(x==static_cast<T>(N));                                   \
    char& c = s.back();\
    char oc = c;\
    T sgn = ( s[0] == '-' ) ? -1 : 1; \
    if (oc != '0') {\
	c = oc-1; \
	try { \
        T x;  \
        svscan(s.c_str(), &x); \
	    cout << " str \"" << s << "\" svscan " << x << std::endl;\
            Assert(x==static_cast<T>(N-sgn));                           \
	} catch (std::invalid_argument& e) {\
	    cout << " ok, got invalid_argument as expected for \"" << s << "\" :\n  " << e.what() << endl;\
	}\
    }\
    if (oc != '9') {\
	c = oc+1; \
	try { \
            T x; \
	    svscan(s.c_str(), &x); \
	    cout << " str \"" << s << "\" svscan " << x << std::endl;\
            Assert(x==static_cast<T>(N+sgn)); \
	} catch (std::invalid_argument& e) {\
	    cout << " ok, got invalid_argument as expected for \"" << s << "\" :\n  " << e.what() << endl;\
	}\
    }\
    c = oc; \
    s += '0'; \
    try { \
        T x;\
	svscan(s.c_str(), &x); \
	cout << " str \"" << s << "\" svscan " << x << std::endl;\
        Assert(x == static_cast<T>(10*N));                        \
    } catch (std::invalid_argument& e) {\
	cout << " ok, got invalid_argument as expected for \"" << s << "\" :\n  " << e.what() << endl;\
    }\
    char &nc = s.back(); \
    nc = '-'; \
    size_t off = svscan(s.c_str(), &x);              \
    cout << " str \"" << s << "\" svscan " << x << std::endl;\
    Assert(x == static_cast<T>(N));              \
    Assert(off == s.size()-1);                 \
    cout << std::endl;\
}

#define TEST(T) {\
    TEST_REAL(T, numeric_limits<T>::min());\
    TEST_REAL(T, numeric_limits<T>::max());\
    if (numeric_limits<T>::min() != 0) {\
	TEST_REAL(T, 0);\
	TEST_REAL(T, -2);\
    }\
    cout << endl; \
}

struct udt{
    int i;
    float f;
    friend std::istream& operator>>(std::istream& is, udt& v){
        return is >> v.i >> v.f;
    }
};

int
main(int, char **) {
    {
        int a=0, b=0;
        const char* in = "1 2 ";
        auto start = svscan(in, std::tie(a, b));
        cout << "a=" << a << " b=" << b << " start=" << start << "\n";
        Assert(a==1);
        Assert(b==2);
        // Let's make sure that if we try to svscan past the end of
        // the input, we get an error.
        bool caught = false;
        try{
            int c;
            svscan(in, &c, start);
        }catch(std::exception& e){
            cout << "svscan(past EOF) correctly threw: " << e.what() << "\n";
            caught = true;
        }
        Assert(caught);
    }

    // check that svto is ok with CRLF (whitespace) following a conversion.
    Assert(svto<uint64_t>("6832456719796238295\r\n") == 6832456719796238295ull );

    TEST(short);
    TEST(unsigned short);
    TEST(int);
    TEST(unsigned int);
    TEST(long);
    TEST(unsigned long);
    TEST(long long);
    TEST(unsigned long long);
    TEST(bool);
    // test a vew bad strings
    for (auto s : {"", "-", "--", "a"}) {
	try {
            int x;
	    svscan(s, &x);
	    cout << " str \"" << s << "\" svscan " << x << std::endl;
	} catch (std::invalid_argument& e) {
	    cout << " ok, got invalid_argument as expected for \"" << s << "\" :\n  " << e.what() << endl;
	}
	try {
            unsigned long x;
	    svscan(s, &x);
	    cout << " str \"" << s << "\" svscan " << x << std::endl;
	} catch (std::invalid_argument& e) {
	    cout << " ok, got invalid_argument as expected for \"" << s << "\" :\n  " << e.what() << endl;
	}
    }

    // Some basic sanity tests for floating point?
    
    std::string s="3.1415";
    float f;
    svscan<float>(s, &f); Assert(f == 3.1415f );
    str_view sv(s);
    sv.remove_suffix(2);
    svscan<float>(sv, &f); Assert(f == 3.14f );
    s = "3.14e9999";
    svscan(s, &f); Assert( isinf(f) );
    sv = s;
    sv.remove_suffix(5);
    size_t off;

    off = svscan(sv, &f);
    Assert( f = 3.14f );
    Assert(off == 4);
    
    s = "Nano";
    svscan(s, &f); Assert( isnan(f) );
    s = "3.14e+x";
    off = svscan<float>(s, &f);
    Assert(f == 3.14f);
    Assert(off == 4);

    s = "3.14e40";
    svscan(s, &f); Assert(isinf(f));
    double d;
    svscan(s, &d); Assert( d == 3.14e40 );

    // And for UDTs.
    sv = "0xa 3.14 more stuff";
    udt u;
    off = svscan<udt>(sv, &u);
    Assert( u.i == 10 && u.f == 3.14f && off == 8);

    // Try the EOF-detector...
    sv = "1 2 3 4 5 6 7 8  ";
    size_t start = 0;
    for(int i=1; i<=8; ++i){
        int ii;
        start = svscan<int>(sv, &ii,start);
        Assert( ii == i );
    }
    off = svscan(sv, nullptr, start);
    Assert(off == sv.size());

    sv = "1 2 3 4 5 6 7 8 extra stuff ";
    start = 0;
    for(int i=1; i<=8; ++i){
        int ii;
        start = svscan<int>(sv, &ii, start);
        Assert( ii == i );
    }

    off = svscan(sv, nullptr, start);
    Assert(sv.substr(off) == "extra stuff ");
    
    vector<int> v(8);
    off = svscan(sv, begin(v), end(v), 0);
    Assert(sv.substr(off) == " extra stuff ");

    cout << FAIL << " Failed tests" << endl;
    return !!FAIL;
}
