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
std::string
content_codec::decode(int16_t ce, const std::string& message,
                      secret_manager& sm){
    DIAGfkey(_secretbox, "content_codec::decode(%d, message[%zu])\n",
             ce, message.size());
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
    // with CentOS6 doesn't have the _easy API.  We're forced to do a little extra
    // copying and a lot of extra index arithmetic.
    if(message.size() < crypto_secretbox_BOXZEROBYTES + crypto_secretbox_NONCEBYTES)
        throw std::runtime_error("encrypted content size: " + std::to_string(message.size()) + " shorter than crypto_secret_ZEROBOXBYTES + crypto_secretbox_NONCEBYTES");
    // to make it easier to compare, we use names (c, m, clen) that
    // correspond to the docs: https://nacl.cr.yp.to/secretbox.html
    fs123_secretbox_header hdr(message);
    std::string keyid = hdr.get_keyid();
    size_t clen = hdr.get_recordsz() + crypto_secretbox_BOXZEROBYTES;
    auto key = sm.get_sharedkey(keyid);
    if(key->size() < crypto_secretbox_KEYBYTES)
        throw std::runtime_error(fmt("secret[%s] is too short (%zu), needed %u",
                                              keyid.c_str(), key->size(), crypto_secretbox_KEYBYTES));
    unsigned char m[clen];
    // DANGER: We're modifying the message here to satisfy the
    // idiosyncratic demands of crypto_secretbox_open.  Then when
    // we're done with crypto_secretbox_open, we'll put the original
    // data back.  crypto_secretbox_open is in C, so it can't throw,
    // so this should be perfectly safe (?!).
    if( hdr.wiresize() < crypto_secretbox_BOXZEROBYTES)
        throw std::runtime_error("fs123_secretbox_hdr smaller than BOXZEROBYTES?  How?");
    auto c = reinterpret_cast<unsigned char const*>(message.data() + hdr.wiresize() - crypto_secretbox_BOXZEROBYTES);
    bzero(const_cast<unsigned char*>(c), crypto_secretbox_BOXZEROBYTES);
    auto ret = crypto_secretbox_open(m, c, clen, hdr.nonce, key->data());
    ::memcpy(const_cast<char*>(&message[0]), &hdr, hdr.wiresize());
    if(0 != ret){
        stats.secretbox_auth_failures++;
        DIAGfkey(_secretbox, "crypto_secretbox_open failed!\n");
        throw std::runtime_error(fmt("message forged, msglen=%zu, secret=%s key[0]=%u", clen, keyid.c_str(), (*key)[0]));
    }
    // Check for the pad byte(s). They must be 0x2 followed by zero or more NULs. 
    auto p = &m[clen];
    auto m0 = &m[crypto_secretbox_ZEROBYTES];
    while( p>m0 && *--p == '\0')
        ;
    if(*p != 0x2)
        throw std::runtime_error("mal-formed or missing pad-bytes at end of message");

    if(key.use_count() == 1)
        stats.secretbox_disappearing_secrets++;
    stats.secretbox_bytes_decrypted += clen;
    stats.secretbox_blocks_decrypted++;
    DIAGfkey(_secretbox, "content_codec::decoded: %s\n", quopri({(const char*)m0, size_t(p-m0)}).c_str());
    return {reinterpret_cast<char*>(m0), size_t(p-m0)};
}

str_view
content_codec::encode(int16_t ce, const std::string& sid,
                      secret_sp secret, str_view input,
                      str_view workspace,
                      size_t pad_alignment, bool derived_nonce){
    if(ce == CE_IDENT)
        return input;
    atomic_scoped_nanotimer _t(&stats.secretbox_encrypt_sec);
    // If the current encoding_sid is the designated
    // DO_NOT_ENCODE_SID, then return the input unchanged
    if(sid == secret_manager::DO_NOT_ENCODE_SID)
        return input;

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
    if(workspace.data() > input.data() - hdr.wiresize() - crypto_secretbox_MACBYTES)
        throw std::invalid_argument(fmt("content_codec::encode:  not enough space between start of workspace and start of input for header and MAC, workspace-input: %td, hdr.wiresize: %zd, MACBYTES: %d", workspace.data() - input.data(), hdr.wiresize(), crypto_secretbox_MACBYTES));
    if(input.data() + (input.size() + padding) > workspace.data() + workspace.size())
        throw std::invalid_argument("content_codec::encode:  not enough space after end for padding");
        
    // DANGER - this is where we play fast and loose with the constness of input.data()!
    unsigned char *plaintext = reinterpret_cast<unsigned char*>(const_cast<char*>(input.data()));
    unsigned char *c = plaintext - crypto_secretbox_ZEROBYTES;
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
    if(0 != crypto_secretbox(c, c, msz, hdr.nonce, secret->data()))
        throw std::runtime_error("crypto_secretbox:  failed to authenticate/decode message");
    if(secret.use_count() == 1)
        stats.secretbox_disappearing_secrets++;
    ::memcpy(plaintext-crypto_secretbox_MACBYTES-hdr.wiresize(), &hdr, hdr.wiresize());
    DIAGfkey(_secretbox, "plain=%zu@%p\n", input.size(), plaintext);
    stats.secretbox_blocks_encrypted++;
    stats.secretbox_bytes_encrypted += msz;
    return {input.data() - crypto_secretbox_MACBYTES - hdr.wiresize(),
            recordsz + hdr.wiresize()};
}

std::ostream& content_codec::report_stats(std::ostream& os) /*static*/ {
    return os << stats;
}
