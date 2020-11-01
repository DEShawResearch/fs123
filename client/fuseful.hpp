// fuseful.hpp - A collection of fuse-specific wrappers, helpers and
// convenience functions.
#pragma once

#ifndef FUSE_USE_VERSION
#error FUSE_USE_VERSION should have been -Defined in the command line
#endif
#include <core123/diag.hpp>
#include <fuse/fuse_lowlevel.h>
#include <core123/complaints.hpp>
#include <core123/str_view.hpp>
#include <iostream>
#include <atomic>

// Keep track of the net (opens - releases)
extern std::atomic<int> fuseful_net_open_handles;

// The fuse_reply_xxx functions return -errno
// of the writev call made by fuse_kern_chan_send to
// carry out the final transmission of our reply to the kernel.
// There is a comment in fuse_kern_chan.c that says:
//   /* ENOENT means the operation was interrupted */
// It's not clear what (if anything) the daemon can or should do with
// that.  The NTFS-3G filesystem is the most "professional" example of
// a fuse filesystem I can find, and they just ignore the value
// returned by fuse_reply_xxx.  Maybe we'll never see it?  Let's at
// least log them, so we'll have some data that justifies our decision
// to ignore.
inline void reply_err(fuse_req_t req, int eno){
    static auto _err = core123::diag_name("err");
    if(eno)
        DIAGfkey(_err, "reply_err(req=%p, eno=%d)\n", req, eno);
    int ret = fuse_reply_err(req, eno);
    if( ret )
        core123::complain("fuse_%s(%d) failed with ret=%d", __func__, eno, ret);
}

inline void reply_entry(fuse_req_t req, const fuse_entry_param* e){
    static auto _lookup = core123::diag_name("lookup");
    DIAGfkey(_lookup, "reply_entry(ino=%lu, entry_to=%f, attr_to=%f)\n", 
             e->ino, e->entry_timeout, e->attr_timeout);
    int ret = fuse_reply_entry(req, e);
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_attr(fuse_req_t req, const struct stat *s, float timeout){
    int ret = fuse_reply_attr(req, s, timeout);
    static auto _getattr = core123::diag_name("getattr");
    DIAGfkey(_getattr, "reply_attr(mode=%o, timeout=%f)\n", s->st_mode, timeout);
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_xattr(fuse_req_t req, size_t len){
    int ret = fuse_reply_xattr(req, len);
    static auto _xattr = core123::diag_name("xattr");
    DIAGfkey(_xattr, "reply_xattr(len=%zu)\n", len);
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_buf(fuse_req_t req, const char *buf, size_t len){
    int ret = fuse_reply_buf(req, buf, len);
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_buf(fuse_req_t req, core123::str_view sv){
    return reply_buf(req, sv.data(), sv.size());
}

inline void  reply_iov(fuse_req_t req, const struct iovec* iov, int count){
    int ret = fuse_reply_iov(req, iov, count);
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_readlink(fuse_req_t req, const char *buf){
    int ret = fuse_reply_readlink(req, buf);
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_open(fuse_req_t req, const fuse_file_info *fi){
    fuseful_net_open_handles++;
    int ret = fuse_reply_open(req, fi);
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_release(fuse_req_t req){
    int ret = fuse_reply_err(req, 0);
    fuseful_net_open_handles--;
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_statfs(fuse_req_t req, const struct statvfs* sv){
    int ret = fuse_reply_statfs(req, sv);
    if( ret )
        core123::complain("fuse_%s failed with ret=%d", __func__, ret);
}

inline void reply_none(fuse_req_t req){
    fuse_reply_none(req);
}

void lowlevel_notify_inval_entry(fuse_ino_t pino, const std::string& name) noexcept;
void lowlevel_notify_inval_inode(fuse_ino_t ino, off_t off, off_t len) noexcept;
// the _detached versions hand the actual work off to a thread-pool.  They're
// safe for use in a callback.
void lowlevel_notify_inval_entry_detached(fuse_ino_t pino, const std::string& name);
void lowlevel_notify_inval_inode_detached(fuse_ino_t ino, off_t off, off_t len);
//int lowlevel_notify_delete(fuse_ino_t pino, fuse_ino_t cino, const char* name, size_t namelen); // not implemented

// fuse_main_ll - lifted from examples/hello_ll.c in the fuse 2.9.2 tree.

// Fuseful's signal handlers will make a best-effort to open the file
// named by a non-NULL signal_report_filename and append some
// diagnostic info to it.

// If non-NULL, the crash_handler will be called by fuseful's signal
// handlers *only* for "Program Termination Signals", e.g., SIGSEGV,
// SIGILL - the signals that we can't return from.  The very limited
// goal of a crash_handler is to clean up anything that would have
// persistent undesirable consequences, e.g., filesystem litter,
// forked processes.  Since it will be called by a signal handler,
// only async-signal-safe functions should be used.  It should assume
// as little as possible about the integrity of data structures,
// threads, etc., and do no more than necessary to achieve the limited
// goal.  It will not be called more than once.  Also note that
// shutting fuse itself down is handled elsewhere in fuseful.cpp, so
// the crash_handler should not call fuse_unmount, fuse_session_exit,
// etc.
int fuseful_main_ll(fuse_args* args, const fuse_lowlevel_ops& llops,
                    const char *signal_report_filename,
                    void (*crash_handler)());

// fuseful_init_failed - may be called by the init callback when it
// can't initialize correctly.  Should not be called anywhere else.
void fuseful_init_failed();

// fuseful_initiate_shutdown - call this when you want to bring it all
// down.  It is intended to be safe to call from anywhere (a "worker"
// thread, or a background or maintenance thread).  In practice, we
// currently (Nov 2020) call it only from maintenance threads when the
// idle timer expires or the suprocess exits.
void fuseful_initiate_shutdown();

// fuseful_report - report statistics.
std::ostream& fuseful_report(std::ostream&);

// fuse_options_to_envvars - convert any options like:
//   -o FOO=bar
// to a putenv("FOO=bar"), for all environment variables (FOO)
// in the list of envvars.
//
// The output of --help starts with 'desc' and lists the named envvars.
// The first non-option argument will be put in 'fuse_device_option'.
// It corresponds to the 'device' argument to mount(8), e.g.,
//   mount [...] [-o options] device dir
void fuse_options_to_envvars(fuse_args* args, const std::string& desc, std::initializer_list<std::string> envvars);

extern std::string fuse_device_option;
extern std::string g_mountpoint;
extern fuse_ino_t g_mount_dotdot_ino;

