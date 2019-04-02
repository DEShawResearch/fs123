#include "fs123/content_codec.hpp"
#include "fs123/acfd.hpp"
#include "fs123/sharedkeydir.hpp"
#include <core123/sew.hpp>
#include <core123/autoclosers.hpp>
#include <core123/exnest.hpp>
#include <core123/strutils.hpp>
#include <cassert>
#include <functional>
#include <iostream>

using namespace core123;

static std::string sfname;
static acfd sffd;

void replace_sf(const std::string& encoding_key,
                std::map<std::string, std::string> newcontents){
    std::ostringstream cmd;
    sew::system(fmt("[ -d '%s' ] && rm %s/*", sfname.c_str(), sfname.c_str()).c_str());
    auto tmpname = sfname + "/encode.keyid";
    acfd fd = sew::open(tmpname.c_str(), O_RDWR|O_TRUNC|O_CREAT, 0600);
    sew::write(fd, encoding_key.data(), encoding_key.size());
    for(auto& kv : newcontents){
        auto fname = sfname + "/" + kv.first + ".sharedkey";
        fd = sew::open(fname.c_str(), O_RDWR|O_TRUNC|O_CREAT, 0600);
        sew::write(fd, kv.second.c_str(), kv.second.size());
    }
    fd.close();
}

struct blob{
    std::unique_ptr<char[]> _buf;
    str_view _content;
    str_view _arena;
    blob(size_t leader, const std::string& content, size_t trailer){
        _buf = std::make_unique<char[]>(leader + content.size() + trailer);
        _arena = str_view(_buf.get(), leader + content.size() + trailer);
        _content = str_view(_buf.get()+leader, content.size());
        ::memcpy(const_cast<char*>(_content.data()), content.data(), content.size());
    };
    str_view arena() const { return _arena; }
    str_view content() const { return _content; }
    size_t leader() const { return _content.data() - _arena.data(); }
    void replace_content(const std::string& s){
        // no bounds checking!!!! 
        ::memcpy(const_cast<char*>(_content.data()), s.data(), s.size());
        _content = str_view(_content.data(), s.size());
    }
};

void
expect_throw(const std::string& msg, std::function<void()> f) {
    bool caught = false;
    try{
        f();
    }catch(std::exception& e){
        caught = true;
        std::cout << "OK - expected exception (" << msg << ") caught:\n";
        for(auto& v : exnest(e))
            std::cout << "\t" << v.what() << "\n";
    }
    if(!caught)
        throw std::runtime_error("Expected throw did not materialize:  " +  msg);
 }

void try_to_use_sfname(){
    sharedkeydir cc(sffd, "encode", 1);
    auto sid = cc.get_encode_sid();
    cc.get_sharedkey(sid);
}
    

