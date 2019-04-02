#include "core123/diag.hpp"
#include "core123/strutils.hpp"
#include <iostream>
#include <sys/time.h>
#include <string>
#include <sstream>
#include <cstdio>

using core123::diag_name;
using core123::get_diag_names;
using core123::set_diag_names;
using core123::get_diag_opts;
using core123::set_diag_destination;
using core123::diag_opt_tstamp;

auto _file = diag_name(__FILE__);

static struct timeval start;

inline void begin_timer(){
    gettimeofday(&start, 0);
}

inline void end_timer(int N, const char *msg){
    struct timeval end;
    gettimeofday(&end, 0);
    double duration = (end.tv_sec - start.tv_sec) + 1.e-6*(end.tv_usec-start.tv_usec);
    printf("%s: %.1f ns per invocation\n",
           msg, duration*1.e9/N);
}

int main(int argc, char **argv){
    const int N=10000000;
    int i;
    std::ostringstream oss;
    std::string off_s("OFF");
    printf("All loops run %d times\n", N);
    DIAG(1, "starting test: names \"" << get_diag_names(true)
	 << "\" opts \"" << get_diag_opts());
    DIAG(0, "should only appear in flood");

    // First, let's try plain old DIAG with no initialization
    set_diag_destination("%none");
    i=N;
    begin_timer();
    while(i--){
        DIAG(_file, "Hello world");
    }
    end_timer(N, "DIAG (unattached)");

    // What about if we use the allegedly 'ultrafast' macros:
    auto OFF = diag_name("OFF");
    i=N;
    begin_timer();
    while(i--){
        DIAG(OFF, "Hello world");
    }
    end_timer(N, "DIAGkey (unattached)");

    // Now let's attach a file to d
    set_diag_destination("%none");
    i=N;
    begin_timer();
    while(i--){
        DIAG(OFF, "Hello world");
    }
    end_timer(N, "DIAGkey (attached, but no keys): ");

    // What happens if there are other keys "on"
    for(int i=0; i<100; ++i){
        set_diag_names(core123::fmt("KEY%d", i));
    }
    i=N;
    begin_timer();
    while(i--){
        DIAG(_file, "Hello world");
    }
    end_timer(N, "DIAG (attached, with 100 keys): ");

    for(int i=0; i<100; ++i){
        set_diag_names(core123::fmt("KEY%d", i));
    }
    // Even more keys?
    for(int i=0; i<10000; ++i){
        set_diag_names(core123::fmt("KEY%d", i));
    }
    i=N;
    begin_timer();
    while(i--){
        DIAG(_file, "Hello world");
    }
    end_timer(N, "DIAG (attached, with 10k keys): ");

    i=N;
    begin_timer();
    while(i--){
        DIAGf(_file, "Hello world");
    }
    end_timer(N, "DIAGf (attached, with 10k keys): ");

    // And now let's turn on the printing
    set_diag_names(__FILE__);
    i=N;
    begin_timer();
    while(i--){
        DIAG(_file, "\n");
    }
    end_timer(N, "DIAGd with just FILE:LINE to a ostringstream: ");
    printf("Wrote %zd bytes to an ostringstream\n", oss.str().length());

    diag_opt_tstamp = true;
    i=N/10;
    begin_timer();
    while(i--){
        DIAG(_file, "\n");
    }
    end_timer(N/10, "DIAGd with just TIMESTAMP FILE:LINE to a ostringstream: ");
    printf("Wrote %zd bytes to an ostringstream\n", oss.str().length());

    // Where does the time go?
    printf("\nHow long does it take to create and/or look up a key?\n");
    i=N;
    begin_timer();
    while(i--){
        diag_name(off_s);
    }
    end_timer(N, "calls to diag_std().get_key(string)");

    i=N;
    begin_timer();
    while(i--){
        diag_name("OFF");
    }
    end_timer(N, "calls to diag_std().get_key(char*)");

    return 0;
}
