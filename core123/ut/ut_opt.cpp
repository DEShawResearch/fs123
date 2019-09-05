#include <string>
#include <cinttypes>
#include <core123/diag.hpp>
#include <core123/opt.hpp>

using namespace std;
using namespace core123;

uint32_t debug = 0;
static uint32_t testu32, ntestu32;
static uint64_t testu64, ntestu64;
static string testpath1, testpath2;
static double testdbl;

void printopts(OptionParser &op) {
    printf("debug = %" PRIu32 "  \"%s\"\n", debug, op.at("debug").valstr.c_str());
    printf("u32 = %" PRIu32 " \"%s\"\n", testu32, op.at("u32").valstr.c_str());
    printf("u32n = %" PRIu32 " \"%s\"\n", ntestu32, op.at("u32n").valstr.c_str());
    printf("path1 = \"%s\" \"%s\"\n", testpath1.c_str(), op.at("path1").valstr.c_str());
    printf("u64 = %" PRIu64 " \"%s\"\n", testu64, op.at("u64").valstr.c_str());
    printf("u64n = %" PRIu64 " \"%s\"\n", ntestu64, op.at("u64n").valstr.c_str());
    printf("path2 = \"%s\" \"%s\"\n", testpath2.c_str(), op.at("path2").valstr.c_str());
    printf("dbl = \"%g\" \"%s\"\n", testdbl, op.at("dbl").valstr.c_str());
    fflush(stdout);
}

#define TEST_PREFIX "TESTOPT_"

int main(int argc, char **argv)
{
    OptionParser op2;
    string diagon;
    op2.add_options({
	{"debug", OptionType::ou32, "0", &debug, "turns on debug"},
	{"u32", OptionType::ou32, "0", &testu32, "set a 32bit unsigned"},
	{"path1", OptionType::ostr, "", &testpath1, "set a string"},
	{"u64", OptionType::ou64, "0xffffffffffffffff", &testu64, "set a 64bit unsigned"},
	{"path2", OptionType::ostr, "", &testpath2, "set another string"},
	{"u32n", OptionType::ou32, "0xdeadbeef", &ntestu32, "set another 32bit unsigned"},
	{"u64n", OptionType::ou64, "0x123456789abcdef0", &ntestu64, "set another 64bit unsigned"},
	{"dbl", OptionType::odbl, "0.1", &testdbl, "set a double"},
    });
    string helpmsg("Options for testopt (use uppercase name prefixed with " TEST_PREFIX " as env var names):\n");
    printf("%s---\n", op2.helptext(&helpmsg));
    printopts(op2);
    auto optind = op2.setopts_from_argv(argc, (const char **) argv);
    printf("--- after argv\n");
    printopts(op2);
    set_diag_destination("%stderr");
    set_diag_names("opt:1");
    op2.setopts_from_env(TEST_PREFIX);
    printf("--- after env\n");
    printopts(op2);
    printf("--- optind %d argc %d\n", optind, argc);
    for (auto i = optind; i < argc; i++) {
	printf("%d: %s\n", i, argv[i]);
    }
    return 0;
}
