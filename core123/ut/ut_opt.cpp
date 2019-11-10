#include <core123/opt.hpp>
#include <core123/ut.hpp>
#include <core123/diag.hpp>
#include <core123/complaints.hpp>
#include <string>
#include <cinttypes>
#include <fstream>

using namespace std;
using namespace core123;

namespace {

const auto _main = diag_name("main");

string diagon;
bool debug;
uint32_t u32;
uint64_t u64;
double dbl;
std::string path1, path2;
int vs;
//bool help;

void printopts() {
#define prtopt(name) std::cout << #name << " = " << name << std::endl;    
    prtopt(debug);
    prtopt(u32);
    prtopt(path1);
    prtopt(u64);
    prtopt(path2);
    prtopt(dbl);
    prtopt(vs);
    //prtopt(help);
}

#define TEST_PREFIX "TESTOPT_"
} // namespace <anon>

int main(int argc, char *argv[])
{
    bool help = false;
    option_parser op;
    
    string refhelp{"    flagfile= (default=) : read flags from the named file\n"};
    EQUAL(op.helptext(), refhelp);

    op.add_option("help", "Produce this message", opt_true_setter(help));
    refhelp += "    help : Produce this message\n";
    EQUAL(help, false);
    EQUAL(op.helptext(), refhelp);
    
    op.add_option("debug", "0", "turns on debug", opt_setter(debug));
    refhelp = "    debug=0 (default=0) : turns on debug\n" + refhelp;
    EQUAL(debug, 0);
    EQUAL(op.helptext(), refhelp);
    
    op.add_option("path1",  "/x", "set a string", opt_setter(path1));
    refhelp += "    path1=/x (default=/x) : set a string\n";
    EQUAL(path1, "/x");
    EQUAL(op.helptext(), refhelp);

    op.add_option("path2", "", "set another string", opt_setter(path2));
    refhelp += "    path2= (default=) : set another string\n";
    EQUAL(path2, "");
    EQUAL(op.helptext(), refhelp);

    op.add_option("u32",  "101", "set a 32bit unsigned", opt_setter(u32));
    refhelp += "    u32=101 (default=101) : set a 32bit unsigned\n";
    EQUAL(u32, 101);
    EQUAL(op.helptext(), refhelp);

    op.add_option("u64", "0xffffffffffffffff", "set a 64bit unsigned", opt_setter(u64));
    refhelp += "    u64=0xffffffffffffffff (default=0xffffffffffffffff) : set a 64bit unsigned\n";
    EQUAL(u64, 0xffffffffffffffff);
    EQUAL(op.helptext(), refhelp);

    op.add_option("dbl", "-3.14e-9", "set a double", opt_setter(dbl));
    refhelp = "    dbl=-3.14e-9 (default=-3.14e-9) : set a double\n" + refhelp;

    op.add_option("verify-something", "-795", "set an int", opt_setter(vs));
    refhelp += "    verify-something=-795 (default=-795) : set an int\n";
    EQUAL(vs, -795);
    EQUAL(u32, 101); // default unchanged
    EQUAL(path1, "/x"); // default unchanged
    EQUAL(op.helptext(), refhelp);

    const char *xv1[] = {"prognamexv1", "--u64=0xfeeeeeeeeeeeeeee", "--help"};
    std::vector<std::string> leftover;

    leftover = op.setopts_from_argv(sizeof(xv1)/sizeof(xv1[0]), (char **) xv1);
    if (_main) {
        DIAG(_main, "--- after xv1:\n");
        printopts();
        DIAG(_main, "--- leftover: " << strbe(leftover) << "\n--- \n");
    }
    EQUAL(help, true);
    EQUAL(leftover.size(), 0);
    EQUAL(u64, 0xfeeeeeeeeeeeeeee);
    EQUAL(u32, 101); // default
    EQUAL(path1, "/x"); // default

    const char *xv2[] = {"prognamexv2", "--verify-something=123", "foo1"};
    leftover = op.setopts_from_argv(sizeof(xv2)/sizeof(xv2[0]), (char **) xv2);
    if (_main) {
        DIAG(_main, "--- after xv2:\n");
        printopts();
        DIAG(_main, "--- leftover: " << strbe(leftover) << "\n--- \n");
    }
    EQUAL(help, true); // still true
    EQUAL(u64, 0xfeeeeeeeeeeeeeee); // value from xv1
    EQUAL(vs, 123);
    EQUAL(u32, 101); // default
    EQUAL(path1, "/x"); // default
    EQUAL(leftover.size(), 1);
    EQUAL(leftover[0], "foo1");

    const char *xv3[] = {"prognamexv3", "foo2", "", "bar2"};
    leftover = op.setopts_from_argv(sizeof(xv3)/sizeof(xv3[0]), (char **) xv3);
    if (_main) {
        DIAG(_main, "--- after xv3:\n");
        printopts();
        DIAG(_main, "--- leftover: " << strbe(leftover) << "\n--- \n");
    }
    EQUAL(help, true); // still true
    EQUAL(u64, 0xfeeeeeeeeeeeeeee); // value from xv1
    EQUAL(vs, 123); // value from xv2
    EQUAL(u32, 101); // default
    EQUAL(path1, "/x"); // default
    EQUAL(leftover.size(), 3);
    EQUAL(leftover[0], "foo2");
    EQUAL(leftover[1], "");
    EQUAL(leftover[2], "bar2");

    const char *xv4[] = {"prognamexv4", "--verify-something", "bar3"};
    bool got_expected_exception1 = false, should_not_reach1 = false;
    try{
        leftover = op.setopts_from_argv(sizeof(xv4)/sizeof(xv4[0]), (char **) xv4);
        CHECK(should_not_reach1);
    }catch(option_error& oe){
        got_expected_exception1 = true;
        CHECK(strcmp(oe.what(), "setopts_from_range: error while processing --verify-something bar3") == 0);
        if (_main) complain(oe, "setopts_from_xv4:");
    }
    if (_main) {
        DIAG(_main, "--- after xv4:\n");
        printopts();
        DIAG(_main, "--- leftover: " << strbe(leftover) << "\n--- \n");
    }
    CHECK(got_expected_exception1);

    const char *xv5[] = {"prognamexv5", "--help=10", "bleep"};
    bool got_expected_exception2 = false, should_not_reach2 = false;
    try{
        leftover = op.setopts_from_argv(sizeof(xv5)/sizeof(xv5[0]), (char **) xv5);
        CHECK(should_not_reach2);
    }catch(option_error& oe){
        got_expected_exception2 = true;
        CHECK(strcmp(oe.what(), "setopts_from_range: error while processing --help=10") == 0);
        if (_main) complain(oe, "setopts_from_xv5:");
    }
    if (_main) {
        DIAG(_main, "--- after xv5:\n");
        printopts();
        DIAG(_main, "--- leftover: " << strbe(leftover) << "\n--- \n");
    }
    CHECK(got_expected_exception2);

    const char *xv6[] = {"prognamexv6", "--u321=99", ""};
    leftover = op.setopts_from_argv(sizeof(xv6)/sizeof(xv6[0]), (char **) xv6);
    if (_main) {
        DIAG(_main, "--- after xv6:\n");
        printopts();
        DIAG(_main, "--- leftover: " << strbe(leftover) << "\n--- \n");
    }
    EQUAL(help, true); // still true
    EQUAL(u64, 0xfeeeeeeeeeeeeeee); // value from xv1
    EQUAL(vs, 123); // value from xv2
    EQUAL(u32, 101); // default
    EQUAL(path1, "/x"); // default
    EQUAL(leftover.size(), 2);
    EQUAL(leftover[0], "--u321=99");
    EQUAL(leftover[1], "");

    op.setopts_from_env(TEST_PREFIX);
    if (_main) {
        DIAG(_main, "--- after env:\n");
        printopts();
        DIAG(_main, "--- \n");
    }
    EQUAL(help, true); // still true
    EQUAL(u64, 0xfeeeeeeeeeeeeeee); // value from xv1
    EQUAL(vs, 123); // value from xv2
    EQUAL(u32, 101); // default
    EQUAL(path1, "/x"); // default

#define ENAME TEST_PREFIX "PATH1"
    string xv7env{ENAME "="};
    xv7env += "yz";
    putenv((char *)xv7env.c_str());
    DIAG(_main, ENAME << "=" << getenv(ENAME));
    op.setopts_from_env(TEST_PREFIX);
    if (_main) {
        DIAG(_main, "--- after putenv:\n");
        printopts();
        DIAG(_main, "--- \n");
    }
    EQUAL(help, true); // still true
    EQUAL(u64, 0xfeeeeeeeeeeeeeee); // value from xv1
    EQUAL(vs, 123); // value from xv2
    EQUAL(u32, 101); // default
    EQUAL(path1, "yz");
    EQUAL(path2, ""); //default

    bool got_expected_exception3 = false, should_not_reach3 = false;
#define UNAME TEST_PREFIX "U32"
    string xv8env{UNAME "="};
    xv8env += "yz";
    putenv((char *)xv8env.c_str());
    DIAG(_main, UNAME << "=" << getenv(UNAME));
    try{
        op.setopts_from_env(TEST_PREFIX);
        CHECK(should_not_reach3);
    }catch(option_error& oe){
        got_expected_exception3 = true;
        CHECK(strcmp(oe.what(), "option_error::set(u32, yz)") == 0);
        if (_main) complain(oe, "setopts_from_xv8:");
    }
    if (_main) {
        DIAG(_main, "--- after xv8:\n");
        printopts();
        DIAG(_main, "--- \n");
    }
    CHECK(got_expected_exception3);

    return utstatus();
}
