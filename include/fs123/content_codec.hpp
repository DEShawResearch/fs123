#pragma once
#include "sodium_allocator.hpp"
#include "secret_manager.hpp"
#include <core123/uchar_span.hpp>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <map>
#include <atomic>
#include <mutex>
#include <cstring>
#include <arpa/inet.h>  // htonl, etc.

// Sigh... crypto_secretbox_MACBYTES was added to sodium.h some time after 0.4.5...
#ifndef crypto_secretbox_MACBYTES
#define crypto_secretbox_MACBYTES (crypto_secretbox_ZEROBYTES - crypto_secretbox_BOXZEROBYTES)
#endif

// The header is provided for informational purposes.  Users of the
// API generally do not see headers.
struct fs123_secretbox_header{
    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    uint32_t recordsz_nbo;  // network byte order!
    uint8_t idlen;
    char keyid[255];
    static_assert(sizeof(keyid) == std::numeric_limits<uint8_t>::max(), "Uh oh. uint8_t can index into an array bigger than 255??");

    // Two constructors: one meant for encryptors, when we know the
    // keyid and the recordsz
    fs123_secretbox_header(const std::string& k, uint32_t recordsz){
        idlen = k.size();
        if(idlen == 0)
            throw std::runtime_error("fs123_secretbox_header::ctor: zero-length id");
        if(size_t(idlen) != k.size())
            throw std::runtime_error("fs123_secretbox_header::ctor: key too long");
        ::memcpy(keyid, k.data(), k.size());
        recordsz_nbo = htonl(recordsz);
        // !!! The nonce is uninitialized !!! 
    }

    // The other meant for decryptors, when we have a serialized
    // header in "wire format".
    fs123_secretbox_header(tcb::span<unsigned char> onwire){
        auto off = offsetof(fs123_secretbox_header, idlen);
        if(onwire.size() < off)
            throw std::runtime_error("fs123_secretbox_header::ctor - size < offsetof(idlen)");
        idlen = onwire[off];
        if(idlen == 0)
            throw std::runtime_error("fs123_secretbox_header::ctor: zero-length id");
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

    // decode takes a bytespan produced by encode and modifies it
    // in-place, returning a bytespan that contains only the original
    // plaintext.  The returned bytespan is guaranteed to be a subspan
    // of the encoded input.
    //
    // If an error occurs (e.g., the header doesn't look right, the
    // sizes are wrong, the key can't be found, the encoded data can't
    // be authenticated, etc.), a std::exception is thrown.  If an
    // exception is thrown, the data in the 'encoded' span is not
    // modified.
    static core123::padded_uchar_span
    decode(int16_t ce, core123::padded_uchar_span encoded, secret_manager& sm);

    // encode: It's tricky because we want to be able to work
    // "in-place" to avoid gratuitous copies, but we also need extra
    // space in front of the plaintext (the fs123_secretbox_header,
    // and for the MAC) and space after the plaintext (for the
    // padding).  So the plaintext input argument is a 'padded_uchar_span',
    // which "IS A" span of bytes inside a larger "bounding box".  It
    // is an error if there is not sufficient space in front and
    // behind the plaintext.
    // 
    // encode returns a padded_uchar_span that uses more of the underlying
    // "bounding box" than the input.
    //
    // Encoding requires a nonce.  With derived_nonce=false, the nonce
    // is obtained by calling libsodium's randombytes_buf.  Otherwise,
    // the nonce is obtained by hashing (libsodium's generichash) the
    // plaintext input.  The hash is keyed using bytes *starting* at
    // crypto_secretbox_KEYBYTES of the encode_sid.  I.e., the hash
    // key does not overlap with the key used for encrypting.  It is
    // an error if there are not enough bytes in the key.
    //
    // If an error is encountered, (e.g., keying problems, not enough
    // workspace, an unknown ce, etc.) encode throws a std::exception.
    // If an exception is thrown, the input span will be unchanged,
    // but other bytes in the blob may have been modified.
    static core123::padded_uchar_span
    encode(int16_t ce, const std::string& sid, secret_sp secret,
           core123::padded_uchar_span input,
           size_t pad_alignment, bool derived_nonce=false);

    static int16_t encoding_stoi(const std::string&);
    static std::string encoding_itos(int16_t);
    static std::ostream& report_stats(std::ostream&);
    static bool libsodium_initialized;
};


