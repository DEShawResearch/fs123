#include <core123/histogram.hpp>
#include <core123/ut.hpp>
#include <cmath>
#include <iostream>
#include <numeric>
#include <fenv.h>
#include <cstdio>
#include <cassert>

using core123::histogram;
using core123::nonuniform_histogram;

void contains(const histogram &h, double x){
    auto p = h.find(x);
    std::cout << "[ " << h.bottom(p) << ", " << h.top(p) << " ) bidx=" << p << " contains " << x << " with count: " << h.count(p) << "\n";
}

void
checkCorners(const histogram& h){
    // These should all pass for a NonUniform histogram.
    for(auto p = h.first_bindex(); p<=h.last_bindex(); ++p){
        double x = h.bottom(p);
        printf("Checking bounds near cut: %a\n", x);
        auto q = h.find(x);
        EQUAL(q, p);
        q = h.find(nextafter(x, INFINITY));
        EQUAL(q, p);
        q = h.find(nextafter(x, -INFINITY));
        EQUAL(q+1, p);
        x = h.top(p);
        q = h.find(x);
        EQUAL(q, p+1);
        q = h.find(nextafter(x, -INFINITY));
        printf("q=h.find(nextafter(%a, -INFINITY)=%a\n", x, nextafter(x, -INFINITY));
        // N.B.  icpc fails this when x == 0.0.  Probably something to
        // do with rounding and extended-precision.  We don't use icc
        // in production, so there's little motivation to "fix" it.
        EQUAL(q, p);
    }
}

void tryinsert(histogram &h, double x){
    auto p = h.find(x);
    std::cout << "Before inserting: " << x << " : " << h.count(p) << "\n";
    h.insert(x);
    std::cout << "After inserting: " << x << " : " << h.count(p) << "\n";
}

void print_hist(histogram& h){
    for(auto p=h.underflow_bindex(); p<=h.overflow_bindex(); ++p){
        std::cout << "[ " << h.bottom(p) << " " << h.top(p) << " ) " << h.count(p) << "\n";
    }
}

int main(int argc, char **argv){

    double cuts[] = {-11., -4., -2., 0., 1.5, 2.6, 3.14};
    std::cout.precision(18);
    nonuniform_histogram h(&cuts[0], &cuts[sizeof(cuts)/sizeof(*cuts)]);
    std::cout << "Empty histogram:\n";
    print_hist(h);

    tryinsert(h, 3.14);
    tryinsert(h, 2.718);
    tryinsert(h, -4.);
    tryinsert(h, nextafter(-4., -INFINITY));
    tryinsert(h, nextafter(-4., INFINITY));
    tryinsert(h, 0.0);
    tryinsert(h, nextafter(0.0, -INFINITY));
    tryinsert(h, nextafter(0.0, INFINITY));
    tryinsert(h, 5.);
    tryinsert(h, -5.);
    tryinsert(h, 0.);
    tryinsert(h, 2.5);

    histogram::value_type sum = 0;
    for(auto b=h.underflow_bindex(); b<=h.overflow_bindex(); ++b)
        sum += h.count(b);

    std::cout << "Full histogram: #entries: " << sum << "\n";
    print_hist(h);
    
    long moment0 = 0;
    double moment1 = 0.;
    double moment2 = 0.;
    for(auto p=h.first_bindex() ; p<=h.last_bindex(); ++p){
        CHECK(h.finiteRange(p));
        double midpt = 0.5*(h.top(p) + h.bottom(p));
        auto c = h.count(p);
        moment0 += c;
        moment1 += c * midpt;
        moment2 += c * midpt*midpt;
    }
    std::cout << "moments of the distribution: " << moment0 << " " << moment1 << " " << moment2 << "\n";

    contains(h, 3.0);
    contains(h, 5.0);
    contains(h, 11.0);
    contains(h, 3.00000001);
    contains(h, -5.1);
    contains(h, -4.5);

    std::cout << "Checking corner cases with 'normal' round-to-nearest\n";
    checkCorners(h);

    auto ret = utstatus();
#if defined(__ICC)
    if(ret)
        fprintf(stderr, "Note:  icc with default float options misbehaves on bin boundaries near 0.\n");
#endif
    return ret;
}
