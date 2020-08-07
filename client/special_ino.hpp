#pragma once

#ifndef FUSE_USE_VERSION
#error FUSE_USE_VERSION should have been -Defined in the command line
#endif
#include <fuse/fuse_lowlevel.h>
#include <core123/throwutils.hpp>

// Eventually, there may be a handful of files in the root directory
// that have "special" properties of one kind or another.  These are
// designated by "special" inos, less than or equal to
// max_special_ino.  They are handled by the *_special_ino functions,
// below.  Here's the synopsis:
//
//  ino   filename       description
//    2   .fs123_statistics    dynamically generated filesystem-wide statistics
//    3   .fs123_ioctl         ioctls only work on this file
//    4   .fs123_config        current values of configurable variables.
//    5   .fs123_server_statistics dynamically generated server-side statistics
//                                 content depends on the type of server, its
//                                 options, etc.

static const int SPECIAL_INO_STATS = 2;
static const int SPECIAL_INO_IOCTL = 3;
static const int SPECIAL_INO_CONFIG = 4;
static const int SPECIAL_INO_SERVER_STATS = 5;
static const fuse_ino_t max_special_ino = 5;

inline const char* special_ino_to_fullname(fuse_ino_t ino){
    switch(ino){
    case SPECIAL_INO_STATS:
        return ".fs123_statistics";
    case SPECIAL_INO_IOCTL:
        return ".fs123_ioctl";
    case SPECIAL_INO_CONFIG:
        return ".fs123_config";
    case SPECIAL_INO_SERVER_STATS:
        return ".fs123_server_statistics";
    default:
        throw core123::se(EINVAL, core123::fmt("special_ino_to_name(%ld) is not a recognized special ino", ino));
    }
}

// lookup returns true iff it has fully handled the request, including 'reply_entry'.
// If it returns false, the request is not 'special' and normal processing should continue.
bool lookup_special_ino(fuse_req_t req, fuse_ino_t pino, const char *name);
struct stat shared_getattr_special_ino(fuse_ino_t ino, struct fuse_file_info *fi);
void getattr_special_ino(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
void open_special_ino(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi);
void read_special_ino(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi);
void release_special_ino(fuse_req_t, fuse_ino_t ino, struct fuse_file_info* fi);
void forget_special_ino(fuse_ino_t ino, uint64_t nlookup);
