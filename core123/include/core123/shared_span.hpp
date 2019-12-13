// Sometimes we want to allocate some memory, and then dole out
// 'views' of it to our friends.  E.g., we allocate some memory, do
// some network calls to fill it, then lop off some undesired header
// and padding, maybe do an in-place decrypt, and then hand off a
// pointer to a sub-set of what's been decrypted to a consumer.

// shared_span is an attempt to keep track of both ownership and
// bounds during this kind of activity.

#pragma once

#include "byt.hpp"
#include "shared_blob.hpp"
#include "span.hpp"
#include <cstddef>

namespace core123{

class shared_span : public tcb::span<byt>{
    using spantype = tcb::span<byt>;
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
    shared_span whole_span() {
        return {b.sp(), b.size()};
    }

    // blob - return *a const reference* to the underlying shared_blob.
    const shared_blob& blob() const {
        return b;
    }

    // subspan, first and last - return a shared_span with the same underlying
    // blob, but with a span that is 'this' span's ->subspan, ->first or ->last.
    // Throw an out_of_range exception if offset or count are out of range.
    shared_span subspan(size_t offset, size_t count=tcb::dynamic_extent){
        auto ret = *this;
        if(offset > size())
            throw std::out_of_range("shared_span::subspan offset exceeds size()");
        if(count != tcb::dynamic_extent && offset+count > size())
            throw std::out_of_range("shared_span::subspan offset+count exceeds size()");
        (spantype&)ret = ((spantype*)this)->subspan(offset, count);
        return ret;
    }
    shared_span first(size_t count){
        if(count > size())
            throw std::out_of_range("shared_span::first");
        return subspan(0, count);
    }
    shared_span last(size_t count){
        if(count > size())
            throw std::out_of_range("shared_span:last");
        return subspan(size()-count, count);
    }

    // avail_{front,back} - how much room is available for grow_{front,back}
    size_t avail_front() const { return data() - b.data(); }
    size_t avail_back() const { return b.size() - avail_front() - size(); }

    // grow_{front,back} - return a shared_span that points into the
    // same blob, but that has more bytes at the front or back.
    // Throws an out_of_range exception if there isn't enough in
    // the underlying blob.
    shared_span grow_front(size_t count){
        return whole_span().subspan(avail_front()-count, size()+count);
    }
    shared_span grow_back(size_t count){
        return whole_span().subspan(avail_front(), size()+count);
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
