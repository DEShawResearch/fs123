#pragma once

#include <vector>
#include <algorithm>
#include <cmath>                // for INFINITY
#include <stdexcept>

namespace core123{

/*
 DOCUMENTATION_BEGIN

histogram is an abstract class with two derived classes (see below):

  uniform_histogram
  nonuniform_histogram

Except for the constructors, all operations work identically on all
derived histograms.

The values stored in the histogram have type histogram::value_type,
typedef'ed to long.  This may become a template parameter in the future.

Items are inserted into a histogram with:

  bindex_t histogram::insert(double x, histogram::value_type incr=1);

which increments the count associated with the bin containing x by
incr.  The value returned is the index of the bin that was incremented.

The histogram is 'read' by member functions that return information about
a 'bindex' (bin index) argument:

  histogram h; 
  double h.bottom(bindex_t); // returns the lower limit of the given bindex
  double h.top(bindex)t;     // returns the upper limit of the given bindex
  value_type h.count(bindex);// returns the count in the given bindex
  bool finiteRange(bindex);  // returns whether the given bindex is bounded

All histograms have an underflow bin and an overflow bin (which are unbounded
on at least one side) as well as a first and last bin (both of which are bounded).

  bindex_t h.underflow_bindex();   // generally 0
  bindex_t h.first_bin();          // generally 1
  bindex_t h.last_bin();           // generally h.size()-2
  bindex_t h.overflow_bindex();    // generally h.size()-1

The caller may choose whether or not to include overflow and underflow
bins when looping, e.g.,

  for(auto b=h.underflow_bindex(); b<=h.overflow_bindex(); ++b) // include over/underflow
      std::cout << "[" <<  h.bottom(b), ", " << h.top(b) << ") " << h.count(b) << "\n";

  for(auto b=h.first_bindex(); b<=h.last_bindex(); ++b)  // exclude over/underflow
      std::cout << "[" <<  h.bottom(b), ", " << h.top(b) << ") " << h.count(b) << "\n";

Histograms can be added with operator+=, incrementing each of the bins
of *this by the amount in the same bin in the right-hand side.  

Finally, a histogram can be cleared with:

  void histogram::clear();

EXAMPLE:

uniform_histogram h(-10, 10, 100);

for(...){
   ...
   double x;
   h.insert(x);
   ...
}

histogram::const_iterator p;
// Estimate the first couple of moments of the distribution
// sampled by the histogram.
for(auto p=h.first_bindex(); p<=h.last_bindex(); ++p){
    double midpt = 0.5*(h.top(p) + h.bottom(p));
    auto c = h.count(p);
    moment0 += c;
    moment1 += c * midpt;
    moment2 += c * midpt*midpt;
}
auto nunbounded = h.count(h.underflow_bindex()) + h.count(h.overflow_bindex());
std::cout << "Ignored " << nunbounded << " entries in unbounded bins\n";
std::cout << "moments of the distribution: " << moment0 << " " << moment1 << " " << moment2 << "\n";

FUTURE DIRECTIONS:

Possible directions are for the domain (i.e., bottom,
top, argument to insert)  to be something other than double, 
and for the range to be something other than long (e.g., double).

Also, various statistics-gathering utilities might be very
useful, e.g., utilities to estimate median, mode and 
various moments.

An "Adaptive" histogram that does not require the caller to specify in
advance the divisions between bins would be pretty nifty.


  DOCUMENTATION_END

*/

class histogram {
public:
    using value_type = long;
    using bins_type = std::vector<value_type>;
    using bindex_t = size_t;

    // find_binnum is virtual.  Measurement
    // indicates that this adds one or two
    // ticks to insertion (timing.cxx).
    int insert(double x, long n=1){
        auto bindex = find(x);
        bins[bindex] += n;
        return bindex;
    }

    void clear() {
        bins.assign(bins.size(), 0);
    }

    virtual ~histogram(){
    }

    histogram& operator+=(const histogram& rhs){
        if( bins.size() != rhs.bins.size() )
            throw std::runtime_error("cannot apply operator+= to histograms of unequal size");
        std::transform(bins.begin(), bins.end(), rhs.bins.begin(), bins.begin(), std::plus<value_type>());
        return *this;
    }

    double bottom(bindex_t b) const {
        return cut(b);
    }
    double top(bindex_t b) const { 
        return cut(b+1);
    }
    auto count(bindex_t b) const { 
        return bins[b];
    }
    bool finiteRange(bindex_t b) const {
        return b > underflow_bindex() && b < overflow_bindex();
    }
    bindex_t first_bindex() const{
        return 1;
    }
    bindex_t last_bindex() const{
        return bins.size()-2;
    }
    bindex_t underflow_bindex()  const{
        return 0;
    }
    bindex_t overflow_bindex() const{
        return bins.size()-1;
    }
   
