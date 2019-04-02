// test program for netstring.hpp
#include <core123/svstream.hpp>
#include <core123/netstring.hpp>
#include <core123/ut.hpp>
#include <random>

using std::to_string;
using std::runtime_error;
using core123::netstring;
using core123::sput_netstring;
using core123::sget_netstring;

static unsigned long numtests = 0;

void dotest(const char *buf, size_t n) {
    numtests += 1;
    auto s1 = netstring({buf, n});
    DIAG(_ut, "nstr : " << s1 << "\n");
    std::stringstream ss;
    sput_netstring(ss, {buf, n});
    auto s2 = ss.str();
    DIAG(_ut, "sput : " << s2 << "\n");
    EQSTR (s1, s2);
    ss.seekg(0);
    std::string s3;
    if (!sget_netstring(ss, &s3))
        throw runtime_error("ERROR got eof from sget_netstring, s3 is \""+s3+"\"");
    EQUAL (s3.size(), n);
    std::string sref(buf, n);
    EQSTR (s3, sref);
    sput_netstring(ss, {buf, n});
    ss << "\n  ";
    sput_netstring(ss, {buf, n});
    ss << "  \n";
    ss.seekg(0);
    auto k = 0u;
    while (1) {
        std::string s4;
        if (!sget_netstring(ss, &s4))
            break;
        EQUAL(s3.size(), n);
        EQSTR(s3, sref);
        k += 1;
    }
    EQUAL(k, 3);
}

int
main(int, char **)
{
    const size_t bufsz = 200;
    unsigned long ntests =  1000000;
    auto cp = getenv("NTESTS");
    if (cp) {
        ntests = std::stoul(cp);
    }
    char buf[bufsz];
    cp = getenv("SEED");
    if (cp) {
        srandom(std::stol(cp));
    }
    
    dotest("", 0);
    dotest("", 1);
    dotest("hello", 5);
    for(auto i = 0ul; i < ntests; i++) {
        size_t n = random() % 200;
        for (size_t j = 0; j < n; j++) {
            buf[j] = static_cast<char>((random() % 95) + 32);
        }
        auto sbuf = std::string(buf, n);
        DIAG(_ut, i << " buf \"" << sbuf << "\"");
        dotest(buf, n);
    }
    return utstatus();
}

