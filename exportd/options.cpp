#include "options.hpp"
#include <core123/opt.hpp>
#include <core123/strutils.hpp>

void 
options::populate(int argc, char *argv[], int startidx){
    core123::option_parser parser;
#define OPTION(type, name, dflt, desc)       \
    parser.add_option(#name, core123::str(dflt), desc, core123::opt_setter(name));
#include "options.inc"
#undef OPTION
    parser.setopts_from_env("FS123P7_EXPORTD_");
    parser.setopts_from_argv(argc, argv, startidx);
}
    
options gopts;
