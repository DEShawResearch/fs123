/// Test code
#include "core123/scoped_timer.hpp"
#include "core123/datetimeutils.hpp"
#include <cstdio>
#include <cstdlib>

using namespace std::chrono;
using core123::scoped_timer;
using core123::tp2dbl;

#define K 1000
#define M (K*K)
#define CALCITER (100*M)    /* if you change this, change *_GOOD below as well */
/* http://en.wikipedia.org/wiki/Logistic_map */
/* Integer version, basically 16-bit fixed point version */
#define INT_LOGISTIC_MAP_INIT 19661
#define INT_LOGISTIC_MAP(x)  ((x)*(65536-(x))/18204)
#define INT_LOGISTIC_GOOD 22415	/* if CALCITER is 100*M */

void dotest(int imax) {
    unsigned long count = 0;
    scoped_timer::accum_type dt(0);
    int ix=0;
    // implicit accumulation of time for each loop iteration in dt
    // when t destructs
    for (int i = 0; i < imax; i++) {
	scoped_timer st(&dt);
	ix = INT_LOGISTIC_MAP_INIT;
	for (int k = 0; k < CALCITER; k++) {
	    ix = INT_LOGISTIC_MAP(ix);
	}
	count += CALCITER;
    }
    printf("%llu iter in %llu ns, %g iter/sec\n", (unsigned long long)count, (unsigned long long)dt.count(),
	   count/(std::chrono::duration<double>(dt).count()));
    if (ix != 22415) {
	fprintf(stderr, "ix %d != 22415\n", ix);
    }
    // test explicit accumulation by adding t.count() to dt
    for (int i = 0; i < imax; i++) {
	scoped_timer snt;
	ix = INT_LOGISTIC_MAP_INIT;
	for (int k = 0; k < CALCITER; k++) {
	    ix = INT_LOGISTIC_MAP(ix);
	}
	dt += snt.elapsed();
	count += CALCITER;
    }
    printf("%llu iter in %llu nsec, %g iter/sec\n", (unsigned long long)count, (unsigned long long)dt.count(),
	   count/(std::chrono::duration<double>(dt).count()));
}

#define TEST(v)  dotest(v)
int
main(int argc, char **argv)
{
    int imax = (argc > 1) ? atoi(argv[1]) : 1;

    TEST(imax);
    {
	scoped_timer t;
        int iix=0;
        printf("timer (using default steady_clock) started at: %.9f\n", tp2dbl(t.started_at()));
        core123::_scoped_timer<std::chrono::system_clock> tsys;
        printf("timer (using default system_clock) started at: %.9f\n", tp2dbl(tsys.started_at()));
        
	for (int i = 0; i < imax; i++) {
	    int ix = INT_LOGISTIC_MAP_INIT;
	    for (int k = 0; k < CALCITER; k++) {
		ix = INT_LOGISTIC_MAP(ix);
	    }
            iix = ix;
	}
        if (iix != 22415) {
            fprintf(stderr, "ix %d != 22415\n", iix);
        }
	auto dt = t.elapsed();
	printf("%d iter in %llu ticks, %g iter/sec\n", imax*CALCITER, (unsigned long long)dt.count(),
	       imax*CALCITER/(std::chrono::duration<double>(dt).count()));
    }
    return 0;
	
}
