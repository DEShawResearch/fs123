#include <core123/histogram.hpp>
#include <iostream>
#include <numeric>
#include <fenv.h>
#include <cmath>
#include <cstdio>

using core123::histogram;
using core123::uniform_histogram;

void contains(const histogram &h, double x){
    auto p = h.find(x);
    std::cout << "[ " << h.bottom(p) << ", " << h.top(p) << " ) bidx=" << p << " contains " << x << " with count: " << h.count(p) << "\n";
}

void
checkCorners(const histogram& h){
    // MANY OF THESE FAIL DUE TO ROUNDOFF WITH DEFAULT ROUNDING.
    // They don't fail if the rounding mode is set to FE_DOWNWARD
    for(auto p=h.underflow_bindex(); p<=h.overflow_bindex(); ++p){
        double top = h.top(p);
        double bottom = h.bottom(p);

        printf("Checking bounds on bin [%a, %a)\n", h.bottom(p), h.top(p));
        if(!isinf(top)){
            auto q = h.find(top);
            if(!(p<q)){
                printf("Failed p<h.find(%a)\n", top);
            }

#if 0
            // For the logarthmic histogram we can't expect that
            // lg(x-ulp) < lg(x), so don't do this test.
            double topminus = nextafter(top, -INFINITY);
            if(!(h.find(topminus) == p)){
                printf("Failed p == h.find(%a)\n", topminus);
            }
#endif
        }
        if(!isinf(bottom)){
            auto q = h.find(bottom);
            if(!(p==q)){
                printf("Failed p == h.find(%a)\n", bottom);
            }
#if 0
            // For the logarthmic histogram we can't expect that
            // lg(x+ulp) > lg(x), so don't do this test.
            double bottomminus = nextafter(bottom, -INFINITY);
            q = h.find(bottomminus);
            if(!(q < p)){
                printf("Failed h.find(%a) < p\n", bottomminus);
            }
#endif                
        }
    }
}

void print_hist(histogram& h){
    for(auto p=h.underflow_bindex(); p<=h.overflow_bindex(); ++p){
        std::cout << "[ " << h.bottom(p) << " " << h.top(p) << " ) " << h.count(p) << "\n";
    }
}

int main(int argc, char **argv){

    uniform_histogram h(1.e-3, 1.e7, 10, log, exp);
    std::cout.precision(17);
    std::cout << "Empty histogram:\n";
    print_hist(h);

    h.insert(3.14);
    h.insert(2.718);
    h.insert(0.0);
    h.insert(5.);
    try{
        h.insert(-5.);
    }catch(std::range_error& e){
        std::cout << "Caught an expected range_error from h.insert(-5.)\n";
    }

        
    h.insert(0.);
    h.insert(2.5);

    histogram::value_type sum = 0;
    for(auto b=h.underflow_bindex(); b<=h.overflow_bindex(); ++b)
        sum += h.count(b);

    std::cout << "Full histogram: #entries: " << sum << "\n";
    print_hist(h);
    
    long moment0 = 0;
    double moment1 = 0.;
    double moment2 = 0.;
    for(auto p=h.first_bindex(); p<=h.last_bindex(); ++p){
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

    std::cout << "Checking corner cases with 'normal' round-to-nearest.  FAILURES ARE EXPECTED\n";
    checkCorners(h);

#if 0 // setting the rounding mode breaks things very badly.
    std::cout << "Checking corner cases with FE_DOWNWARD rounding.  NO FAILURES ARE EXPECTED\n";
    fesetround(FE_DOWNWARD);
    checkCorners(h);
#endif

    return 0;
}
