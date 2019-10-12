#include <core123/opt.hpp>
#include <core123/diag.hpp>
#include <core123/complaints.hpp>
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
int foo_bar;
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
    prtopt(foo_bar);
    //prtopt(help);
}

#define TEST_PREFIX "TESTOPT_"

int main(int argc, char *argv[])
{
    bool help = false;
    option_parser op2;
    op2.add_option("help", "Produce this message", opt_true_setter(help));
    op2.add_option("debug", "0", "turns on debug", opt_setter(debug));
    op2.add_option("u32",  "0", "set a 32bit unsigned", opt_setter(u32));
    op2.add_option("path1",  "", "set a string", opt_setter(path1));
    op2.add_option("u64", "0xffffffffffffffff", "set a 64bit unsigned", opt_setter(u64));
    op2.add_option("path2", "", "set another string", opt_setter(path2));
    op2.add_option("u32n", "0xdeadbeef", "set another 32bit unsigned", opt_setter(u32n));
    op2.add_option("u64n", "0x123456789abcdef0", "set another 64bit unsigned", opt_setter(u64n));
    op2.add_option("dbl", "0.1", "set a double", opt_setter(dbl));
    op2.add_option("foo-bar", "77", "set an int", opt_setter(foo_bar));

    std::vector<std::string> leftover;
    try{
        leftover = op2.setopts_from_argv(argc, argv);
        if(help){
            std::cerr << "You asked for help on the command line:\n" << op2.helptext() << "\n";
            return 0;
        }
        printf("--- after argv\n");
        printopts();
    }catch(option_error& oe){
        complain(oe, "setopts_from_argv:");
    }
    try{
        op2.setopts_from_env(TEST_PREFIX);
        printf("--- after env\n");
        printopts();
    }catch(option_error& oe){
        complain(oe, "setopts_from_env:");
    }        
    int i=0;
    printf("--- leftover arguments\n");
    for (auto& e : leftover){
	printf("%d: %s\n", i++, e.c_str());
    }
    return 0;
}
