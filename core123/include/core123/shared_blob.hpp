// Sometimes we want a blob of bytes.  It turns out that (in C++17)
//   blob = new std::byte[N]
// is a pretty reliable way to get such a blob.
//
// The main advantage over string and vector<byte> is that it doesn't
// insist on zero-initializing.  The disadvantage is that it doesn't
// carry the length along with it.
//
// Furthermore, bare new is "never" a good idea.
//
// So:
// A "unique_blob" is a unique_ptr<byte[]> and a length.
// A "shared_blob" is a shared_ptr<byte[]> and a length.

// Note that the standard library supports shared pointers to arrays
// only in C++17.  There's some (possibly misguided)
// backward-compatibility code here.  *IF* it ever becomes a problem,
// seriously consider just tossing the conditional logic overboard and
// reverting to:
//
// #if __cpplib_shared_ptr_arrays >= 201611L
// #error This code needs shared pointer arrays, e.g., shared_ptr<byte[]>
// #endif

#pragma once
#include "span.hpp"
#include <cstddef>
#include <memory>
#if __has_include(<version>)
#include <version>
#endif

namespace core123{

template <typename SMART_PTR>
class _blob{
    size_t len;
    SMART_PTR bytes;
public:
    using blob_type = SMART_PTR;
    _blob() : len{}, bytes{}{}
#if __cpp_lib_shared_ptr_arrays >= 201611L
    _blob(size_t _len) : len(_len), bytes(new unsigned char[_len]){}
#else
    // Pre-17, we have to provide a custom-deleter.
    _blob(size_t _len) : len(_len), bytes(new unsigned char[_len], std::default_delete<unsigned char[]>()){}
#endif
    _blob(SMART_PTR _bytes, size_t _len) : len(_len), bytes(_bytes){}
    size_t size() const { return len; }
    unsigned char* data() const {
        return bytes.get();
    }
    const SMART_PTR& sp() const { return bytes; }
    tcb::span<unsigned char> as_span() const { return {data(), size()}; }
};
    
using unique_blob = _blob<std::unique_ptr<unsigned char[]>>;
#if __cpp_lib_shared_ptr_arrays >= 201611L
using shared_blob = _blob<std::shared_ptr<unsigned char[]>>;
#else
// pre-17, the element_type cannot be an array
using shared_blob = _blob<std::shared_ptr<unsigned char>>;
#endif

}
