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
    t->Update(s, sz);
}

// digest must point to at least sz (16) bytes of allocated memory
void tr_digest(void *p, char *digest, size_t sz) {
    auto t = static_cast<threeroe *>(p);
    t->Final().digest(reinterpret_cast<unsigned char*>(digest), sz);
}

// hexdigest must point to at least sz (33) bytes of allocated memory
void tr_hexdigest(void *p, char *hexdigest, size_t sz) {
    if(sz==0) return;
    auto t = static_cast<threeroe *>(p);
    auto nwritten = t->Final().hexdigest(hexdigest, sz);
    hexdigest[nwritten] = '\0'; // make sure it's NUL-terminated
}

void *tr_copy(void *p) {
    auto ret = new threeroe("", 0);
    memcpy((void *)ret, p, sizeof(threeroe));
    return ret;
}
}
