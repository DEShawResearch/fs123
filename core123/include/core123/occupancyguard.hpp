#pragma once

#include <atomic>

// An "Occupancy Guard" allows you to satisfy the fire marshall's
// demand that a "room" will have no more than N occupants at any
// one time.
//
// First, before any thread constructs an occupancyguard, establish the
// capcacity in a std::atomic<int>:
// 
//   std::atomic<int> Capacity = N;
//
// Then, use the *same* Capacity as the constructor argument to one or
// more occupancyguards, which may be constructed in different
// threads.  The guard will limit the number of simultaneous occupants
// to the initial value, N.  E.g.
//
//   occupancyguard g(&Capacity);
//
//   if(g){
//       // hooray!  We're in!
//   }else{
//       // booo!  The room was full.  We didn't get in!
//   }
// 
// If an occupancyguard is occupying a slot, then its bool conversion
// is true.  The slot is occupied until the occupancyguard goes out of
// scope or until it is released.  If g was not granted a slot when it
// was constructed, then its bool conversion is false and nothing
// happens when it goes out of scope or is released.
//
// occupancyguard is non-copyable, but it is move-able, so ownership
// can be transferred in the conventional way with std::move.
//
// All methods are noexcept.  Should that be explicitly part of the
// interface?
//
// UNTESTED - It's possible to change the capacity while slots are
// allocated.  Simply increment (fetch_add) or decrement (fetch_sub)
// the Capacity.  No notification will be sent if the capacity is
// reduced below the current occupancy, but new occupants will be
// denied until the occupancy again falls below the capacity.
namespace core123{
struct occupancyguard{
    occupancyguard(std::atomic<int>* capacity = nullptr) : Np(capacity){
        if(!Np)
            return;
        if(Np->fetch_sub(1) <= 0){
            Np->fetch_add(1);
            Np = nullptr;
        }
    }
    occupancyguard(const occupancyguard&) = delete;
    occupancyguard& operator=(const occupancyguard&) = delete;
    occupancyguard(occupancyguard&& rhs) noexcept : Np(rhs.Np) {
        rhs.Np = nullptr;
    }
    occupancyguard& operator=(occupancyguard&& rhs) noexcept {
        selfdestruct();
        Np = rhs.Np;
        rhs.Np = nullptr;
        return *this;
    }
    operator bool() const{
        return Np;
    }
    void release() {
        selfdestruct();
        Np = nullptr;
    }
    ~occupancyguard() {
        selfdestruct();
    }
protected:
    std::atomic<int>* Np;
    void selfdestruct() {
        if(Np)
            Np->fetch_add(1);
    }
};
} // namespace core123

