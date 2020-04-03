#define CORE123_DIAG_FLOOD_ENABLE 1
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

auto _secretbox = diag_name("secretbox");

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
    
void replace_content(padded_uchar_span& ss, size_t leader, const std::string& s){
    ss = ss.bounding_box().subspan(leader, 0);
    ss = ss.append(s);
}

struct upspan : public core123::padded_uchar_span{
    uchar_blob ub;
    upspan(size_t pre, size_t len, size_t post) : ub(pre+len+post) {
        (core123::padded_uchar_span&)(*this) = core123::padded_uchar_span(ub, pre, len);
    }
};

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

    sfname = "ut_content_codec.secrets";
    mkdir(sfname.c_str(), 0700); // no sew - EEXIST is fine.
    sffd = sew::open(sfname.c_str(), O_RDONLY|O_DIRECTORY);
    replace_sf("1",
               {{"1", "12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"}});

    // Let's encode "hello"
    sharedkeydir sm(sffd, "encode", 1);
    std::string hello = "hello";
    upspan ws1(48, hello.size(), 8);
    replace_content(ws1, 48, hello);
    auto esid = sm.get_encode_sid();
    auto esecret =  sm.get_sharedkey(esid);
    auto encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX, esid, esecret, ws1, 4);
    auto s1 = std::string(as_str_view(encoded));
    std::cout << "This should look like noise:  " << quopri(s1) << "\n";
    // Do it again - expect to get a different ciphertext because we'll get a different
    // nonce from /dev/urandom
    replace_content(ws1, 48, hello);

    encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX, esid, esecret, ws1, 4);
    auto s2 = std::string(as_str_view(encoded));
    std::cout << "And this should look like different noise:  " << quopri(s2) << "\n";
    assert(s1 != s2);
    std::cout << "OK - two encodes with derived_nonce=false are different\n";

    // Check that both of them decode back to "hello"
    auto c1 = s1;
    auto c2 = s2;
    auto d1 = content_codec::decode(content_codec::CE_FS123_SECRETBOX, as_uchar_span(c1), sm);
    auto d2 = content_codec::decode(content_codec::CE_FS123_SECRETBOX, as_uchar_span(c2), sm);
    assert(as_str_view(d1) == hello);
    assert(as_str_view(d2) == hello);
    std::cout << "OK - roundtrip with derived_nonce=false\n";
    
    // Try it again with derived_nonce=false:
    replace_content(ws1, 48, hello);
    encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX, esid, esecret, ws1, 4, true);
    auto s3 = std::string(as_str_view(encoded));
    assert(s3 != s2);
    std::cout << "OK - encodes with derived_nonce=true and false are different\n";

    replace_content(ws1, 48, hello);
    encoded = content_codec::encode(content_codec::CE_FS123_SECRETBOX, esid, esecret, ws1, 4, true);
    auto s4 = std::string(as_str_view(encoded));
    assert(s4 == s3);
    std::cout << "OK - two encodes with derived nonce are the same\n";

    // Don't unshared_copy() it this time.  Just work directly on encoded.
    auto d4 = content_codec::decode(content_codec::CE_FS123_SECRETBOX, encoded, sm);
    assert(as_str_view(d4) == hello);
    std::cout << "OK - roundtrip with derived_nonce=true\n";
    
    // Now let's try to break things...
    // First, let's check that the constructor fails when it's supposed to:
    sharedkeydir smx(-1, "encode", 10);
    // the constructor doesn't care that the file doesn't exist.  but
    // encode and decode do:
    expect_throw("secretfile does not exist (encode)", [&](){
            replace_content(ws1, 48, hello);
            smx.get_sharedkey(esid);
        });

    replace_sf("2\n",{
            {"1", "12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678 12345678\n"}});
    expect_throw("encode_sid not in sharedkeydir", [&](){
            sharedkeydir smy(sffd, "encode", 1);
            upspan b(48, hello.size(), 1);
            replace_content(b, 48, hello);
            auto s = smy.get_sharedkey("2");
            content_codec::encode(content_codec::CE_FS123_SECRETBOX, "2", s, b, 1);
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

