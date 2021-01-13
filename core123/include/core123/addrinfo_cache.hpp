#pragma once

// addrinfo_cache - a cache of the results of calling getaddrinfo.
//
//   addrinfo_cache addresses the overhead and unpredictability of
//   calling getaddrinfo.  Usually, getaddrinfo returns in under a
//   millisecond.  But sometimes it accesses the filesystem
//   (/etc/hosts), or network services (DNS) and (especially under
//   heavy load, or during network outages) it can take seconds or
//   even minutes to return.
//
//   Values "returned" by getaddrinfo are recorded in a:
//
//   struct addrinfo_result{
//     int status;
//     struct addrinfo* aip;
//     int eno;  // non-zero only if status==EAI_SYSTEM
//   }
//
//   which are cached so they can be found quickly - avoiding system
//   calls and network traffic on the caller's critical path.
//
//   The lookup() member takes arguments that are analogous to
//   getaddrinfo's and returns a shared_ptr to an addrinfo_result.
//
//     std::shared_ptr<addrinfo_result>
//     addrinfo_cache::lookup(const std::string& name,
//                            const std::string& service,
//                            addrinfo* hints = nullptr)
//
//   if a result is in the cache, then return it.
//
//   if a result is not in the cache, call getaddrinfo, and if the
//   status is not EAI_AGAIN, record the result in the cache.  Return
//   the result.
//
//   N.B.  If name or service is an empty string, the corresponding
//   argument to gettaddrinfo will be a nullptr.
//
//   The cache is refreshed explicitly, at the caller's request by:
//
//     void
//     addrinfo_cache::refresh(size_t max_size=100)
//
//   which removes least recently used entries until the cache has
//   fewer entries than the specified max_size, then refreshes every
//   remaining cached addrinfo_result by calling getaddrinfo again,
//   with the same arguments.  The cached entry is left unchanged if
//   getaddrinfo returns EAI_AGAIN.
//
//   Refresh might take a long time and is expected to be called from
//   time to time, in a background thread, off the critical path.
//   E.g., something like:
//
//      core123::addrinfo_cache aic;
//      core123::periodic refresh_thread{[&](){
//          aic.refresh();
//          return std::chrono::minutes(1);
//      }};
//     
//   Theoretically, very large caches can be accomodated, but since
//   refresh takes time proportional to the cache size, care should be
//   taken with caches much bigger than a few hundred entries.
//   addrinfo_cache is probably not the right tool to manage millions
//   of names.
//
//   Informational methods:
//     size_t size() - returns the number of names in the cache.
//     size_t eai_again_count() - returns the number of times getaddrinfo
//              has returned EAI_AGAIN.

// THREAD SAFETY: all member functions are thread safe and are
//   synchronized so they may be called concurrently.
//
// BLOCKING: Calls to getaddrinfo are potentially slow and may block
//   for seconds or minutes.  But getaddrinfo is only called when
//   lookup() misses the cache (the first time a particular
//   combination of args is presented to lookup) and by refresh() (off
//   the critical path).
//
// CAVEATS: Note that the cache grows whenever a lookup is called with
//   'new' arguments.  The cache shrinks when refresh is called.  The
//   addrinfo_cache should perform well up to a max_size of a few
//   hundred entries, but beyond that, refresh may become painfully
//   slow.
//
// ENHANCEMENT: Could be more generic.  The details of calling
//   getaddrinfo and freeaddrinfo could be separated from the generic
//   machinery of maintaining the map and the lru list.
//
// ISSUES: Calling getaddrinfo(3) is not simple.  Interpreting the
//   results is not simple.  This module does not help with any of
//   that.  See man getaddrinfo(3) for details.


#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

