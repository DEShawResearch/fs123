// Test program for ConfigParser (ini file reader)
// reads one or more ini files into a ConfigParser object and then
// dumps the values to stdout in a simple format (can be compared
// to the output of testconfigparser.py)

// Mark Moraes, D. E. Shaw Research

#include "core123/configparser.hpp"
#include "core123/ut.hpp"
#include <algorithm>
#include <iostream>
#include <cctype>
#include <core123/envto.hpp>
#include <core123/streamutils.hpp>
#include <core123/diag.hpp>

using namespace std;
using namespace core123;

static auto _main = diag_name("main");

#define ue urlescape

int main(int argc, char **argv) try {
    if (argc < 2 || argv[1][0] == '-') {
        cerr << "Usage: " << argv[0] << " INI_FILENAME ..." << endl;
        exit(1);
    }
    vector<string> filenames{&argv[1], &argv[argc]};
    auto elines = core123::envto<bool>("TESTCFP_ELINES", true);
    auto summary = core123::envto<bool>("TESTCFP_SUMMARY", false);

    // first test raw config parser (no python-style interpolation of values returned by get())
    RawConfigParser<> rcfp;
    if (rcfp.read(filenames) == 0) {
        cerr << "could not read some file from [ " << core123::insbe(", ", filenames) << " ]" << endl;
        exit(1);
    }
    for (const auto& s: rcfp.get_section_names()) {
        DIAG(_main, "section " << s);
        auto sectp = rcfp.at(s);
        for (const auto& n: *sectp) {
            DIAG(_main, "option " << ue(n.first) << " = " << ue(n.second));
            EQSTR (rcfp.get(sectp, n.first), n.second);
            string k = n.first;
            transform(k.begin(), k.end(), k.begin(), ::tolower);
            DIAG(_main, "checking " << k);
            EQSTR (rcfp.get(sectp, k), n.second);
            transform(k.begin(), k.end(), k.begin(), ::toupper);
            DIAG(_main, "checking " << k);
            EQSTR (rcfp.get(sectp, k), n.second);
        }
    }

    // now print the interpolated values (for comparison with testconfigparser.py)
    ConfigParser<> cfp;
    cfp.empty_lines_in_values_ = elines;
    if (cfp.read(filenames) == 0) {
        cerr << "could not read some file from [ " << core123::insbe(", ", filenames) << " ]" << endl;
        exit(1);
    }
    for (const auto& s: cfp) {
        if (!summary) cout << ue(s.first) << "\n";
        auto indent = "    ";
        for (const auto& n: *(s.second)) {
            string k = n.first;
            auto v1 = cfp.get(s.second, n.first);
            transform(k.begin(), k.end(), k.begin(), ::tolower);
            DIAG(_main, "checking " << k << " " << v1);
            if (n.second.find_first_of('%') == string::npos) {
                EQUAL (v1, n.second);
            }
            if (!summary) cout << indent << ue(k) << "=" << ue(v1) << "\n";
            auto v2 = cfp.get(s.second, k);
            DIAG(_main, "checking " << k << " " << v2);
            EQSTR (v2, v1);
            transform(k.begin(), k.end(), k.begin(), ::toupper);
            auto v3 = cfp.get(s.second, k);
            DIAG(_main, "checking " << k << " " << v3);
            EQSTR (v3, v1);
        }
    }
    return utstatus(summary);
} catch (exception& e) {
    cerr << "Error: " << e.what() << endl;
    return 1;
}
