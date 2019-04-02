#pragma once
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <mutex>

// Mark an object of type T with an expiration time.
//
// The std::chrono Kool-aid is completely gratuitous.  We could have
// done it all with time_t and gettimeofday.

// ISSUES:
//   T must be a class (something we can derive from).  We can't
//   have an expiring<int>.  This should be fixable with some
//   magic involving std::enable_if<std::is_class<T>::value>>.

namespace core123{
template<typename T, typename Clk = std::chrono::system_clock>
struct expiring : public T{
    using clk_t = Clk;
    typename Clk::time_point good_till;

    expiring() : T{}, good_till{clk_t::time_point::min()} // bool(expiring()) == false
    {}

    // More ambitious would be to template the
    // constructor over any argument pack that
    // can be used to construct a T...
    template<class Rep, class Period>
    expiring(std::chrono::duration<Rep, Period> ttl, T _t) : 
        expiring(clk_t::now() + ttl,  std::move(_t))
    {}

    expiring(const typename clk_t::time_point& _good_till, T _t) :
        T(std::move(_t)), good_till(_good_till)
    {}

    // The 'asifnow' arguments can be used to cut down on the number
    // of times clk_t::now().
    bool expired(typename clk_t::time_point asifnow = clk_t::now()) const{
        return asifnow > good_till;
    }

    auto ttl(typename clk_t::time_point asifnow = clk_t::now()) const{
        return good_till - asifnow;
    }
};

template <typename T, typename Clk=std::chrono::system_clock>
expiring<T, Clk>
make_already_expired(T t){
    return expiring<T,Clk>( Clk::time_point::min(), std::move(t));
}

template <typename T, typename Clk=std::chrono::system_clock>
expiring<T, Clk>
make_never_expires(T t){
    return expiring<T, Clk>( Clk::time_point::max(), std::move(t));
}


// expiring_cache - a very simple, thread-safe "map" that supports
//  insertion and deletion of 'expiring' key/value pairs.  If
//  constructed with a 'size' argument, then every insertion is
//  followed by sufficient random evictions to prevent growth past the
//  stated size.
//
//  The entries carry an expiration time, set at the time of insertion,
//  by specifying a ttl (in seconds) from the moment of insertion.  If
//  the ttl is cache_control:max_age - Age, then the semantics of the
//  expiring cache will match those of an http proxy.
// 
//  Successful lookups will return an expiring<T> with an expired()
//  method that (for now) returns false.  Unsuccessful lookups, will
//  return a value with expired() that returns true.
//
//  A lookup may be unsuccessful either because there is no matching
//  key, or the associated value has expired, or it has been randomly
//  evicted.  When a lookup fails due to expiration, the corresponding
//  entry is erased from the map.
//
//  In accord with STL convention, insert() does not replace an existing entry.
//  However, typical usage is expected to be:
//      auto v = ec.lookup(k);
//      if(v) 
//         return v;
//      ret = ... // uncached lookup
//      ec.insert(k, ret, ttl);
//
//  so the entry is very unlikely to be present when insert is called.
//  (The probability is non-zero because of interleaved threads).
template <typename K, typename V, typename Clk = std::chrono::system_clock>
class expiring_cache{
    using eV = expiring<V, Clk>;
    const size_t max_size;
    std::unordered_map<K, eV> themap;
    std::mutex mtx;
    size_t _evictions, _expirations, _hits, _misses;
    size_t evict_bkt;

    // random_eviction: find a "random" bucket and erase a number of
    // entries, equal to the bucket's size, starting at the first
    // entry (if any) in the bucket.  On average, this will erase
    // load_factor() entries.  It might erase nothing!  If the
    // implementation is sane, it will erase all the entries in the
    // chosen bucket, but I don't think the standard actually
    // guarantees that.
    void random_eviction(){
        //  Lock must be held while called!!
        if(++evict_bkt>=themap.bucket_count())
            evict_bkt = 0;
        auto n = themap.bucket_size(evict_bkt);
        if(n==0)
            return;
        auto it = themap.find(themap.begin(evict_bkt)->first);
        do{
            // N.B.  an easy way to fail the valgrind unit test is to
            // write this (incorrectly!!) as:  
            //   themap.erase(it); ++it; 
            it = themap.erase(it);
            ++_evictions;
        }while(--n && it!=themap.end());
    }

public:
    using clk_t = Clk;
    expiring_cache(size_t _max_size) :
        max_size(_max_size), 
        themap(),
        _evictions(0),
        _expirations(0),
        _hits(0),
        _misses(0),
        evict_bkt(0)
    {}
    eV lookup(const K& k, typename clk_t::time_point asifnow = clk_t::now()){
        std::lock_guard<std::mutex> lk(mtx);
        auto ii = themap.find(k);
        if(ii == themap.end()){
            _misses++;
            return {};
        }
        if( ii->second.expired(asifnow) ){
            themap.erase(ii);
            _expirations++;
            return {};
        }
        _hits++;
        return ii->second;
    }
    bool insert(const K& k, const eV& v){
        if(max_size==0)
            return false;
        std::lock_guard<std::mutex> lk(mtx);
        bool ret = themap.emplace(k, v).second;
        while(themap.size() > max_size)
            random_eviction(); // N.B.  might evict the one we just inserted, iterators are invalidated!
        return ret;
    }

    template <class Rep, class Period>
    bool insert(const K& k, const V& r, std::chrono::duration<Rep, Period> ttl){
        // it's weird, but not harmful, to insert an item with
        // negative ttl.  It will be erased the first time it is
        // lookup'ed.
        return insert(k, eV(ttl, r));
    }
    
    bool insert(const K& k, const V& r, const typename Clk::time_point& tp){
        return insert(k, eV(tp, r));
    }

    auto erase(const K& k){
        std::lock_guard<std::mutex> lk(mtx);
        return themap.erase(k);
    }

    void erase_expired(typename clk_t::time_point asifnow = clk_t::now()){
        std::lock_guard<std::mutex> lk(mtx);
        // New-fangled erase_if?  Not until it's in clang's libcpp...
        for(auto i = themap.begin(), last=themap.end(); i != last; ){
            if( i->second.good_till < asifnow ){
                _evictions++;
                i = themap.erase(i);
            }else{
                ++i;
            }
        }
    }

    size_t evictions() const { return _evictions; }
    size_t hits() const { return _hits; }
    size_t misses() const { return _misses; }
    size_t expirations() const { return _expirations; }
    size_t size() const { return themap.size(); }
};
} // namespace core123
