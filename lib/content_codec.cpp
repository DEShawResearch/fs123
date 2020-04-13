#define CORE123_DIAG_FLOOD_ENABLE 1
#include "fs123/content_codec.hpp"
#include <sodium.h>
#include <core123/diag.hpp>
#include <core123/stats.hpp>
#include <core123/scoped_nanotimer.hpp>
#include <core123/strutils.hpp>
#include <memory>
#include <string>

using namespace core123;

namespace{
#define STATS_INCLUDE_FILENAME "codec_statistic_names"
#define STATS_STRUCT_TYPENAME codec_stats_t
#include <core123/stats_struct_builder>
codec_stats_t stats;
    
auto _secretbox = diag_name("secretbox");

bool static_initialize_libsodium(){
    // Should we be more harsh if sodium_init returns non-zero?  E.g.,
    // throw an exception?  It would be hard to catch, and therefore
    // hard to work around if, for example, we weren't intending to
    // use libsodium, didn't care that it failed to initialize.
    return sodium_init() != -1;
}

} // namespace <anon>

/*static*/ bool
content_codec::libsodium_initialized = static_initialize_libsodium();

int16_t
content_codec::encoding_stoi(const std::string& encoding) /*static*/{
    // FIXME - this ignores the ;q=Number clause of the accept-encoding
    // string.  It returns the wrong result for something like:
    //     Accept-encoding:  fs123-secretbox;q=0
    if(encoding.find("fs123-secretbox") != std::string::npos)
        return CE_FS123_SECRETBOX;
    if(encoding.empty() || encoding.find("identity") != std::string::npos)
        return CE_IDENT;
    // N.B.  it's perfectly normal to return CE_UNKNOWN.  A cache
    // might tell us that *it's* willing to accept "gzip" encoding.
    // The fact that we've never heard of "gzip" encoding is not an
    // error.
    return CE_UNKNOWN;
}

std::string
content_codec::encoding_itos(int16_t encoding) /*static*/{
    switch(encoding){
    case CE_IDENT:
        return "";
    case CE_FS123_SECRETBOX:
        return "fs123-secretbox";
    case CE_UNKNOWN:
        return "unknown-encoding";
    }
    throw std::invalid_argument("content_codec::encoding_itos");
}

