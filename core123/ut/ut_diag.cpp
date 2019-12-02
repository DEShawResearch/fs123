//
// Unit Test of diagging facility
//
// $Revision$
//
// Authors: Federico D. Sacerdoti 2007

#include "core123/diag.hpp"
#include <iostream>
#include <fstream>
#include <cstring>

using namespace std;
using core123::the_diag;
using core123::diag_name;
using core123::get_diag_names;
using core123::set_diag_names;

auto _file = diag_name(__FILE__);

// Lots of blank lines.  Feel free to delete these to keep
// line numbering the same when/if you add lines to the actual
// code












#define lev 99 /* try to tickle namespace collisions in macros. */
auto keyNet = diag_name("Net");
auto keyNL = diag_name("NL");
auto keyMisc = diag_name("Misc");

int opt_debug = 0;
std::string opt_diag = "";
bool opt_blow_assert = false;
std::string opt_log = "ut_newdiag.log";

const char * f(){ DIAGf(_file, "diagnostics within diagnostics!\n"); return "function with diagnostics"; }
int main (int argc, char* argv[]) {
    std::cerr << "At top of main:  diag::get_names(): " << get_diag_names() << std::endl;
    try {
        // This isn't nearly as "safe" and "clean" as boost program_options.
        // But we don't need boost!!
        for(int i=1; i<argc; ++i){
            const char* v = argv[i];
            if      (strcmp(v, "--help")==0 || strcmp(v, "-h")==0){
                std::cout << "ut_diag: " << "\n";
                std::cout << "Known keys: " << get_diag_names() << "\n";
                return 0;
            }else if(strcmp(v, "--debug")==0 || strcmp(v, "-d")==0){
                opt_debug = std::stoi(argv[++i]);
                std::cout << "debug level set to " << opt_debug << "\n";
            }else if(strcmp(v, "--diag")==0 || strcmp(v, "-D")==0){
                opt_diag = argv[++i];
            }else if(strcmp(v, "--blow-assert")==0 || strcmp(v, "-a")==0){
                opt_blow_assert = true;
            }else if(strcmp(v, "--log")==0 || strcmp(v, "-l")==0){
                opt_log = argv[++i];
            }
        }
    } catch (exception& e) {
        cerr << "option parsing error: " << e.what() << "\n";
        return 2;
    }
    ofstream os;
    the_diag().diag_intermediate_stream.precision(17);
    if (!opt_diag.empty()){
        std::cerr  << "opt_diag: " << opt_diag << "\n";
        set_diag_names(opt_diag);
    }else{
        set_diag_names(__FILE__, false);
        set_diag_names("NET=1", false);
    }
    
    std::cerr << "After command line options, diag::get_names(): " << get_diag_names() << std::endl;
    keyMisc += opt_debug;

    DIAG(keyNet, "Good day Mr Net\n");
    double x = 0.1234567890123456789;
    DIAG(keyMisc, "Expect to see 17 digits: " << x << "\n");
    DIAG(keyNL, "Hi NL its " << 40 << " degrees outside.  This string does not have a newline");
    DIAG(keyNL, "Hi NL.   This string does have a newline\n");
    DIAG(keyMisc, "Hi misc\n");
    DIAG(keyMisc>=2, "Up a level Misc\n");
    DIAG(keyMisc>=3, "Up a level again Misc\n"); 

    DIAGf(_file, "Straight, no chaser\n");
    
    cout << "\n Keys are: \n"<< get_diag_names(false) << "\n";
    cout << "\n Keys are: \n"<< get_diag_names(true) << "\n";
    DIAGf(_file, "lev=%d %s\n", lev, f());
}
