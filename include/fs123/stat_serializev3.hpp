#ifndef _stat_serializev3_dot_hpp_
#define _stat_serializev3_dot_hpp_

#include <core123/svto.hpp>
#include <iostream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <cstring>

// These don't change very often.  But when they do, be very careful:
//  - make sure the order is the same on the insertion<< and the svScan.
//  - be very careful about mismatching protocols on the clients and
//    servers.  For now (April, 2017), the scanners are tolerant
//    of extra info.  If done carefully, that should allow server-side
//    upgrades which add new fields, to precede client-side upgrades
//    which require new fields.

#ifdef __APPLE__
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#define st_ctim st_ctimespec
#endif
inline std::ostream& operator<<(std::ostream& os, const struct stat& sb){
    return os << sb.st_mode << " " << sb.st_nlink << " " << sb.st_uid << " " << sb.st_gid << " " << sb.st_size << " " << sb.st_mtime << " " << sb.st_ctime << " " << sb.st_atime << " " << sb.st_ino << " " << sb.st_mtim.tv_nsec << " " << sb.st_ctim.tv_nsec << " " << sb.st_atim.tv_nsec << " " << sb.st_dev << " " << sb.st_blocks << " " << sb.st_blksize << " "  << sb.st_rdev;
}

namespace core123{
template <>
inline size_t
svscan(str_view sv, struct stat* sb, size_t start){
    ::memset(sb, 0, sizeof(struct stat));
    return svscan(sv, std::tie(sb->st_mode, sb->st_nlink, sb->st_uid, sb->st_gid, sb->st_size, sb->st_mtime, sb->st_ctime, sb->st_atime, sb->st_ino, sb->st_mtim.tv_nsec, sb->st_ctim.tv_nsec, sb->st_atim.tv_nsec, sb->st_dev, sb->st_blocks, sb->st_blksize, sb->st_rdev), start);
}
} // namespace core123

inline std::ostream& operator<<(std::ostream& os, const struct statvfs& sb){
    return os << sb.f_bsize << " " << sb.f_frsize << " " << sb.f_blocks << " " << sb.f_bfree << " " << sb.f_bavail << " " << sb.f_files << " " << sb.f_ffree << " " << sb.f_favail << " " << sb.f_fsid << " " << sb.f_flag << " " << sb.f_namemax;
}

namespace core123{
template <>
inline size_t
svscan(str_view sv, struct statvfs* svfsp, size_t start){
    ::memset(svfsp, 0, sizeof(struct statvfs));
    return svscan(sv, std::tie(svfsp->f_bsize, svfsp->f_frsize, svfsp->f_blocks, svfsp->f_bfree, svfsp->f_bavail, svfsp->f_files, svfsp->f_ffree, svfsp->f_favail, svfsp->f_fsid, svfsp->f_flag, svfsp->f_namemax), start);
}
} // namespace core123

#endif
