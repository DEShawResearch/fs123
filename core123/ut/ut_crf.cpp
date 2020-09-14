#include <core123/crf.hpp>
#include <core123/ut.hpp>
#include <core123/complaints.hpp>
#include <map>

using namespace std;
using core123::complain;
using core123::svscan_crf;
using core123::format_crf;
using core123::str_view;

// We really should have a "CHECK_THROW" in ut.hpp
#define CHECK_THROW(STMT) \
    do{                   \
    bool caught = false;       \
    try{                                        \
        STMT;              \
    }catch(std::exception& e){                                          \
    complain(e, "Correctly caught an exception from svscan_crf");       \
    caught = 1;                                                         \
    }                                                                   \
    CHECK(caught)                                                       \
    }while(0)

int main(int, char **) try {
    string crf =
        "+0,0:->\n"
        "+2,2:\0\1->\r\n\n"  // the key is NUL SOH.  the data is CR LF
        "+3,5:abc->55555\n"
        "+2,9:de->999999999\n"
        "+3,3:fgh->\0\0\0\n" // multiple NULs shouldn't cause any trouble
        "\n"s; // <-  Note the 's'.  This is a std::string literal!
        
    map<str_view, str_view> m;
    auto enmap = [&m](str_view k, str_view d){
                     m[k] = d;
                 };
    svscan_crf(crf, enmap, 0);
    CHECK(m.at("abc") == "55555");
    CHECK(m.at("de") == "999999999");
    CHECK(m.at("\0\1"s) == "\r\n");
    CHECK(m.at("") == "");
    CHECK(m.at("fgh") == "\0\0\0"s); 
    auto rt = format_crf(m.begin(), m.end());
    // N.B.  C++ maps have a deterministic, lexicographic order.  Since
    // the original 'ok' crf was ordered, it will round-trip perfectly.
    EQUAL(crf, format_crf(m));
  
    size_t goodlen = crf.size()-1;
    // Leading zeros on the lengths are ignored.
    // The numbers are still decimal.
    crf.resize(goodlen); m.clear();
    crf += "+010,00010:1234567890->0987654321\n\n";
    EQUAL(svscan_crf(crf, enmap, 0), crf.size());
    EQUAL(m.at("1234567890"), "0987654321");

    // Check for a variety of error conditions:
    // Start the scan at the wrong place:
    CHECK_THROW(svscan_crf(crf, enmap, 1));
    CHECK_THROW(svscan_crf(crf, enmap, 2));
    CHECK_THROW(svscan_crf(crf, enmap, 30));
    CHECK_THROW(svscan_crf(crf, enmap, 500));
    CHECK_THROW(svscan_crf(crf, enmap, 4000000000)); // Don't segfault!

    // Missing terminal newline:
    crf.resize(goodlen); m.clear();
    CHECK_THROW(svscan_crf(crf, enmap, 0));
    
    // Missing newline at end of record
    crf.resize(goodlen); m.clear();
    crf += "+3,3:ijk->123"  // missing newline
        "+3,2:lmn->22\n"
        "\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    // Mis-typed arrow in record:
    crf.resize(goodlen); m.clear();
    crf += "+3,3:ijk--123\n"
        "+3,2:lmn->22\n"
        "\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    // missing + at start of record
    crf.resize(goodlen); m.clear();
    crf += "3,3:ijk->123\n"
        "+3,2:lmn->22\n"
        "\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));
    
    // space instead of plus at start of record
    crf.resize(goodlen); m.clear();
    crf += " 3,3:ijk->123\n"
        "+3,2:lmn->22\n"
        "\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    // whitespace at end of record
    crf.resize(goodlen); m.clear();
    crf += " 3,3:ijk->123\n "
        "+3,2:lmn->22\n"
        "\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    // Extra newline terminates early:
    crf.resize(goodlen); m.clear();
    crf += "+3,3:ijk->123\n"
        "\n"
        "+3,2:lmn->22\n"
        "\n";
    auto next = svscan_crf(crf, enmap, 0);
    CHECK(m.count("lmn") == 0);
    // but we can keep reqding from where the first one left off.
    CHECK(svscan_crf(crf, enmap, next) == crf.size());
    CHECK(m.at("lmn") == "22")

    // klen is a little too big.
    crf.resize(goodlen); m.clear();
    crf += "+4,3:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));
    CHECK_THROW(svscan_crf(crf, enmap, goodlen));

    // klen is *way* too big.  Don't segfault.
    crf.resize(goodlen); m.clear();
    crf += "+4000000000,3:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));
    CHECK_THROW(svscan_crf(crf, enmap, goodlen));

    // klen starts with a spurious -minus sign
    crf.resize(goodlen); m.clear();
    crf += "+-3,3:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    // klen starts with a spurious +plus sign
    crf.resize(goodlen); m.clear();
    crf += "++3:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    // klen has a spurious x
    crf.resize(goodlen); m.clear();
    crf += "+0x3:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    // dlen is *way* too big.  Don't segfault!
    crf.resize(goodlen); m.clear();
    crf += "+3,3000000000:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));
    CHECK_THROW(svscan_crf(crf, enmap, goodlen));

    // dlen is *way* too big.  We shouldn't segfault.
    crf.resize(goodlen); m.clear();
    crf += "+3,3000000000:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));
    CHECK_THROW(svscan_crf(crf, enmap, goodlen));

    // dlen starts with a spurious -minus sign
    crf.resize(goodlen); m.clear();
    crf += "+3,-3:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    // dlen starts with a spurious +plus sign
    crf.resize(goodlen); m.clear();
    crf += "+3,+3:abc->def\n\n";
    CHECK_THROW(svscan_crf(crf, enmap, 0));

    return utstatus(true);
}catch (exception& e){
    complain(e, "FAIL:  error caught by main");
    return 1;
 }
