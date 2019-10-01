#include <core123/opt.hpp>
#include <core123/diag.hpp>
#include <string>
#include <cinttypes>
#include <fstream>

using namespace std;
using namespace core123;

string diagon;
bool debug;
uint32_t u32, u32n;
uint64_t u64, u64n;
double dbl;
std::string path1, path2;
//bool help;

void printopts() {
#define prtopt(name) std::cout << #name << " = " << name << std::endl;    
    prtopt(debug);
    prtopt(u32);
    prtopt(path1);
    prtopt(u64);
    prtopt(path2);
    prtopt(u32n);
    prtopt(u64n);
    prtopt(dbl);
    //prtopt(help);
}

#define TEST_PREFIX "TESTOPT_"

int main(int argc, char **argv)
{
    option_parser op2("myprog - a program for doing my stuff\nUsage:  myprog [options] foo bar\nOptions:\n");
    op2.add_option("debug", "0", "turns on debug", opt_setter(debug));
    op2.add_option("u32",  "0", "set a 32bit unsigned", opt_setter(u32));
    op2.add_option("path1",  "", "set a string", opt_setter(path1));
    op2.add_option("u64", "0xffffffffffffffff", "set a 64bit unsigned", opt_setter(u64));
    op2.add_option("path2", "", "set another string", opt_setter(path2));
    op2.add_option("u32n", "0xdeadbeef", "set another 32bit unsigned", opt_setter(u32n));
    op2.add_option("u64n", "0x123456789abcdef0", "set another 64bit unsigned", opt_setter(u64n));
    op2.add_option("dbl", "0.1", "set a double", opt_setter(dbl));
    //op2.del_option("help");
    //op2.add_option("help", "print help", opt_true_setter(help));

    auto optind = op2.setopts_from_argv(argc, (const char **) argv);
    printf("--- after argv\n");
    printopts();
    set_diag_destination("%stderr");
    set_diag_names("opt:1");
    op2.setopts_from_env(TEST_PREFIX);
    printf("--- after env\n");
    printopts();
    printf("--- optind %d argc %d\n", optind, argc);
    for (auto i = optind; i < argc; i++) {
	printf("%d: %s\n", i, argv[i]);
    }
    return 0;
}
