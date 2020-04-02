#pragma once

#include "sew.hpp"
#include "autoclosers.hpp"
#include "intutils.hpp"
#include <stdexcept>

namespace core123{

// Note - there are far more complete uuid libraries out there.  But
// there's no need for external linkage if all you need is a 'random'
// uuid from /dev/urandom.
// 
//    See RFC4122/DCE 1.1
//
// Generate a Version=4 Variant=1 UUID.
// Version=4 means "random"
// Variant=1 means the binary representation is big-endian, but
// since we're only returning the text form, the only implication
// is that the upper two bits of byte 16 are 0b10xx
inline
std::string gen_random_uuid(){
    ac::fd_t<> fd = sew::open("/dev/urandom", O_RDONLY);
    unsigned char buf[16]; // exactly 16 bytes.  No more.  No less
    auto nread = sew::read(fd, buf, 16); 
    if(nread != 16)
        throw std::runtime_error("gen_uuid_random:  Short read from /dev/urandom");
    fd.close();
    buf[6] = (buf[6]&0xf) | 0x40;  // M=4  (Version 4, random)
    buf[8] = (buf[8]&0x3f) | 0x80;  // N=0b10xx (Variant 2)
    std::string ret(36, '\0');  // exactly 36 bytes.  No more.  No less
    char *p = &ret[0];
    char *e = p+36;
    int i = 0;
    while(p < e){
        if(i==4 || i==6 || i==8 || i==10)
            *p++ = '-';
        *p++ = hexlownibble(buf[i]>>4);
        *p++ = hexlownibble(buf[i]);
        ++i;
    }
    return ret;
}

}
