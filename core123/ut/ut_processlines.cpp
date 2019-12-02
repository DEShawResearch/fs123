// simple unit tests for processline, plus a cat clone
#include <core123/processlines.hpp>
#include <core123/ut.hpp>
#include <core123/strutils.hpp>
#include <core123/diag.hpp>
#include <sstream>
#include <fstream>

using namespace std;
using namespace core123;

namespace {
const auto _main = diag_name("main");
const auto _test = diag_name("test");
const auto _check = diag_name("check");
struct Test {
    std::string s;
    vector<str_view> svec;
    Test(str_view teststr) : s{teststr}, svec{svsplit_exact(s, "\n")} {
        DIAG(_test, "creating test \"" << quopri(teststr) << '"');
        // weird corner case: teststr being empty string "" means
        // a vector of size 1 with the empty string, and teststr
        // ending with newline means we have an empty string
        // as the last element of the vector so nuke that last
        // element in these two cases.
        if (s.size() == 0 || s[s.size()-1] == '\n') {
            DIAG(_test>1, "removing last element, s.size()=" << s.size());
            svec.pop_back();
        }
        if (_test) {
            DIAG(_test, svec.size() << " elements");
            for (auto i = 0u; i < svec.size(); i++)
                DIAG(_test>1, i << ": \"" << quopri(svec[i]) << '"');
        }
    }
    template<typename T> void check_one(T& inp, size_t bsize) {
        unsigned n = 0;
        processlines(inp, [&](str_view sv) {
            DIAG(_test>2, "got " << n << ':' << quopri(sv));
            EQUAL(svec[n], sv);
            n++;
        }, true, bsize);
        EQUAL(n, svec.size());
    }
    void check_all(size_t bsize) {
        DIAG(_check, "checking istream");
        istringstream iss(s);
        check_one(iss, bsize);
        auto f = ::tmpfile();
        if (f == nullptr)
            throw std::runtime_error("could not create tmpfile");
        sew::fwrite(s.data(), 1, s.size(), f);
        ::rewind(f);
        DIAG(_check, "checking FILE");
        check_one(f, bsize);
        ::rewind(f);
        DIAG(_check, "checking fd");
        auto fd = ::fileno(f);
        check_one(fd, bsize);
    }
};
} // namespace <anon>

int main(int argc, char **argv) try {
    if (argc > 1) {
        // if we have arguments, cat them
        auto catfunc = [](str_view sv) {
            cout << sv << '\n';
        };
        for (auto i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1] == '\0') {
                processlines(stdin, catfunc);
            } else if (argv[i][0] == '+' && argv[i][1] == '\0') {
                processlines(cin, catfunc);
            } else if (argv[i][0] == '=' && argv[i][1] == '\0') {
		auto fd0 = 0; // need lvalue
                processlines(fd0, catfunc);
            } else if (argv[i][0] == '-') {
                cerr << "Usage: " << argv[0] << " [FILENAME...]\nwhere FILENAME is - to use stdin, + to use cin, = to use fd 0, no FILENAMEs to run internal unit tests, or just cat the specified files.\n";
                exit(1);
            } else {
                ifstream inp(argv[i], ios::binary);
                processlines(inp, catfunc);
            }
        }
        return 0;
    }
    // without arguments, run internal test cases
    const char *tests[] = {"", "x", "xx", "x\ny", "xx\nyy", "x\nyy", "xx\ny",
	"x\ny\nz", "x\ny\nzz", "x\nyy\nz", "x\nyy\nzz",
	"xx\ny\nz", "xx\ny\nzz", "xx\nyy\nz", "xx\nyy\nzz",
	
    };
    // we iterate over all tests with different block sizes, and different
    // number of empty lines before and after to see if we
    // rattle any boundaries. 
    for (const auto& t: tests) {
        DIAG(_main, "testing base string \"" << quopri(t) << '"');
	for (auto bsize = 3u; bsize < 10u; bsize++) {
	    for (string pfx; pfx.size() < 3; pfx += '\n') {
		for (string sfx; sfx.size() < 3; sfx += '\n') {
		    DIAG(_main>1, "bsize=" << bsize << " pfxlen=" << pfx.size() << " sfxlen=" << sfx.size());
		    Test tx(sfx+t+pfx);
		    tx.check_all(bsize);
		}
	    }
        }
    }
    return utstatus(true);
} catch (exception& e) {
    cerr << "Error: " << e.what() << endl;
    return 1;
}
