#pragma once
#include "sodium_allocator.hpp"
#include "secret_manager.hpp"
#include <core123/str_view.hpp>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <map>
#include <atomic>
#include <mutex>
#include <cstring>
#include <arpa/inet.h>  // htonl, etc.

// The header is provided for informational purposes.  Users of the
// API generally do not see headers.
struct fs123_secretbox_header{
    uint8_t nonce[crypto_secretbox_NONCEBYTES];
    uint32_t recordsz_nbo;  // network byte order!
    uint8_t idlen;
    char keyid[255];
    static_assert(sizeof(keyid) == std::numeric_limits<uint8_t>::max(), "Uh oh. uint8_t can index into an array bigger than 255??");

    // Two constructors: one meant for encryptors, when we know the
    // keyid and the recordsz
    fs123_secretbox_header(const std::string& k, uint32_t recordsz){
        idlen = k.size();
        if(size_t(idlen) != k.size())
            throw std::runtime_error("fs123_secretbox_header::setkeyid: key too long");
        ::memcpy(keyid, k.data(), k.size());
        recordsz_nbo = htonl(recordsz);
        // !!! The nonce is uninitialized !!! 
    }

    // The other meant for decryptors, when we have a serialized
    // header in "wire format".
    fs123_secretbox_header(core123::str_view onwire){
        idlen = onwire.at(offsetof(fs123_secretbox_header, idlen));
        size_t len = wiresize();
        if( onwire.size() < len)
            throw std::runtime_error("fs123_secretbox_header::ctor - not enough input bytes");
        ::memcpy(&nonce[0], onwire.data(), len);
    }

    size_t wiresize() const {
        return idlen + offsetof(fs123_secretbox_header, keyid[0]);
    }

    std::string get_keyid() const{
        return {keyid, idlen};
    }

    uint32_t get_recordsz() const{
        return ntohl(recordsz_nbo);
    }
};
static_assert(sizeof(fs123_secretbox_header)==284, "Weird padding?  This code is incorrect if there's internal padding in fs123_secretbox_header!");

struct content_codec{
    enum {                      // possible values for content_encoding
        CE_IDENT=1,
        CE_FS123_SECRETBOX,
        CE_UNKNOWN};

    static std::string decode(int16_t ce, const std::string& encoded, secret_manager& sm);

    // encode: It's tricky because we want to be able to work
    // "in-place" to avoid gratuitous copies (premature
    // optimization??).  So the arguments are the 'input' string_view
    // pointing to the plaintext, and a 'workspace' string_view, which
    // must contain the input, and enough extra space at the front and
    // back for headers, padding, etc.  WARNING: encode treats the
    // workspace as writeable, regardless of the declared const-ness
    // of the value returned by workspace.data().
    //
    // encode returns a pair containing a string_view pointing to the
    // encoded data, and a boolean saying whether it actually
    // performed the requested encoding.  Failure to perform the
    // encoding is *not* an error; e.g., if the current value of
    // encode_sid is 0.  If return.second is false, then return.first
    // is the same as the original input and the entire workspace
    // (which includes the entire input) is unmodified.  If
    // return.second is true, then return.first points somewhere in
    // the workspace, and encode *may* have modified *any* byte in the
    // workspace.

    // With derived_nonce=false, the nonce is obtained by calling
    // libsodium's randombytes_buf.  Otherwise, the nonce is obtained
    // by hashing (libsodium's generichash) the plaintext input.  The
    // hash is keyed using bytes *starting* at
    // crypto_secretbox_KEYBYTES of the encode_sid.  I.e., the hash
    // key does not overlap with the key used for encrypting.
    //
    // If an error is encountered, (e.g., keying problems, not enough
    // workspace, an unknown ce, etc.) encode throws a std::exception.
    // If an exception is thrown, the input will be unchanged, but
    // other bytes in the workspace may have been modified.
    static core123::str_view
    encode(int16_t ce, const std::string& sid, secret_sp secret,
           core123::str_view input,
           core123::str_view workspace,
           size_t pad_alignment, bool derived_nonce=false);

    static int16_t encoding_stoi(const std::string&);
    static std::string encoding_itos(int16_t);
    static std::ostream& report_stats(std::ostream&);
    static bool libsodium_initialized;
};