// We don't really specialize to the fs123-secretbox encoding until
// we get to decode and encode.  In theory, we could support
// other encodings, by branching in encode and decode.
//
// The fs123-secretbox format is modeled after rfc8188.  The "wire format"
// is in the fs123_secretbox_header:
//
//   | nonce(24) | recordsz(4) | secretidlen(2)=0x2 | secretid(2) | ciphertext(recordsz) |
//
// The recordsz, secretidlen and secretid are in network byte order.
core123::padded_uchar_span
content_codec::decode(int16_t ce, core123::padded_uchar_span message,
                      secret_manager& sm){
    DIAGfkey(_secretbox, "content_codec::decode(%d, message[%zu]@%p)\n",
             ce, message.size(), message.data());
    switch(ce){
    case CE_IDENT:
        DIAGfkey(_secretbox, "decode(CE_IDENT):  returning message unmodified");
        return message;
    case CE_UNKNOWN:
        throw std::runtime_error("cannot decode reply with unrecognized encoding");
    case CE_FS123_SECRETBOX:
        break; // fall through...
    default:
        throw std::logic_error("This can't happen.  reply.encoding isn't even CE_UNKNOWN");
    }
    
    atomic_scoped_nanotimer _t(&stats.secretbox_decrypt_sec);
    // N.B.  We're using the 'non-easy', original-djb-recipe crypto_secretbox API
    // because we  want to compile and link on CentOS6 and the libsodium that ships
    // with CentOS6 doesn't have the _easy API.
    // <snip https://libsodium.gitbook.io/doc/secret-key_cryptography/secretbox> 
    // crypto_secretbox_open() takes a pointer to 16 bytes before the
    // ciphertext and stores the message 32 bytes after the
    // destination pointer, overwriting the first 32 bytes with zeros.
    // </snip>
    // 16 == crypto_secretbox_BOXZEROBYTES
    // 32 == crypto_secretbox_ZEROBYTES
    static_assert(crypto_secretbox_ZEROBYTES ==32 && crypto_secretbox_BOXZEROBYTES==16, "These are not the boxes we're looking for...");
    fs123_secretbox_header hdr(message); // throws if message too small
    DIAGf(_secretbox, "decode:  hdr.wiresize(): %zd hdr.get_recordsz(): %d\n", hdr.wiresize(), hdr.get_recordsz());
    std::string keyid = hdr.get_keyid();
    auto ciphertext_len = hdr.get_recordsz();
    if( ciphertext_len != message.size() - hdr.wiresize() )
        throw std::runtime_error("content_codec::decode:  Header is garbled.  hdr.get_recordsz() != message.size() - hdr.wiresize()");
    auto key = sm.get_sharedkey(keyid);
    if(key->size() < crypto_secretbox_KEYBYTES)
        throw std::runtime_error(fmt("secret[%s] is too short (%zu), needed %u",
                                              keyid.c_str(), key->size(), crypto_secretbox_KEYBYTES));
    // DANGER: We're assuming the crypto_secretbox_open works in-place
    // with overlapping 'm' and 'c' buffers.
    auto cstart = message.data() + hdr.wiresize();
    auto padded_cstart = cstart - crypto_secretbox_BOXZEROBYTES;
    size_t padded_len = ciphertext_len + crypto_secretbox_BOXZEROBYTES;
    auto plainstart = padded_cstart + crypto_secretbox_ZEROBYTES;
    // N.B.  NaCl's docs say we must zero out the first 16 bytes of
    // input.  Libsodiums do not, and code inspection confirms it.  So
    // there's no need for bzero.
    // N.B.  The "strong exception guarantee" relies on the belief
    // that crypto_secretbox_open does not modify any data if it
    // returns non-zero.
    auto ret = crypto_secretbox_open(padded_cstart, padded_cstart, padded_len, hdr.nonce, key->data());
    if(0 != ret){
        stats.secretbox_auth_failures++;
        DIAGfkey(_secretbox, "crypto_secretbox_open failed!\n");
        throw std::runtime_error(fmt("message forged, msglen=%zu, secret=%s key[0]=%u", padded_len, keyid.c_str(), (*key)[0]));
    }
    // Check for the pad byte(s). They must be 0x2 followed by zero or more NULs. 
    auto pend = &cstart[hdr.get_recordsz()];
    while( pend>plainstart && *--pend == '\0')
        ;
    if(*pend != 0x2)
        throw std::runtime_error("mal-formed or missing pad-bytes at end of message");

    if(key.use_count() == 1)
        stats.secretbox_disappearing_secrets++;
    stats.secretbox_bytes_decrypted += padded_len;
    stats.secretbox_blocks_decrypted++;
    DIAGfkey(_secretbox, "content_codec::decoded: %s\n", quopri({(const char*)plainstart, std::min(size_t(512), size_t(pend-plainstart))}).c_str());
    return {message, size_t(plainstart-message.data()), size_t(pend-plainstart)};
}

