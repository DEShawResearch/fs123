#include "fs123/sharedkeydir.hpp"
#include "fs123/acfd.hpp"
#include <core123/fdstream.hpp>
#include <core123/expiring.hpp>
#include <core123/unused.hpp>
#include <core123/autoclosers.hpp>
#include <core123/sew.hpp>
#include <sodium.h>
#include <mutex>

using namespace core123;

static const std::string suffix = ".sharedkey";

sharedkeydir::sharedkeydir(int dirfd_, const std::string& encode_sid_indirect_, unsigned refresh_sec) :
    dirfd(dirfd_),
    encode_sid_indirect(encode_sid_indirect_),
    secret_cache(10),
    refresh_time(std::chrono::seconds(refresh_sec))
{
    if(!legal_sid(encode_sid_indirect))
        throw std::runtime_error("sharedkeydir::sharedkeydir:  encode_sid_indirect: '" + encode_sid_indirect + "' is not a legal sid name.");
}

std::string
sharedkeydir::get_encode_sid() /*override*/{
    std::lock_guard<std::mutex> lg(mtx);
    if(!encode_sid.expired())
        return encode_sid;
    std::string ret = refresh_encode_sid();
    encode_sid = expiring<std::string>(refresh_time, ret);
    return ret;
}

secret_sp
sharedkeydir::get_sharedkey(const std::string& sid) /*override*/{
    if(!legal_sid(sid))
        throw std::runtime_error("sharedkeydir::get_sharedkey:  sid: '" + sid + "' contains illegal characters");;
    auto exsid = secret_cache.lookup(sid);
    if(!exsid.expired())
        return std::move(exsid);
    auto ret = refresh_secret(sid);
    secret_cache.insert(sid, ret, refresh_time);
    return ret;
}

void
sharedkeydir::regular_maintenance() /*override*/{
    // This may be overkill.  Logically, there's no problem with
    // leaving expired keys in the secret_cache.  But it seems like
    // good hygeine to flush them every once in a while.  Note that if
    // our libsodium is new enough, memory for our secrets is
    // de-allocated with sodium_free , which *should* zero out memory
    // before putting it back on the heap.
    secret_cache.erase_expired();
}

secret_sp
sharedkeydir::refresh_secret(const std::string& sid) /*private*/{
    // The key is in the file called <dirname>/<sid>.sharedkey so we'll
    // be ready for the day we have <sid>.pubkey and <sid>.privkey.
    if(!legal_sid(sid))
        throw std::runtime_error("sharedkeydir::refresh_secret:  sid: '" + sid + "' contains illegal characters");
    auto fname = sid + suffix;
    acfd fd = sew::openat(dirfd, fname.c_str(), O_RDONLY);
    char inbuf[1025];
    auto nread = sew::read(fd, inbuf, sizeof(inbuf));
    if(nread == sizeof(inbuf))
        throw std::runtime_error("sharedkeydir::refresh_secret: " + fname + " is too long");
    size_t vmaxsz = (nread+1)/2;
    secret_sp v( std::make_shared<secret_sp::element_type>(vmaxsz) );    
    size_t binlen;
    const char *hexend;
    int status = hex2bin(v->data(), v->size(),
                                &inbuf[0], nread,
                                ": \t\n\f", &binlen, &hexend);
    if(status!=0)
        throw std::runtime_error("sharedkeydir::refresh_secret: sodium_hex2bin failure");
    // Strict!  No trailing non-whitespace slop.
    if(hexend != &inbuf[nread])
        throw std::runtime_error("sharedkeydir::refresh_secret: non-whitespace after the key");
    v->resize(binlen);
    return v;
}

std::string
sharedkeydir::refresh_encode_sid() /*private*/{
    if(encode_sid_indirect.empty())
        throw std::runtime_error("sharedkey::refresh_encode_sid: encod_sid_indirect is empty");
    auto fname = encode_sid_indirect + ".keyid";
    acfd fd = sew::openat(dirfd, fname.c_str(), O_RDONLY);
    boost::fdistream ifs(fd);
    std::string sid;
    // Are we ok with ifs >> string?  I.e., allowing leading and
    // trailing whitespace?  Ignoring extra "stuff" after the first
    // word?
    std::string ret;
    ifs >> ret;
    if(!ifs)
        throw std::runtime_error("Error reading from " + fname);
    if(!legal_sid(ret))
       throw std::runtime_error("Illegal sid: '" + sid + "' in " + fname);
    return ret;
}

std::ostream&
sharedkeydir::report_stats(std::ostream& os){
    return os << "secret_cache_hits: " << secret_cache.hits() << "\n"
              << "secret_cache_misses: " << secret_cache.misses() << "\n"
              << "secret_cache_expirations: " << secret_cache.expirations() << "\n"
              << "secret_cache_evictions: " << secret_cache.evictions() << "\n"
              << "secret_cache_size: " << secret_cache.size() << "\n";
}
