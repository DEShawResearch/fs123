// Sometimes we want to allocate some memory, and then dole out
// 'views' of it to our friends.  E.g., we allocate some memory, do
// some network calls to fill it, then lop off some undesired header
// and padding, maybe do an in-place decrypt, and then hand off a
// pointer to a sub-set of what's been decrypted to a consumer.

// shared_span is an attempt to keep track of both ownership and
// bounds during this kind of activity.

#pragma once

#include "shared_blob.hpp"
#include "span.hpp"
#include "str_view.hpp"
#include <cstddef>
#include <cstring>

namespace core123{

// as_span, as_str_view: Convert between core123::str_view and
// tcb::span<unsigned char>.  These should be somewhere else (or
// ideally, nowhere at all).  The casts are pretty scary.  In as_span,
// we're throwing away const-ness and converting from char* to
// unsigned char*, both of which are potentially UB.  We really should
// refactor so we don't need these...
inline str_view as_str_view(tcb::span<unsigned char> sp){
    return {(char*)sp.data(), sp.size()};
}

inline tcb::span<unsigned char> as_span(str_view sv){
    return {(unsigned char*)sv.data(), sv.size()};
}

class shared_span : public tcb::span<unsigned char>{
    using spantype = tcb::span<unsigned char>;
    shared_blob b;
public:
    // constructors:  default, from a pre-existing shared_ptr+len, from
    // a pre-existing shared_blob and from just a len.
    shared_span() : spantype(), b() {}
    shared_span(shared_blob::blob_type sp, size_t len) : spantype(sp.get(), len), b(sp, len){}
    shared_span(shared_blob _b) : spantype(_b.as_span()), b(_b) {}
    shared_span(size_t len) : b(len){
        (spantype&)(*this) = b.as_span();
    }

    // whole_span - returns a shared span that covers the whole underlying blob
    shared_span whole_span() const {
        return {b.sp(), b.size()};
    }

    // blob - return *a const reference* to the underlying shared_blob.
    const shared_blob& blob() const {
        return b;
    }

    // unshared_copy - create a completely new shared_span with a copy
    // of this->data(), and the same amount of avail_front() and
    // avail_back() space.
    shared_span unshared_copy() const {
        shared_span ret(b.size());
        ret = ret.subspan(data() - b.data(), size());
        ::memcpy(ret.data(), data(),  size());
        return ret;
    }

    // Non-modifying methods:
    // 
    // subspan, first and last - return a shared_span with the same underlying
    // blob, but with a span that is 'this' span's ->subspan, ->first or ->last.
    // Throw an out_of_range exception if offset or count are out of range.
    shared_span subspan(size_t offset, size_t count=tcb::dynamic_extent) const {
        auto ret = *this;
        if(offset > size())
            throw std::out_of_range("shared_span::subspan offset exceeds size()");
        if(count != tcb::dynamic_extent && offset+count > size())
            throw std::out_of_range("shared_span::subspan offset+count exceeds size()");
        (spantype&)ret = ((spantype*)this)->subspan(offset, count);
        return ret;
    }
    shared_span first(size_t count) const {
        if(count > size())
            throw std::out_of_range("shared_span::first");
        return subspan(0, count);
    }
    shared_span last(size_t count) const {
        if(count > size())
            throw std::out_of_range("shared_span:last");
        return subspan(size()-count, count);
    }

    // avail_{front,back} - how much room is available for grow_{front,back}
    size_t avail_front() const { return data() - b.data(); }
    size_t avail_back() const { return b.size() - avail_front() - size(); }

    // Modifying methods:
    //
    // grow_{front,back} - change the span contained in *this.  They
    // return a shared_span to the *newly* grown but uninitialized
    // data.  Throw an out_of_range exception if there isn't enough
    // space in the underlying blob.
    shared_span grow_front(size_t count) {
        if(avail_front() < count)
            throw std::out_of_range("shared_span::grow_front:  not enough space");
        (spantype&)(*this) = {data()-count, size()+count};
        return subspan(0, count);
    }
    shared_span grow_back(size_t count) {
        if(avail_back() < count)
            throw std::out_of_range("shared_span::grow_front:  not enough space");
        (spantype&)(*this) = {data(), size()+count};
        return subspan(size()-count, count);
    }

    // {ap,pre}pend - like grow_{front,back}, but they also copy
    // the data from their argument into the newly acquired space.
    shared_span prepend(spantype more){
        auto ret = grow_front(more.size()); // checks sizes
        ::memcpy(ret.data(), more.data(), more.size());
        return ret;
    }
    shared_span append(spantype more){
        auto ret = grow_back(more.size()); // checks sizes
        ::memcpy(ret.data(), more.data(), more.size());
        return ret;
    }

    void reset(spantype s) {
        if(s.data() < b.data() || s.data() + s.size() > b.data() + b.size())
            throw std::out_of_range("shared_span::reset(span) points to data not in managed blob");
        (spantype&)(*this) = s;
    }

    // Methods that pass through to the blob's shared pointer.
    // N.B.  operator bool is from spantype::bool, not blob.
    bool unique() const noexcept{
        return b.sp().unique();
    }
    long int use_count() const noexcept{
        return b.sp().use_count();
    }
};

}