    virtual bindex_t find(double x) const = 0;

protected:
    bins_type bins;
    // The default constructor is useful for derived classes,
    // who may not have enough info to size bins until
    // the body of the constructor
    histogram() {}

    // The default copy constructor is protected.
    histogram(const histogram& h) : bins(h.bins) {}

    // A copy constructor with an extra ignored argument
    // facilitates creation of 'empty-but-isomorphic'
    // histograms.
    histogram(const histogram& h, bool) :
        bins(h.bins.size())
    {}

    virtual double cut(bindex_t bin) const = 0;

    // Allocating a specific size is convenient when
    // the derived class' constructor knows how many
    // bins there are going to be.  Add two for the
    // oveflow...
    histogram(std::size_t n) : bins(n+2) {
    }

    histogram& operator=(const histogram& rhs){
        bins = rhs.bins;
        return *this;
    }

    // swap, analogous to vector::swap.  Allows
    // one to swap two histograms without the copy.
    void swap(histogram& other){
        bins.swap(other.bins);
    }

    // realloc is convenient when the derived class'
    // constructor doesn't know (until some point
    // in the body of the constructor) how many
    // bins there are going to be.  It's also
    // convenient for operator=.
    void realloc(std::size_t n, bool clearme){
        bins.resize(n+2);
        if(clearme)
            clear();
    }
};

/* 
   DOCUMENTATION_BEGIN

uniform_histogram is a histogram with a fixed number of
equal-sized bins.  The constructor is:

  uniform_histogram(double bottom, double top, std::size_t nbins);

There is also a default constructor:

  uniform_histogram()

and a 'reconfigure' method, which you can use to separate
object creation from initialization, e.g.,

    uniform_histogram u;
    //... more stuff, figure out bottom and top ...
    u.reconfigure(bottom, top, nbins);

A uniform_histogram contains nbins Bins between bottom and top.  In addition, it
keeps track of two additional "overflow" bins, one from (-inf, bottom)
and the other from [top, inf).  The Bin::finiteRange() member function
returns false for the overflow bins and true for all the rest.

It's possible to construct a logarithmic histogram with uniform_histogram.  Just
take the log of the inputs, and, if so desired, exponentiate the bin
boundaries before working with them, e.g. to make a histogram over 7
decades with 10 bins per decade:

   uniform_histogram lgh(log(1.e-4), log(1.e3), 70);
   p = lgh.find(log(x));
   double lg_bin_bottom = h.bottom(p);
   double bin_bottom = exp(lg_bin_bottom);

Histograms with bins spaced according to any monotonic function can be
generated this way.  Just change log() to the desired function, and
use the inverse of the functions (e.g., exp()) when extracting limits.

In fact, one can provide the monotonic function, e.g., log, and its
inverse, e.g., exp as arguments to the uniform_histogram constructor:

   uniform_histogram lgh2(1.e-4, 1.e-3, 70, log, exp);
   p = lgh2.find(x);
   bin_bottom = h2.bottom(p);

In addition to a 'normal' copy constructor, there is also
an 'isomorphic-but-empty' copy constructor:

   uniform_histogram(const uniform_histogram& rhs, bool ignored);

This copies the number of bins and their spacings, the forward and
backward maps, etc., but the contents of the constructed bins are
initialized to zero.

WARNING - the uniform_histogram uses the "obvious" arithmetic manipulations to
map insert's argument to a bin, e.g.,

   bin = (int) (x - bottom)*(nbins/(top-bottom))

This "obvious" arithmetic has some surprising (i.e., wrong) roundoff
properties.  In particular, it sometimes rounds up when it should not.
Consider a uniform_histogram(-5., 5., 10), i.e., 10 bins of size 1.0.
insert(-1.0e-18) increments the [0, 1.) bin because when we do 

  x - bottom

we get ((-1.0e-18) - (-5.)) which rounds to 5.0, not 4.9999...999.
Hence the result lands in the wrong bin.  Setting the fp rounding mode
to FE_DOWNWARD before calling insert appears to fix the problem, but
at the cost of about 100 ticks (a factor of nearly 3) on an Opteron
250 per insertion.  Also note that setting the rounding mode to
FE_DOWNWARD might have surprising and undesirable effects on your math
library.  In particular, log and exp may misbehave, which would be
problematic if you're trying to create a logarithmic histogram with
precise intervals.

One could argue that the bug is in 'bottom' and 'top' rather than in
'insert' and 'find' because they do not correctly report the upper
and lower bounds on the values that will be inserted or found.  FIXME -
can we fix bottom and top (at some cost)??

Another possibility is to use nonuniform_histogram, which does a
binary search to find the bin, using a consistent comparison operator
for all tests.  With nonuniform_histogram, you won't get any surprises
from roundouff, but since it's logarithmic in the number of bins,
insertion will be considerably slower than with uniform_histogram.

   DOCUMENTATION_END
*/ 

class uniform_histogram : public histogram {
protected:
    typedef double (*dfd)(double); // double function of double

public:
    // The default constructor is useful for initializing vectors,
    // serialization, etc.
    uniform_histogram() : histogram(), bottom_m(), top_m(), invbinspacing(), forwardmap(0), inversemap(0)
    {}

