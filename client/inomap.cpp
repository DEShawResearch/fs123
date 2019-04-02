#include "inomap.hpp"
#include "app_mount.hpp"
#include <core123/complaints.hpp>
#include <core123/diag.hpp>
#include <core123/throwutils.hpp>
#include <cstring>
#include <unordered_map>
#include <set>
#include <mutex>
#include <iostream>
#include <atomic>

using namespace core123;

auto _inomap = diag_name("inomap");

namespace{

// inorecord - we have one of these for every ino with a non-zero
// reference count in inomap.  And we're still burning ~75 bytes per
// ino.  See Notes.2015, near March 26, 2015 for some insights.  It
// doesn't seem worth the trouble to try to do better.
struct inorecord{
    static const size_t BUFSZ = 12;
    union{
        char *ptr;
        char buf[BUFSZ];
    } __attribute__((packed)) u;
    int32_t refcount:31;
    uint32_t uptr:1;
    fuse_ino_t pino;
    uint64_t validator;
    const char* name() const{
        return (uptr) ? u.ptr : &u.buf[0];
    }

    inorecord(const char *from, fuse_ino_t _pino, uint64_t _validator) : refcount(0), pino(_pino), validator(_validator){
        fill_buf(from);
    }
    ~inorecord(){
        if(uptr)
            free(u.ptr);
    }
    // !!! We need a move-constructor that clears from.uptr 
    // !!! otherwise we would double-free when we delete from.
    inorecord(inorecord&& from){
        refcount = from.refcount;
        uptr = from.uptr;
        ::memcpy(&u.buf[0], &from.u.buf[0], BUFSZ);
        from.uptr = 0;
        pino = from.pino;
        validator = from.validator;
    }
    // it's not hard to implement move-assignment, but until we do,
    // don't allow the default one to be used accidentally.
    inorecord& operator=(inorecord&& from) = delete; 
    // On the other hand, it's crucial to hide the
    // copy constructor and copy-assignment operator
    // so they can't leave us with two copies of the ptr
    // to be freed on delete.
    inorecord& operator=(const inorecord&) = delete;
    inorecord(const inorecord&) = delete;
private:
    void fill_buf(const char *from){
        size_t l = ::strlen(from) + 1;
        char *to;
        if(l > sizeof(u.buf)){
            uptr = 1;
            to = u.ptr = (char *)malloc(l);
        }else{
            uptr = 0;
            to = &u.buf[0];
        }
        ::memcpy(to, from, l);
    }
};
static_assert(sizeof(inorecord) == inorecord::BUFSZ+sizeof(int32_t)+sizeof(fuse_ino_t)+sizeof(uint64_t), 
              "Oops.  Packing of inorecord doesn't look right.");

using inomap_t = std::unordered_map<uint64_t, inorecord>;
inomap_t inomap;

using inomap_mutex_t = std::mutex;
using inomap_lock_t = std::unique_lock<inomap_mutex_t>;

inomap_mutex_t inomap_mtx;

// _fullname must be called with a lock held on the inomap_mtx
// It may not be called on the inorecord of the root itself,
// i.e., the one with key=1 in the inomap.  We short-circuit
// the recursion in two places to keep that from happening.
std::string _fullname(const struct inorecord& r){
    if(r.pino==1)
        return std::string("/") + r.name();
    auto p = inomap.find(r.pino);
    if(p == inomap.end())
        throw se(EINVAL, str("couldn't find pino =", r.pino, "in inomap"));
    auto pname = _fullname(p->second);
    return pname + "/" + r.name();
}
} // namespace <anonymous>

std::string ino_to_fullname(fuse_ino_t ino){
    if(ino==1)
        return {};
    inomap_lock_t lk(inomap_mtx);
    return _fullname(inomap.at(ino));
}

std::pair<std::string, uint64_t>
ino_to_fullname_validator(fuse_ino_t ino){
    if(ino==1)
        return {{}, 1};
    inomap_lock_t lk(inomap_mtx);
    auto& r = inomap.at(ino);
    return {_fullname(r), r.validator};
}

fuse_ino_t ino_to_pino(fuse_ino_t ino) try {
    inomap_lock_t lk(inomap_mtx);
    return inomap.at(ino).pino;
 }catch(std::exception& e){
    std::throw_with_nested(std::runtime_error(strfunargs(__func__, ino)));
 }

std::pair <fuse_ino_t, std::string> ino_to_pino_name(fuse_ino_t ino) try {
    inomap_lock_t lk(inomap_mtx);
    auto& r = inomap.at(ino);
    return {r.pino, r.name()};
 }catch(std::exception& e){
    std::throw_with_nested(std::runtime_error(strfunargs( __func__, ino)));
}

uint64_t ino_update_validator(fuse_ino_t ino, uint64_t validator) try {
    inomap_lock_t lk(inomap_mtx);
    auto& ir = inomap.at(ino);
    uint64_t ret = ir.validator;
    if( proto_minor <= 1 || validator > ir.validator)
        ir.validator = validator;
    if( proto_minor > 1 && validator < ir.validator )
        throw se(EIO, fmt("ino_update_validator:  new validator (%lu) is less than cached validator (%lu).  Server is confused.", (unsigned long)validator, (unsigned long)ir.validator));
    DIAGfkey(_inomap, "update validator(ino=%lu): old: %lu, new: %lu\n", (unsigned long)ino, (unsigned long)ret, (unsigned long)validator);
    return ret;
 }catch(std::exception& e){
    std::throw_with_nested(std::runtime_error(strfunargs(__func__, ino, ino_to_fullname_nothrow(ino))));
 }

uint64_t ino_get_validator(fuse_ino_t ino) try {
    inomap_lock_t lk(inomap_mtx);
    return inomap.at(ino).validator;
 }catch(std::exception& e){
    std::throw_with_nested(std::runtime_error(strfunargs(__func__, ino)));
 }

void ino_remember(fuse_ino_t pino, const char *name, fuse_ino_t ino, uint64_t validator){
    inomap_lock_t lk(inomap_mtx);
    auto iterbool = inomap.emplace(std::piecewise_construct,
                   std::forward_as_tuple(ino), 
                   std::forward_as_tuple(name, pino, validator));
    if(!iterbool.second){
        // there was already an entry for ino in the map.  Let's
        // make sure that it's for the same name.
        if( strcmp(name, iterbool.first->second.name()) )
            throw se(EINVAL, fmt("ino_remember(pino=%lu, name=%s, ino=%lu) does not match existing record in inomap with name=%s",
                                         pino, name, ino, iterbool.first->second.name()));
    }
    iterbool.first->second.refcount++;
    DIAGfkey(_inomap, "inomap_remember(%lu, %s, %lu, %lu) refcount: %d\n", pino, name, ino, validator, iterbool.first->second.refcount);
}

void ino_forget(fuse_ino_t ino, uint64_t nlookup){
    inomap_lock_t lk(inomap_mtx);
    auto p = inomap.find(ino);
    if(p==inomap.end()){
        complain("forget(%lu):  can't find ino in inomap", ino);
        return;
    }
    if(p->second.refcount == 0)
        complain("forget(%lu) refcount was zero", ino);
    p->second.refcount -= nlookup;
    DIAGfkey(_inomap, "inomap_forget(%lu) refcount: %d\n", ino, p->second.refcount);
    if(p->second.refcount < 0){
        complain("forget(%lu) refcount < 0", ino);
        p->second.refcount = 0;
    }
    if( p->second.refcount == 0 ){
        inomap.erase(p);
    }
}

size_t ino_count(){
    inomap_lock_t lk(inomap_mtx);
    return inomap.size();
}
        
