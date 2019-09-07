// Very simple wrapper around ThreeRoe to make it callable by Python ctypes
// because ctypes is portable across python2, python3 and pypy
// whereas a C Extension is not.
// Mark Moraes, D. E. Shaw Research
#include "core123/threeroe.hpp"

using core123::threeroe;

extern "C" {
void *tr_create(const char *s, size_t sz) {
    return new threeroe(s, sz);
}

void tr_free(void *p) {
    auto t = static_cast<threeroe *>(p);
    delete t;
}

void tr_update(void *p, const char *s, size_t sz) {
    auto t = static_cast<threeroe *>(p);
    t->update(s, sz);
}

// digest must point to at least sz (16) bytes of allocated memory
// See the caller in threeroe.py.
void tr_digest(void *p, char *digest) {
    auto t = static_cast<threeroe *>(p);
    using d_t = decltype(t->digest());
    *(d_t*)digest = t->digest();
}

// hexdigest must point to at least 33 bytes of allocated memory.
// See the caller in threeroe.py.
void tr_hexdigest(void *p, char *hexdigest) {
    auto t = static_cast<threeroe *>(p);
    std::string s = t->hexdigest();
    ::memcpy(hexdigest, s.data(), s.size()+1);
}

void *tr_copy(void *p) {
    auto ret = new threeroe("", 0);
    ::memcpy((void *)ret, p, sizeof(threeroe));
    return ret;
}
}