namespace core123{

namespace detail{
struct gai_args{
    std::string name;
    std::string service;
    struct addrinfo hints;
    bool operator<(const gai_args& rhs) const{
        return std::tie(name, service, hints.ai_family, hints.ai_socktype, hints.ai_protocol, hints.ai_flags) <
            std::tie(rhs.name, rhs.service, rhs.hints.ai_family, rhs.hints.ai_socktype, rhs.hints.ai_protocol, rhs.hints.ai_flags);
    }
    gai_args(const std::string& _name, const std::string& _service, struct addrinfo* _hints)
        : name(_name), service(_service)
    {
        if(_hints){
            hints = *_hints;
            // the caller should have zero'ed these, but let's not trust the caller.
            hints.ai_addrlen = 0;
            hints.ai_addr = nullptr;
            hints.ai_canonname = nullptr;
            hints.ai_next = nullptr;
        }else{
            std::memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_UNSPEC;
            hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG;
        }
    }
};

} // namespace detail

struct addrinfo_result{
    int status;
    struct addrinfo* aip;
    int eno; // non-zero only if status==EAI_SYSTEM
    addrinfo_result(const detail::gai_args& args){
        aip = nullptr;
        status = ::getaddrinfo(args.name.empty()? nullptr : args.name.c_str(),
                               args.service.empty()? nullptr : args.service.c_str(),
                               &args.hints,
                               &aip);
        eno = (status == EAI_SYSTEM) ? errno : 0;
    }
    // Non-copyable.  We don't want to call freeaddrinfo twice!
    addrinfo_result(const addrinfo_result&) = delete;
    addrinfo_result& operator=(const addrinfo_result&) = delete;
    ~addrinfo_result(){
        if(aip)
            ::freeaddrinfo(aip);
    }
};

namespace detail{
struct gai_mrecord;
using gai_map_t = std::map<gai_args, gai_mrecord>;
struct gai_mrecord{
    std::shared_ptr<addrinfo_result> air;
    // more_ru and less_ru constitute a linked list of map iterators
    // in most-recently-used order.  They are not initialized by the
    // ctor, but are assigned when the gai_mrecord is inserted into
    // the_map.  The more_ru member of the *most_ru and the less_ru
    // member of *least_ru remain undefined, and may not be used, even
    // for gai_mrecords in the_map.
    gai_map_t::iterator less_ru;
    gai_map_t::iterator more_ru;
    gai_mrecord(std::shared_ptr<addrinfo_result> _air):
        air(_air)
    {}
};
} // namespace detail

struct addrinfo_cache{
    std::shared_ptr<addrinfo_result>
    lookup(const std::string& name, const std::string& service, addrinfo* hints = nullptr){
        detail::gai_args args(name, service, hints);
        std::unique_lock<std::mutex> lk(map_mtx);
        auto p = the_map.find(args);
        if(p!=the_map.end()){
            make_most_ru(p);
            _hit_count++;
            return p->second.air;
        }
        lk.unlock();
        auto newvalue = std::make_shared<addrinfo_result>(args); // might be slow
        _miss_count++;
        if(newvalue->status == EAI_AGAIN){
            _eai_again_count++;
            return newvalue; // return it, but don't record it in the_map.
        }
        lk.lock();
        bool inserted;
        std::tie(p, inserted) = the_map.insert(std::make_pair(args, detail::gai_mrecord{newvalue}));
        if(!inserted){  // rarely - only if another was inserted while we weren't holding the lock
            // NOT WELL TESTED!
            p->second.air = newvalue;
            make_most_ru(p);
            return p->second.air;
        }
        if(the_map.size()==1){
            least_ru = p;
        }else{
            // make p more ru than the previous most_ru
            most_ru->second.more_ru = p;
            p->second.less_ru = most_ru;
        }
        most_ru = p;
        return newvalue;
    }

    void refresh(size_t max_size = 100){
        std::lock_guard<std::mutex> refresh_lg(refresh_mtx);
        std::unique_lock<std::mutex> lk(map_mtx);
        while(the_map.size() > max_size){
            auto p = least_ru->second.more_ru;
            the_map.erase(least_ru);
            least_ru = p;
        }
        // It's safe to iterate over the_map because the only place
        // erase is called is a few lines above, and we're safely
        // under the same refresh_lg lock_guard, so no other threads
        // can be erase-ing behind our back.
        for(auto& e : the_map){
            lk.unlock();
            auto newvalue = std::make_shared<addrinfo_result>(e.first); // might be slow
            _refresh_count++;
            lk.lock();
            if(newvalue->status != EAI_AGAIN) // don't update the_map with transient failures
                e.second.air = newvalue;
            else
                _eai_again_count++;
        }
    }