padded_uchar_span
content_codec::encode(int16_t ce, const std::string& sid,
                      secret_sp secret, padded_uchar_span input,
                      size_t pad_alignment, bool derived_nonce){
    if(ce == CE_IDENT)
        return input;
    atomic_scoped_nanotimer _t(&stats.secretbox_encrypt_sec);

    if(secret->size() < crypto_secretbox_KEYBYTES)
        throw std::runtime_error(fmt("secret[%s] is too short (%zu), needed %u",
                                              sid.c_str(), secret->size(), crypto_secretbox_KEYBYTES));
        
    // The only encoding we understand is fs123-secretbox:
    if(ce != CE_FS123_SECRETBOX)
        throw std::invalid_argument("content_codec::encode only understands the identity and fs123-secretbox encodings");

    // padding
    size_t padding =  pad_alignment - (input.size() % pad_alignment);
    auto recordsz = crypto_secretbox_MACBYTES + input.size() + padding;
    auto msz = crypto_secretbox_ZEROBYTES + input.size() + padding;
    // Work in-place.  But make sure we've got enough padding on the front and
    // back of the content:
    fs123_secretbox_header hdr(sid, recordsz);
    if(input.avail_front() < hdr.wiresize() + crypto_secretbox_MACBYTES)
        throw std::invalid_argument("content_codec::encode:  not enough space to prepend header and MAC");
    if(input.avail_back() < padding)
        throw std::invalid_argument("content_codec::encode:  not enough space after end for padding");
        
    auto plaintext = input.data();
    auto c = plaintext - crypto_secretbox_ZEROBYTES;
    ::bzero(c, crypto_secretbox_ZEROBYTES);
    plaintext[input.size()] = 0x2; // first pad-byte is 0x2
    ::bzero(plaintext + input.size()+1, padding-1); // remaining pad bytes (if any) are 0x0

    // N.B.  in libsodium 1.0.5 through 1.0.13 (and probably more) randombytes_buf
    // returns void.  It does *not* have an error return.  I think it aborts if
    // it can't generate  random bytes, but I'm not sure.  A security review
    // has pointed out that this behavior isn't optimal,
    //
    //    https://www.privateinternetaccess.com/blog/2017/08/libsodium-v1-0-12-and-v1-0-13-security-assessment/
    // 
    // but it's not clear whether it will be changed.
    if(derived_nonce){
        // Use the bytes *after* crypto_secretbox_KEYBYTES for the key.
        auto keybytes = secret->size() - crypto_secretbox_KEYBYTES;
        if(keybytes < crypto_generichash_KEYBYTES_MIN)
            throw std::runtime_error(fmt("Not enough bytes in key %zu to derive a nonce %u.\n",
                                                  secret->size(), crypto_secretbox_KEYBYTES + crypto_generichash_KEYBYTES_MIN));
        keybytes = std::min(keybytes, size_t(crypto_generichash_KEYBYTES_MAX));
        auto key = secret->data() + crypto_secretbox_KEYBYTES;
        crypto_generichash(hdr.nonce, crypto_secretbox_NONCEBYTES,
                           plaintext, input.size(),
                           key, keybytes); 
    }else{
        randombytes_buf(hdr.nonce, crypto_secretbox_NONCEBYTES); // in libsodium
    }
    if( hdr.wiresize() < crypto_secretbox_BOXZEROBYTES)
        throw std::runtime_error("fs123_secretbox_hdr smaller than BOXZEROBYTES.  How??" );
    // <snip https://libsodium.gitbook.io/doc/secret-key_cryptography/secretbox>
    // crypto_secretbox() takes a pointer to 32 bytes before the
    // message, and stores the ciphertext 16 bytes after the
    // destination pointer, the first 16 bytes being overwritten with
    // zeros.
    // </snip>
    // 16 == crypto_secretbox_BOXZEROBYTES
    // 32 == crypto_secretbox_ZEROBYTES
    static_assert(crypto_secretbox_ZEROBYTES ==32 && crypto_secretbox_BOXZEROBYTES==16, "These are not the boxes we're looking for...");
    if(0 != crypto_secretbox(c, c, msz, hdr.nonce, secret->data()))
        throw std::runtime_error("crypto_secretbox:  failed to authenticate/decode message");
    if(secret.use_count() == 1)
        stats.secretbox_disappearing_secrets++;
    ::memcpy(plaintext-crypto_secretbox_MACBYTES-hdr.wiresize(), &hdr, hdr.wiresize());
    DIAGfkey(_secretbox, "plain=%zu@%p\n", input.size(), plaintext);
    stats.secretbox_blocks_encrypted++;
    stats.secretbox_bytes_encrypted += msz;
    return input.subspan(-ssize_t(crypto_secretbox_MACBYTES + hdr.wiresize()), recordsz + hdr.wiresize());
}

std::ostream& content_codec::report_stats(std::ostream& os) /*static*/ {
    return os << stats;
}
