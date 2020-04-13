#pragma once

#include <string>
#include <vector>
#include <memory>
#include "sodium_allocator.hpp"

// A secret is a vectors<unsigned char>, with a fancy allocator
// that uses the libsodium's "secure memory utilities".  Thus, the
// secret's data is allocated on a protected (not swapped, etc.)
// page, and when the secret is destroyed, sodium_free is called
// to sanitize the memory.  This *seems like* a "best practice",
// but it's not clear how much it costs or how much it buys us or
// even whether we've been sufficiently dilligent to actually
// benefit from it.
using secret_t = std::vector<unsigned char, sodium_allocator<unsigned char>>;
// We manage collections of secrets in a map whose mapped_type is
// a shared_pointer to a secret (see above).  A thread wishing to
// use a secret should assign the mapped value (copy semantics)
// into its own secret_sp and should find the secret's bytes at
// sp->data().  The shared_ptr guarantees that the owned object
// (the secret) will outlive the map.  The indirection through
// data() means that the secret bytes won't be copied to elsewhere
// in the thread's memory.
using secret_sp = std::shared_ptr<secret_t>;

struct secret_manager {
    virtual std::string get_encode_sid() = 0;
    virtual secret_sp get_sharedkey(const std::string& sid) = 0;
    virtual void regular_maintenance(){}
    virtual std::ostream& report_stats(std::ostream& os){return os;}
    virtual ~secret_manager(){}
    // hex2bin forwards to sodium_hex2bin, unless our version of
    // libsodium is too old, in which case it uses a private
    // implementation.
    static int hex2bin(unsigned char *const bin, const size_t bin_maxlen,
                        const char *const hex, const size_t hex_len,
                        const char *const ignore, size_t *const bin_len,
                       const char **const hex_end);
    static bool legal_sid(const std::string& sv);
};    

