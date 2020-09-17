// test program for netstring.hpp
#include <core123/netstring.hpp>
#include <core123/ut.hpp>
#include <core123/str_view.hpp>
#include <core123/diag.hpp>
#include <core123/svstream.hpp>
#include <core123/svto.hpp>
#include <core123/complaints.hpp>
#include <random>

using namespace std;
using core123::str_view;
using core123::netstring;
using core123::svscan_netstring;
using core123::sput_netstring;
using core123::sget_netstring;

static unsigned long numtests = 0;

void dotest(str_view svin) {
    numtests += 1;
    auto s1 = netstring(svin);
    DIAG(_ut, " nstr : " << s1 << "\n");
    std::stringstream ss;
    sput_netstring(ss, svin);
    auto s2 = ss.str();
    str_view svs2;
    auto end_of_s2 = svscan_netstring(s2, &svs2, 0);
    EQUAL(svs2, svin);         // check that svscan_netstring "works"
    EQUAL(end_of_s2, s2.size()); // and that it points to the end of s2
    DIAG(_ut, "sput : " << s2 << "\n");
    EQSTR (s1, s2); // netstring and sput_netstring do the same thing
    ss.seekg(0);
    string s3;
    CHECK(sget_netstring(ss, &s3)); // sget_netstring returns true
    EQUAL (s3, svin);               // and it matches the input.
    sput_netstring(ss, svin);  // append another copy of svin to ss
    ss << "\n  ";              // followed by whitespace
    sput_netstring(ss, svin);  // followed by another copy
    ss << "  \n";              // followed by whitespace
    ss.seekg(0);               // reset the ss read pointer
    string ssstr = ss.str();   // all of ss, copiled out to a string
    str_view sssv(ssstr);      // a string_view into ssstr
    int ncopies = 0;
    string s4;
    size_t next = 0;
    while(sget_netstring(ss, &s4)){
        EQUAL(s4, svin);       // found another copy of svin in ss.
        str_view svs4;

        // Let's try again with the scan_netstring with skip_white=false
        // That should fail.
        bool caught;
        try{
            svscan_netstring<false>(sssv, &svs4, next);
        }catch(std::exception&){
            caught = true;
        }
        CHECK(caught);
        // But if we skip over the whitespace, it should succeed:
        auto nn = core123::svscan(sssv, nullptr, next);
        nn = svscan_netstring<false>(sssv, &svs4, nn);
        EQUAL(svs4, svin);
        
        // And find it again, but with svscan_netstring
        next = svscan_netstring(sssv, &svs4, next);
        EQUAL(svs4, svin);
        EQUAL(nn, next);

        // And one more time, but with 
        ncopies += 1;
    }
    EQUAL(ncopies, 3); // we found three copies of svin in ss.
}

void expect_ok(str_view nsin, size_t expected_offset) try {
    str_view payloadsv;
    auto offset = svscan_netstring(nsin, &payloadsv, 0);
    EQUAL(offset, expected_offset);
    core123::isvstream svs(nsin);
    string payloadstr;
    CHECK(sget_netstring(svs, &payloadstr));
    EQUAL(expected_offset, size_t(svs.tellg()));
 }catch(std::exception& e){
    core123::complain(e, "expect_ok(\"" + std::string(nsin) + "\") threw exception:");
    CHECK(false);
 }

void expect_error(str_view nsin) try {
    bool caught = false;
    try{
        str_view payloadsv;
        svscan_netstring(nsin, &payloadsv, 0);
    }catch(std::exception&){
        caught = true;
    }
    CHECK(caught);

    core123::isvstream svs(nsin);
    std::string payloadstr(512, 'x'); // big enough to not fit in SSO
    try{
        sget_netstring(svs, &payloadstr);
    }catch(std::exception&){
        caught = true;
    }
    // check that we both threw an exception and set the failbit
    // and left the payload with the capacity of a default string.
    CHECK(caught);
    CHECK(!svs);
    CHECK(payloadstr.empty());
    EQUAL(payloadstr.capacity(), std::string().capacity());
 }catch(std::exception& e){
    core123::complain(e, "expect_error(\"" + std::string(nsin) + "\") threw an *unexpected* exception:");
    CHECK(false);
 }
    
void test_corner_cases(){
    // Test a few cases that should work:
    expect_ok("3:,,,,,,,,,", 6);
    expect_ok("   3:,,,,,,,,,", 9);
    expect_ok("3:\0\0\0,   "s, 6);  // note the 's'.  It's a C++ string literal with NULs.
    expect_ok("\t3:abc,XTRA", 7); // and various kinds of whitespace.
    expect_ok("\r3:abc,", 7); // and various kinds of whitespace.
    expect_ok("\t\r\n 3:abc,JUNK", 10); // and various kinds of whitespace.
    expect_ok("\f 3:abc,\0\0\0"s, 8); // and various kinds of whitespace.
    
    // Test that svscan_netstring and sget_netstring properly handle
    // a selection of contrived error conditions and corner cases.
    expect_error("3:1b,");    // too short
    expect_error("3:1b,  ");  // too short
    expect_error("  3:1b,  ");  // too short
    expect_error("3:abcd,"); // wrong length
    expect_error("3:abc");   // premature EOF
    expect_error("99:abcdefghijk,"); // premature EOF
    expect_error("999999999:abcdefg,"); // will allocate ~1GB, but then discard before throwing.
    expect_error("4000000000:abcdefg"); // should not even try to allocate memory.  Check with valgrind.
    expect_error("3,abc,"); // missing colon
    expect_error("-1:abc,");  // minus sign. Nope.
    expect_error("-0:,");     // minus sign. Nope.
    expect_error("+0:,");     // plus sign. Nope.
    expect_error("abc3:abc,"); // not a number
    expect_error("3abc:abc,"); // not a number
    expect_error("\03:abc,"); // NUL isn't a number either.
    expect_error("3\0:abc,"); // even when it comes after a number.
    expect_error("1\00:1234567890,"); // or in the middle
    expect_error("0x3:abc,"); // hex isn't a number either.
    expect_error("03:abc,");  // leading zeros aren't allowed either
    expect_error("0000000003:abc,");  // even a lot of leading zeros
    expect_error("00:,");       // even "just" leading zeros
    expect_error("010:1234567890,");  // but leading zeros DO NOT make it octal!
}

int
main(int, char **)
{
    const size_t bufsz = 200;
    // It's not clear that we learn much from calling do_test a
    // million times.  But until something better comes along, it only
    // takes a few seconds...
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
    
    test_corner_cases();

    dotest("");
    dotest("hello");
    dotest(std::string(12345, 'x'));
    for(auto i = 0ul; i < ntests; i++) {
        size_t n = random() % 200;
        for (size_t j = 0; j < n; j++) {
            buf[j] = static_cast<char>((random() % 95) + 32);
        }
        auto sbuf = string(buf, n);
        DIAG(_ut, i << " buf \"" << sbuf << "\"");
        dotest(str_view(buf, n));
    }
    return utstatus();
}