    uniform_histogram(double _bottom, double _top, std::size_t _nbins,
            dfd _forwardmap = 0, dfd _inversemap =0) 
        : histogram(_nbins), 
          bottom_m(_forwardmap? (*_forwardmap)(_bottom): _bottom ), 
          top_m(_forwardmap? (*_forwardmap)(_top): _top),
          invbinspacing(_nbins/(top_m - bottom_m )),
          forwardmap(_forwardmap),
          inversemap(_inversemap)
    {}

    void reconfigure(double _bottom, double _top, std::size_t _nbins,
            dfd _forwardmap = 0, dfd _inversemap =0) {
        uniform_histogram(_bottom, _top, _nbins, _forwardmap, _inversemap).swap(*this);
    }

    uniform_histogram(const uniform_histogram& h) : 
        histogram(h),
        bottom_m(h.bottom_m),
        top_m(h.top_m),
        invbinspacing(h.invbinspacing),
        forwardmap(h.forwardmap),
        inversemap(h.inversemap)
    {}

    // With a seconde argument, the we copy the
    // structure (bottom_m, top_m, spacing, etc) but
    // not the contents (bins).
    uniform_histogram(const uniform_histogram& h, bool ignored) : 
        histogram(h, ignored),
        bottom_m(h.bottom_m),
        top_m(h.top_m),
        invbinspacing(h.invbinspacing),
        forwardmap(h.forwardmap),
        inversemap(h.inversemap)
    {}

    uniform_histogram& operator=(const uniform_histogram& h){
        histogram::operator=(h);

        bottom_m = h.bottom_m;
        top_m = h.top_m;
        invbinspacing = h.invbinspacing;
        forwardmap = h.forwardmap;
        inversemap = h.inversemap;
        return *this;
    }

    void swap(uniform_histogram &h){
        histogram::swap(h);
        std::swap(bottom_m, h.bottom_m);
        std::swap(top_m, h.top_m);
        std::swap(invbinspacing, h.invbinspacing);
        std::swap(forwardmap, h.forwardmap);
        std::swap(inversemap, h.inversemap);
    }

    // Sometimes you want to set the forward and backward
    // mapping after construction, e.g., when deserializing.
    dfd getForwardMap() const {
        return forwardmap;
    }

    void setForwardMap( double (*_fm)(double) ){
        forwardmap = _fm;
    }

    dfd getInverseMap() const {
        return forwardmap;
    }

    void setInverseMap( double (*_fm)(double) ){
        forwardmap = _fm;
    }

    // Sometimes somebody hands you a histogram and you
    // want to know what it looks like:
    double getBottom() const{
        return bottom_m;
    }

    double getTop() const{
        return top_m;
    }

    virtual bindex_t find(double x) const {
        if(forwardmap)
            x = (*forwardmap)(x);
        // What about NaN?  NaN is entirely plausible if, say,
        // forwardmap = log and we're presented with a non-positive input.
        // If the hardware and compiler follow IEEE rules,
        // both tests should fail with x=NaN, and we should get
        // into the else clause, whose behavior then depends
        // on what happens with (std::size_t)(NaN), which is
        // (I think)  undefined.
        // An isnan test would slow things down a lot in the
        // normal case...  Need a better idea...
        // On x86_64, with gcc4, we the conversion seems to
        // give us 0x8000000000000000, which is pretty much
        // guaranteed to give us a range error if we ever
        // try to do a findbin.  That's the typical result
        // of inserting a non-positive value into a logarithmic
        // histogram.
        if( x < bottom_m )
            return underflow_bindex();
        else if( x >= top_m )
            return overflow_bindex();
        else
            // This rounds badly.  E.g., with: bottom_m=-4,
            // invbinspacing=1, x=1-epsilon we get x-bottom_m = 4 (i.e.,
            // the epsilon is rounded away), pushing us into the [1,2)
            // bin instead of the [0, 1) bin.  This can be fixed by
            // setting the rounding mode to round_down, but that's
            // pretty expensive.
            return 1+(std::size_t)((x - bottom_m)*invbinspacing);
    }
        
protected:
    virtual double cut(bindex_t bin) const{
        if(bin<=0)
            return -INFINITY;
        else if((size_t)bin>=bins.size())
            return INFINITY;
        else{
            double b = bottom_m + (bin-1)/invbinspacing;
            if(inversemap)
                b = (*inversemap)(b);
            return b;
        }
    }

