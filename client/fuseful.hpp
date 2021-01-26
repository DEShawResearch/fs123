// fuseful.hpp - A collection of fuse-specific wrappers, helpers and
// convenience functions.
#pragma once

#ifndef FUSE_USE_VERSION
#error FUSE_USE_VERSION should have been -Defined in the command line
#endif
#include <fuse/fuse_lowlevel.h>
#include <core123/str_view.hpp>
#include <atomic>

// Keep track of the net (opens - releases)
extern std::atomic<int> fuseful_net_open_handles;


// Wrap the fuse_reply_xxx functions so we can consistently
// catch errors, report diagnostics, etc.  But see the comment
// in fuseful.cpp about the futility of catching errors.
void reply_err(fuse_req_t req, int eno);
void reply_entry(fuse_req_t req, const fuse_entry_param* e);
void reply_attr(fuse_req_t req, const struct stat *s, float timeout);
void reply_xattr(fuse_req_t req, size_t len);
void reply_buf(fuse_req_t req, const char *buf, size_t len);
void reply_buf(fuse_req_t req, core123::str_view sv);
void reply_iov(fuse_req_t req, const struct iovec* iov, int count);
void reply_readlink(fuse_req_t req, const char *buf);
void reply_open(fuse_req_t req, const fuse_file_info *fi);
void reply_release(fuse_req_t req);
void reply_statfs(fuse_req_t req, const struct statvfs* sv);
void reply_none(fuse_req_t req);

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