int main(int /*argc*/, char **/*argv*/) try {
    // Testing for "success" is easy.  Encode something.  Check that
    // it's garbled.  Then decode it.  It's also worth checking that
    // encoding twice with derived_nonce=false produces two different
    // ciphertexts and with derived_nonce=true produces the same
    // ciphertext.  But none of that is very interesting: problems
    // with encoding and decoding will show up very quickly when the
    // class is used.
    //
    // It's much more important to test that content_codec and
    // the secret_manager fail when they should.  E.g.,
    //
    //   - missing sharedkeydir or a garbled .sharedkey file
    //   - garbage in the .sharedkey file
    //   - required secret not in sharedkeydir
    //   - ciphertext fails authentication
    //
    // Such errors are much less likely to be immediately noticed
    // in end-to-end testing.
    //
    // There are also issues related to the refresh time...  E.g., do
    // the shared pointers have the expected lifetimes?  That's pretty
    // hard to test - it needs two threads to hit a narrow window
    // in which one reloads the .sharedkey while the other is in
    // the process of using it.

    sfname = "ut_content_codec.secret.d";
    mkdir(sfname.c_str(), 0700); // no sew - EEXIST is fine.
    sffd = sew::open(sfname.c_str(), O_RDONLY|O_DIRECTORY);
    replace_sf("1",
               {{"1", "12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"}});

    // Let's encode "hello"
    sharedkeydir sm(sffd, "encode", 1);
    std::string hello = "hello";
    blob ws1(48, hello, 8);
    core123::str_view encoded;
    auto esid = sm.get_encode_sid();
    auto esecret =  sm.get_sharedkey(esid);
    encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX, esid, esecret, ws1.content(), ws1.arena(), 4);
    std::cout << "This should look like noise:  " << quopri({encoded.data(), encoded.size()}) << "\n";
    auto s1 = std::string(encoded);
    // Do it again - expect to get a different ciphertext because we'll get a different
    // nonce from /dev/urandom
    ws1.replace_content(hello);
    encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX, esid, esecret, ws1.content(), ws1.arena(), 4);
    std::cout << "And this should look like different noise:  " << quopri({encoded.data(), encoded.size()}) << "\n";
    auto s2 = std::string(encoded);
    assert(s1 != s2);
    std::cout << "OK - two encodes with derived_nonce=false are different\n";

    // Check that both of them decode back to "hello"
    auto d1 = content_codec::decode(content_codec::CE_FS123_SECRETBOX, s1, sm);
    auto d2 = content_codec::decode(content_codec::CE_FS123_SECRETBOX, s2, sm);
    assert(d1 == hello);
    assert(d2 == hello);
    std::cout << "OK - roundtrip with derived_nonce=false\n";
    
    // Try it again with derived_nonce=false:
    ws1.replace_content(hello);
    encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX, esid, esecret, ws1.content(), ws1.arena(), 4, true);
    auto s3 = std::string(encoded);
    assert(s3 != s2);
    std::cout << "OK - encodes with derived_nonce=true and false are different\n";

    ws1.replace_content(hello);
    encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX, esid, esecret, ws1.content(), ws1.arena(), 4, true);
    auto s4 = std::string(encoded);
    assert(s4 == s3);
    std::cout << "OK - two encodes with derived nonce are the same\n";

    auto d4 = content_codec::decode(content_codec::CE_FS123_SECRETBOX, s4, sm);
    assert(d4 == hello);
    std::cout << "OK - roundtrip with derived_nonce=true\n";
    
    // Now let's try to break things...
    // First, let's check that the constructor fails when it's supposed to:
    sharedkeydir smx(-1, "encode", 10);
    // the constructor doesn't care that the file doesn't exist.  but
    // encode and decode do:
    expect_throw("secretfile does not exist (encode)", [&](){
            ws1.replace_content(hello);
            smx.get_sharedkey(esid);
        });

    replace_sf("2\n",{
            {"1", "12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"}});
    expect_throw("encode_sid not in sharedkeydir", [&](){
            sharedkeydir smy(sffd, "encode", 1);
            blob b(48, "hello", 1);
            auto s = smy.get_sharedkey("2");
            content_codec::encode(content_codec::CE_FS123_SECRETBOX, "2", s, b.content(), b.arena(), 1);
        });

    replace_sf("1\n",
               {{"abc", " 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"},
                       {"1", "12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"}});
    try_to_use_sfname(); // shouldn't throw.  abc is a perfectly good secretid

    replace_sf("fred\n",{
               {"fred", "barney 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"},
               {"1", "12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"}
        });
    expect_throw("keyfile has non-hex", [&](){ try_to_use_sfname(); });

    replace_sf("1\n", {
               {"22", "12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 xyz\n"},
               {"1", " 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"}
        });
    try_to_use_sfname();  // it's not a problem to use keyid=1.
    expect_throw("non-numeric secret in secretfile", [&](){
            sharedkeydir cc(sffd, "encode", 1);
            cc.get_sharedkey("22");
        });
    
    return 0;
 }catch(std::exception& e){
    for(auto& v : exnest(e))
        std::cout << v.what() << "\n";
    std::cout << "FAIL\n";
    exit(1);
 }