    double bottom_m;
    double top_m;
    double invbinspacing;

    dfd forwardmap;
    dfd inversemap;

};


/* 
   DOCUMENTATION_BEGIN

   A nonuniform_histogram histogram has arbitrary, irregularly spaced bins.  

   template <typename ITER>
   nonuniform_histogram(ITER begin, ITER end);

   The constructor takes a range of 'cuts', specified in the usual way
   with a begin and end iterator.  The value_type of the iterators must
   be convertible to double.  The values of the cuts define the
   boundaries between bins.  They must be monotonically increasing.
   E.g.,

   double cuts[6] = {-3.14, -2.718, -.577, 0., 1.414, 1.732};
   histograms::nonuniform_histogram h( &cuts[0], &cuts[6] );
   h.insert(1.0);

   There is also a default constructor:

     nonuniform_histogram()

   and a 'reconfigure' method, which you can use to separate
   object creation from initialization, e.g.,

     nonuniform_histogram nu;
     //... more stuff, figure out cuts ...
     nu.reconfigure(cuts.begin(), cuts.end());

   Insertion requires a binary search and is therefore logarithmic in
   the number of bins.

   In addition to a 'normal' copy constructor, there is also
   an 'isomorphic-but-empty' copy constructor:

     nonuniform_histogram(const nonuniform_histogram& rhs, bool ignored);

   This copies the cuts and the number of bins, but the contents
   of the constructed bins are initialized to zero.

   DOCUMENTATION_END

   TODO - should we have a default constructor and a way to
   modify the cuts and clear the counts after construction?
   Insisting that the structure of the cuts be known at
   construction time is inconvenient and leads to idioms like:

   nonuniform_histogram nu((double *)0, (double *)0);

   nonuniform_histogram(b, e).swap(nu);
*/ 

class nonuniform_histogram : public histogram {
public:
    // The default constructor is useful for initializing vectors,
    // serialization, etc.
    nonuniform_histogram() : cuts(){
        cuts.push_back(-INFINITY);
        cuts.push_back(INFINITY);
    }

    template <typename ITER>
    nonuniform_histogram(ITER begin, ITER end){
        cuts.push_back(-INFINITY);
        copy(begin, end, back_inserter(cuts));
        cuts.push_back(INFINITY);
        // There's probably some waste in cuts.capacity() - cuts.size().
        // Should we care?  The idiom to 'compact' a vector is:
        //  std::vector<double>(cuts).swap(cuts);
        // C++11 has cuts.shrink_to_fit()
        realloc(cuts.size() - 3, true);
    }

    template <typename ITER>
    void reconfigure(ITER begin, ITER end){
        nonuniform_histogram(begin, end).swap(*this);
    }

    nonuniform_histogram(const nonuniform_histogram& h) : 
        histogram(h),
        cuts(h.cuts)
    {}

    // With a second argument, we copy the structure (cuts)
    // but not the contents (bins) of the histogram.
    nonuniform_histogram(const nonuniform_histogram& h, bool ignored) : 
        histogram(h, ignored),
        cuts(h.cuts)
    {}

    nonuniform_histogram& operator=(const nonuniform_histogram& h){
        // histogram::operator= and the constructor
        // for the newcuts could conceivably throw.
        // Leave *this intact until we're safely
        // out of danger.
        std::vector<double> newcuts(h.cuts);
        histogram::operator=(h);

        // We're committed now.  We've modified
        // *this, so we better finish without
        // throwing anything.
        cuts.swap(newcuts);
        return *this;
    }

    void swap( nonuniform_histogram& h){
        histogram::swap(h);
        cuts.swap(h.cuts);
    }

    virtual bindex_t find(double x) const{
        // Cool!  There's an STL algorithm to do this?
        std::vector<double>::const_iterator p 
            = upper_bound(cuts.begin(), cuts.end(), x);
        return (p - cuts.begin())-1;
    }
        
protected:
    virtual double cut(bindex_t bin) const{
        return cuts[bin];
    }

    std::vector<double> cuts;
};

} // namespace core123