    size_t size() const{
        std::unique_lock<std::mutex> lk(map_mtx);
        return the_map.size();
    }

    size_t eai_again_count() const{
        return _eai_again_count;
    }

    size_t hit_count() const{
        return _hit_count;
    }

    size_t miss_count() const{
        return _miss_count;
    }

    size_t refresh_count() const{
        return _refresh_count;
    }
    addrinfo_cache() = default;
    // Non-copyable
    addrinfo_cache(const addrinfo_cache&) = delete;
    addrinfo_cache& operator=(const addrinfo_cache&) = delete;
private:
    detail::gai_map_t the_map;
    using iter = detail::gai_map_t::iterator;
    std::atomic<size_t> _eai_again_count{0}; // count of EAI_AGAIN returns
    std::atomic<size_t> _hit_count{0};
    std::atomic<size_t> _miss_count{0};
    std::atomic<size_t> _refresh_count{0};
    // most_ru and least_ru are the head and tail of an auxiliary
    // linked list in recently-used order.  They are uninitialized,
    // and may not be used unless the_map contains at least one entry.
    iter most_ru;
    iter least_ru;
    // Member functions hold the map_mtx whenever modifying the_map.
    // The map_mtx is released when blocking getaddrinfo is called.
    mutable std::mutex map_mtx;
    // Only one refresh may run concurrently.  The refresh_mtx is held
    // by refresh() through getaddrinfo calls.
    mutable std::mutex refresh_mtx;

    void
    make_most_ru(iter p){
        // Pre-condition:  p is a bona fide initialized iterator into the_map
        //  and its ru_less and ru_more members connect it to the ru-list.
        if(p==most_ru)
            return;
        // N.B.  p is in the ru-list, and it is not the most_ru, so
        //       there are at least two entries in the ru-list now.
        detail::gai_mrecord& pmr = p->second;
        if(p==least_ru){
            // disconnect pmr by moving the least_ru link "forward"
            least_ru = pmr.more_ru;
            // N.B.  least_ru->second.more_ru is undefined/unusable
        }else{
            // disconnect pmr by relink "around"  it
            pmr.less_ru->second.more_ru = pmr.more_ru;
            pmr.more_ru->second.less_ru = pmr.less_ru;
        }
        // reconnect it at the most_ru end:
        pmr.less_ru = most_ru;
        // N.B.  pmr.more_ru is undefined/unusable
        most_ru->second.more_ru = p;
        most_ru = p;
    }

    // _check_invariant should *never* fail.  If it does, it's a bug
    // (logic_error), not a runtime_error.  But since a bug slipped
    // through early testing, we keep _check_invariant around to be
    // used in unit tests.  It takes time O(s*log(s)) where
    // s=the_map.size().  It's all internal map iterator derefs and
    // finds - no calls to getaddr_info, so it's not *that* slow.  BUT
    // it holds the lock for the entire time that it runs, so it shouldn't
    // be called frequently in production.
    void check_invariant_already_locked() const{
        // HACK!!!  Maybe we should make a genuine core123::assert ??
#define core123__assert(P) do{ if(!(P)) throw std::runtime_error("invariant violated: " #P); }while(0)
        if(the_map.size() == 0)
            return;
        // for each item, p, in the ru-list:
        // check the invariant that p really is "in"
        // the map and that p->less->more == p
        auto p = most_ru;
        size_t n = 1;
        while( true ){
            core123__assert(the_map.find(p->first) == p);
            if(p == least_ru)
                break;
            auto less = p->second.less_ru;
            core123__assert(less->second.more_ru == p);
            p = less;
            ++n;
        }
        core123__assert(n == the_map.size());
#undef core123__assert
    }
public:
    void _check_invariant() const{
        std::unique_lock<std::mutex> lk(map_mtx);
        check_invariant_already_locked();
    }
};

} // namespace core123
