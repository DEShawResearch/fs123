// uchar_span.hpp - some tools for working with contiguous blocks of
// unsigned chars.  (N.B.  When we really switch to C++17, we might
// create a parallel bytespan.hpp).

// Sometimes we want a blob of bytes.  It turns out that (in C++11)
//   blob = new unsigned char[N]
// is a pretty reliable way to get such a blob.
//
// The main advantage over string and vector<unsigned char> is that it
// doesn't insist on zero-initializing.  It also doesn't have all the
// extra baggage of string.  There are many disadvantages.  Among
// them: it doesn't carry the length along with it and bare new is
// "never" a good idea.
//
// So:
// A "uchar_blob" is a unique_ptr<unsigned char[]> and a length.
// A "uchar_span" is a tcb::span<unsigned char>

// It's sometimes useful to allocate some memory, and then work with
// 'views' of various sizes within it.  E.g., we allocate some memory,
// do some I/O to fill it, then slap a header on the front, call an
// in-place encryption function and finally hand off a pointer to a
// somehat larger blob to some network code.

// A "padded_uchar_span" is a uchar_span with an associated "bounding
// box".  In addition to all the usual span-y things, you can also
// "grow" the span *within* the bounding box with: grow_front()
// grow_back().  The shrinking members (subspan, front, back) return a
// padded_uchar_span (not just a uchar_span), so they retain the
// bounding box.

// The free function: as_str_view converts from a uchar_span to a
// str_view.

// The free function: as_uchar_span returns a uchar_span over the
// contents of a *non*-const string reference.  Note that it avoids
// const_cast, but it does a reinterpret_cast from char* to unsigned
// char*, which seems safe (famous last words).

// Note that as_uchar_span will not accept a str_view or a const
// reference because doing so would implicitly throw away const-ness.
// Doing so is a bad smell in general, and catastrophic if std::string
// is copy-on-write, which is "still a thing" in 2020 thanks to
// Redhat's devtoolset reliance on libstdc++ from 4.9.5.

#pragma once
#include "span.hpp"
#include "str_view.hpp"
#include <cstddef>
#include <memory>
#include <cstring>

namespace core123{

using uchar_span = tcb::span<unsigned char>;

class uchar_blob{
    size_t len;
    std::unique_ptr<unsigned char[]> bytes;
public:
    uchar_blob() : len{}, bytes{}{}
    uchar_blob(size_t _len) : len(_len), bytes(new unsigned char[_len]){}
    uchar_blob(std::unique_ptr<unsigned char[]>&& _bytes, size_t _len) : len(_len), bytes(std::move(_bytes)){}
    size_t size() const { return len; }
    unsigned char* data() const {
        return bytes.get();
    }
    operator bool() const { return bool(bytes); }
    unsigned char* release() noexcept { return bytes.release(); }
    explicit operator uchar_span() const {
        return {data(), size()};
    }
};

class padded_uchar_span : public uchar_span{
    uchar_span bb; // "bounding box"
public:
    // constructors:  default, from a pre-existing uchar_span, from
    // a pre-existing shared_blob and from just a len.
    padded_uchar_span() : uchar_span(), bb() {}
    padded_uchar_span(uchar_span _bb, size_t offset=0, size_t count=tcb::dynamic_extent) : bb(_bb) {
        // avoid UB
        if(offset > bb.size())
            throw std::out_of_range("padded_uchar_span::ctor offset exceeds size()");
        if(count != tcb::dynamic_extent && offset+count > bb.size())
            throw std::out_of_range("padded_uchar_span::ctor offset+count exceeds size()");
        (uchar_span&)(*this) = bb.subspan(offset, count);
    }
    
    // bounding_box - returns the padded span that "fills" the bounding box
    padded_uchar_span bounding_box() const {
        return bb;
    }

    // avail_{front,back} - how much room is available for grow_{front,back}
    size_t avail_front() const { return data() - bb.data(); }
    size_t avail_back() const { return bb.size() - avail_front() - size(); }

    // subspan - like span::subspan *but* the offset argument is signed, and
    // the requirement on the value of offset and count is that the
    // span of count bytes starting at data()+offset must fit entirely
    // within the bounding box.  If it does not, an out_of_range error
    // is thrown.
    padded_uchar_span subspan(ssize_t offset, size_t count) const {
        auto ret = *this;
        if(offset < -ssize_t(avail_front()))
            throw std::out_of_range("padded_uchar_span::subspan seeking too much space in front");
        if(avail_front() + offset + count > bb.size())
            throw std::out_of_range("padded_uchar_span::subspan seeking too much space at back");
        (uchar_span&)ret = uchar_span(data()+offset, count);
        return ret;
    }
    // first - equivalant to subspan(0, count)
    padded_uchar_span first(size_t count) const {
        return subspan(0, count);
    }
    // last - equivalent to subspan(size()-count, count)
    padded_uchar_span last(size_t count) const {
        return subspan(ssize_t(size())-ssize_t(count), count);
    }

    // grow_front - equivalnet to subspan(-count, count+size())
    padded_uchar_span grow_front(size_t count) {
        return subspan(-ssize_t(count), count + size());
    }
    // grow_back -  equivalent to subspan(0, count+size)
    padded_uchar_span grow_back(size_t count) {
        return subspan(0, count + size());
    }

    // {ap,pre}pend - grow_{front,back}(more.size()), followed by
    // memcpy of the bytes of 'more' into the newly acquired space.
    padded_uchar_span prepend(uchar_span more){
        auto ret = grow_front(more.size());
        ::memcpy(ret.data(), more.data(), more.size());
        return ret;
    }
    padded_uchar_span append(uchar_span more){
        auto ret = grow_back(more.size());
        ::memcpy(data()+size(), more.data(), more.size());
        return ret;
    }
    padded_uchar_span prepend(str_view more){
        auto ret = grow_front(more.size());
        ::memcpy(ret.data(), more.data(), more.size());
        return ret;
    }
    padded_uchar_span append(str_view more){
        auto ret = grow_back(more.size());
        ::memcpy(data()+size(), more.data(), more.size());
        return ret;
    }
};

inline str_view as_str_view(uchar_span sp){
    return {reinterpret_cast<const char*>(sp.data()), sp.size()};
}

// See comments above about why as_uchar_span doesn't accept a const
// reference or str_view.
inline uchar_span as_uchar_span(std::string& s){
    // We have to use reinterpret_cast to convert from char to
    // unsigned char.  Add a static_assert to be absolutely sure we're
    // not discarding const-ness.
    static_assert( !std::is_const<decltype(&s[0])>::value, "Expected &s[0] to be non-const");
    return {reinterpret_cast<unsigned char*>(&s[0]), s.size()};
}

} // namespace core123
