#include <core123/histogram.hpp>

#include <iostream>
#include <numeric>
#include <fenv.h>
#include <sstream>
#include <cstdio>
#include <iterator>
using core123::histogram;
using core123::uniform_histogram;

/*
struct addCounts{
    long operator()(const hbin &a, const hbin &b){
        return a.count() + b.count();
    }
    long operator()(const long& a, const hbin &b){
        return a + b.count();
    }
};
*/

void contains(const histogram &h, double x){
    auto p = h.find(x);
    std::cout << "[ " << h.bottom(p) << ", " << h.top(p) << " ) bidx=" << p << " contains " << x << " with count: " << h.count(p) << "\n";
}

void
checkCorners(const uniform_histogram& h){
    // MANY OF THESE FAIL DUE TO ROUNDOFF.  IS THERE ANY
    // FAST WAY AROUND THIS??
    for(auto p=h.underflow_bindex(); p<=h.overflow_bindex(); ++p){
        double top = h.top(p);
        double bottom = h.bottom(p);

        printf("Checking bounds on bin [%a, %a)\n", h.bottom(p), h.top(p));
        if(!isinf(top)){
            auto q = h.find(top);
            if(!(p<q)){
                printf("Failed p<h.find(%a)\n", top);
            }

            double topminus = nextafter(top, -INFINITY);
            if(!(h.find(topminus) == p)){
                printf("Failed p == h.find(%a)\n", topminus);
            }
        }
        if(!isinf(bottom)){
            auto q = h.find(bottom);
            if(!(p==q)){
                printf("Failed p == h.find(%a)\n", bottom);
            }
            double bottomminus = nextafter(bottom, -INFINITY);
            q = h.find(bottomminus);
            if(!(q < p)){
                printf("Failed h.find(%a) < p\n", bottomminus);
            }
                
        }
    }
}

void print_hist(histogram& h){
    for(auto p=h.underflow_bindex(); p<=h.overflow_bindex(); ++p){
        std::cout << "[ " << h.bottom(p) << " " << h.top(p) << " ) " << h.count(p) << "\n";
    }
}

int main(int argc, char **argv){
    uniform_histogram h(-5., 5., 10);

    std::cout.precision(17);
    std::cout << "Empty histogram:\n";
    print_hist(h);

    h.insert(3.14);
    h.insert(2.718);
    h.insert(0.0);
    h.insert(5.);
    h.insert(-5.);
    h.insert(0.);
    h.insert(2.5);

    histogram::value_type sum = 0;
    for(auto b=h.underflow_bindex(); b<=h.overflow_bindex(); ++b)
        sum += h.count(b);

    std::cout << "Full histogram: #entries: " << sum << "\n";
    print_hist(h);
    
    // Look at serialization:
#if 0
    std::stringstream oss;
    boost::archive::binary_oarchive oa(oss);
    oa << const_cast<const uniform_histogram&>(h);
    boost::archive::binary_iarchive ia(oss);
    std::cout << "The serialized histogram has size: " << oss.str().size() << "\n";
    uniform_histogram hcopy;
    ia >> hcopy;
    
    std::cout << "Full histogram after ser/des: #entries: " << sum << "\n";
    print_hist(h);
#endif
    

    long moment0 = 0;
    double moment1 = 0.;
    double moment2 = 0.;
    long nunbounded = 0;
    for(auto p=h.underflow_bindex() ; p<=h.overflow_bindex(); ++p){
        if(h.finiteRange(p)){
            double midpt = 0.5*(h.top(p) + h.bottom(p));
            moment0 += h.count(p);
            moment1 += h.count(p) * midpt;
            moment2 += h.count(p) * midpt*midpt;
        }else{
            nunbounded += h.count(p);
        }
    }
    std::cout << "moments of the distribution: " << moment0 << " " << moment1 << " " << moment2 << "\n";

    contains(h, 3.0);
    contains(h, 5.0);
    contains(h, 11.0);
    contains(h, 3.00000001);
    contains(h, -5.1);
    contains(h, -4.5);

    std::cout << "Checking corner cases with 'normal' round-to-nearest.  FAILURES ARE EXPECTED!\n";
    checkCorners(h);

    std::cout << "Checking corner cases with FE_DOWNWARD rounding.  NO FAILURES ARE EXPECTED\n";
    fesetround(FE_DOWNWARD);
    checkCorners(h);

    return 0;
}
