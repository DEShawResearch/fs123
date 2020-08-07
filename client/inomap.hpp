#pragma once

#ifndef FUSE_USE_VERSION
#error FUSE_USE_VERSION should have been -Defined in the command line
#endif
#include <fuse/fuse_lowlevel.h>
#include <vector>
#include <string>
#include <iosfwd>
#include <utility>

std::string ino_to_fullname(fuse_ino_t ino); 
inline std::string ino_to_fullname_nothrow(fuse_ino_t ino) try {
    return ino_to_fullname(ino);
}catch(...){
    return std::string("//not_in_inomap");
}
fuse_ino_t ino_to_pino(fuse_ino_t ino);
std::pair <fuse_ino_t, std::string> ino_to_pino_name(fuse_ino_t ino);
std::pair <std::string, uint64_t> ino_to_fullname_validator(fuse_ino_t ino);
void ino_remember(fuse_ino_t pino, const char *name, fuse_ino_t ino, uint64_t validator);
void ino_forget(fuse_ino_t ino, uint64_t nlookup);
uint64_t ino_update_validator(fuse_ino_t ino, uint64_t validator);
uint64_t ino_get_validator(fuse_ino_t ino);

size_t ino_count();
