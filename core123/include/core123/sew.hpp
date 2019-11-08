#pragma once

// <system_error> would be a lot more useful if the unix system calls
// were wrapped by a library that threw a system_error when the call
// failed.  This is that library...  sew is mnemonic for
// system_error_wrapper

// This header defines wrapper functions, named 'foo' in the
// core123::sew namespace corresponding to most POSIX
// system calls, '::foo'.  The wrapper will call '::foo', check for
// error conditions, and if any are detected, it will throw a
// std::system_error with an appropriate errno and a nicely
// formated message.
//
// The basic pattern is that you put something like:
//
//    #include <core123/sew.hpp> // this file
//    ...
//    using namespace core123;
//    // or, if you don't want the whole core123 namespace
//    namespace sew = core123::sew;
//
// at the top of your source file.  And then, whenever you would
// have used a POSIX-function:
//
//      result = some_posix_function(argy, bargy);
//      if(result < 0){ // or whatever the error condition is
//          ... // cobble together a 'msg' and throw an exception.
//      }
//
// you write:
//
//      result = sew::some_posix_function(argy, bargy);
//
// By pushing the error-checking and exception generation down (into
// sew::some_function), and the exception-handling up (into your caller)
// your code becomes *much* simpler and more readable.
//
// If you use this idiom, your code must be prepared to be "unwound"
// from any point.  This means that you probably want to wrap 'open'
// objects with an autocloser so that you don't leak things like file
// descriptors.  See <core123/autoclosers.hpp> for simple wrappers
// that provide autoclosing file-descriptors, FILE* and DIR*.

#include "autoclosers.hpp"
#include "core123/throwutils.hpp"
#include <system_error>
#include <sstream>
#include <string>
#include <memory>
#include <cerrno>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <fcntl.h>
#include <sys/mman.h>	// mmap and friends
#include <unistd.h>
#include <signal.h>
#if __has_include(<sys/uio.h>)
#include <sys/uio.h>
#endif
#if __has_include(<sys/mount.h>)
#include <sys/mount.h>
#endif
#include <sys/resource.h>

// gcc says union wait shadows wait(), sigh.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <sys/wait.h>
#pragma GCC diagnostic pop

#include <dirent.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#if __has_include(<sys/sendfile.h>)
#include <sys/sendfile.h>
#endif
#include <poll.h>
#if __has_include(<sys/epoll.h>)
#include <sys/epoll.h>
#endif
#include <sys/utsname.h> // uname
#if __has_include(<sys/inotify.h>)
#include <sys/inotify.h>
#endif
#if __has_include(<sys/vfs.h>)
#include <sys/vfs.h>	// statfs
#endif
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <sys/time.h>
#include <utime.h>
#include <grp.h> // setgroups

#include <memory>
#include <string>

// Provide stream-insertion for sigval, i.e., sigqueue's last arg:
inline std::ostream& operator<<(std::ostream& o, const sigval& sv){ 
    return o<<sv.sival_ptr; 
}

namespace core123 { namespace sew {

// Now let's wrap those POSIX functions...  Wrapped functions should
// look nearly identical to the "wrapee".  You can read a Unix man
// page, e.g., 'man 2 foo', and know exactly what to expect from the
// wrapper, foo.  The only difference should be that foo reports
// errors by throwing a system_error rather than through the return value.
// If foo's return value is used exclusively for error reporting, then
// foo returns void.  Otherwise, foo returns the same type as foo.
//
// TODO - some POSIX functions are known problem-children.  It would
// be nice to wrap them with a slightly more robust/sane api.
// curcwd() is an example, along with anything else that "returns" a
// string, but makes it absurdly hard to know how much space to
// allocate.  Other candidates are gethostname() and readlink() or
// anything that intersects with PATH_MAX, NAME_MAX and getconf.  Even
// further down the slippery slope is an enhanced getenv() with an
// optional "ifnotfound" second arg and a "fork_exec".  A
// "fork_exec_wait" that take lists of strings as arguments and then
// carefully handles the corner cases would fill a large gap between
// system() and execv().

// TODO - varargs functions, e.g., clone, ptrace, printf, scanf
//  and their siblings all need special handling.  There may be
//  others.

// TODO - things in libdl.  If we incorporate them using the
// existing pattern, then we force anyone who links with the
// library to also link with -ldl.  That seems wrong.  It suggests
// a misdesign.  Leave libdl out for now...

// N.B.  it's not clear how or where these names should be declared or
// defined or #defined.  The tradeoffs are complicated.  We need to
// take some measurements rather than speculating about the impact on
// ease-of-use (header-only implementation), namespace pollution
// (#defines don't respect namespaces), ability to overload names,
// compile time, object code size (initializers and weak symbols
// compiled into every .o), run-time latency (static initializers),
// and runtime throuhgput (every call is intermediated by some number
// of wrappers and constructors).
//
// Measurements:
// -   Using something like:
//         #define _wrap(name) static decltype(wrap(&::name, #name)) name = wrap(&::name, #name)
//     for every symbol in a file with a single function and a few #includes
//     from about .375 to about .625 sec.  I.e., it adds about 0.25 sec to the
//     compilation.
//     (see ut/ut_sew_compile_time.cpp)

// wrap - wrap a function that returns a value to the caller.  If the
// wrapped function returns the special value ERRVAL, then throw a
// system_error.  Otherwise return the returned value to the caller.

// Don't use 'strfunargs' for the execv family because it doesn't
// "look inside" argv, which is where all the interesting arguments
// are hiding.  Use this instead:
inline std::string
execv_what(const char *name, const char *filename, char *const argv[], char *const envp[] = nullptr){
    std::ostringstream oss;
    auto argvend = argv;
    while( *++argvend ) ;
    oss << name << "(" << filename << ", [" << strbe(", ", argv, argvend) << ", <(char*)0>]";
    if(envp)
        oss << ", " << envp;
    oss << ")";
    return oss.str();
}

template <typename R, typename ... Args>
constexpr auto
wrap(R (*f)(Args...), const char *name, R errval=static_cast<R>(-1)){
    return [f,name,errval](Args ... args){
        auto ret = f(args...);
        if( ret == errval ){
            throw se(strfunargs(name, args ...));
        }
        return ret;
    };
}

// wrapvoid - wrap a function whose return value is used only for error
//  signaling, but is not useful to the caller.  If the wrapped function
//  returns the special value ERRVAL, then throw a system_error.  Otherwise,
//  return void.
template <typename R, typename ... Args>
constexpr auto
wrap_void(R (*f)(Args...), const char *name, R errval=static_cast<R>(-1)){
    return [f,name,errval](Args ... args){
        if(f(args...) == errval){
            throw se(strfunargs(name, args...));
        }
    };
}

// wrap_returns_errno - wrap a function that returns the errno.
template <typename R, typename ... Args>
constexpr auto
wrap_returns_errno(R (*f)(Args...), const char *name){
    return [f,name](Args ... args){
        auto ret = f(args...);
        if(ret){
            throw se(ret, strfunargs(name,  args...));
        }
    };
}

// wrap_check_errnoN - wrap a function with N specific values
//  that indicate an error.

// wrap_check_errno0 - there is no single value that indicates
// an error.  Rely on errno for errors.  E.g., fread, frwite, strtol
template <typename R, typename ... Args>
constexpr auto
wrap_check_errno0(R (*f)(Args...), const char *name){
    return [f,name](Args ... args){
        errno = 0;
        auto ret = f(args...);
        if(errno){
            throw se(strfunargs(name, args...));
        }
        return ret;
    };
}

// wrap_check_errno1 - one particular value *might* indicate
//  an error, but only if it is returned together with a non-zero
//  errno.  E.g., readdir.
template <typename R, typename ... Args>
constexpr auto
wrap_check_errno1(R (*f)(Args...), const char *name, R errval=static_cast<R>(-1)){
    return [f,name,errval](Args ... args){
        errno = 0;
        auto ret = f(args...);
        if(ret == errval && errno){
            throw se(strfunargs(name, args...));
        }
        return ret;
    };
}

// N.B.  By mid-December 2013, we're up to ~250 functions here.  It's
// starting to approach completeness - but I'm sure there are gaps.
// Should we make a real effort at completeness?

#define _wrap(name) static decltype(wrap(&::name, #name)) name = wrap(&::name, #name)
#define _wrap_void(name) static decltype(wrap_void(&::name, #name)) name = wrap_void(&::name, #name)
#define _wrap_check_errno0(name) static decltype(wrap_check_errno0(&::name, #name)) name = wrap_check_errno0(&::name, #name)
#define _wrap_check_errno1(name) static decltype(wrap_check_errno1(&::name, #name)) name = wrap_check_errno1(&::name, #name)
#define _wrap_returns_errno(name) static decltype(wrap_returns_errno(&::name, #name)) name = wrap_returns_errno(&::name, #name)

#define _wrapev(name, ev) static decltype(wrap(&::name, #name, ev)) name = wrap(&::name, #name, ev)
#define _wrapev_void(name, ev) static decltype(wrap_void(&::name, #name, ev)) name = wrap_void(&::name, #name, ev)
#define _wrapev_check_errno0(name, ev) static decltype(wrap_check_errno0(&::name, #name,  ev)) name = wrap_check_errno0(&::name, #name,  ev)
#define _wrapev_check_errno1(name, ev) static decltype(wrap_check_errno1(&::name, #name, ev)) name = wrap_check_errno1(&::name, #name, ev)
#define _wrapspecial(decl, body) static inline decl body
#define _wrapev_linkage(name, ev) _wrapev(name, ev)
#define _wrap_void_linkage(name) _wrap_void(name)

#define _COMMA ,

// several functions have arguments that are char*, but that might be
// uninitialized, or be unterminated if the function returns with an
// error condition.  In such cases, we can't just pass the args
// through to strfunargs(args...) because it would try to format the
// uninitialized char*.  These are denoted with 'special case char*'
// in the list below.
//
// Note that these symbols are fundamentally different from the
// others.  These are bona fide function names, so they participate in
// overload name resolution, unlike the std::functions. 

// Portability macros - we currently have *extremely* limited
// goals for portability.  We're almost exclusively focused on
// Linux, but if we can make the code work on another platform with
// minimal changes, we'll do so.  "Minimal" changes means no
// autoconf and nothing to make the code unreadable or slow(er)
// to compile.  
#if !defined(__GLIBC)
#define _NOT_GLIBC(X)
#else
#define _NOT_GLIBC(X) X
#endif
    
//  fd-related utilities
// _wrap_void(close); // autocloser overload.  See below
_wrap_void(fstat);
_wrap_void(stat);
_wrap_void(lstat);
_wrap_void(fstatat);
_wrap(read);
_wrap(pread);
_wrap(readv);
_wrap(write);
_wrap(pwrite);
_wrap(writev);
_wrap(lseek);
_NOT_GLIBC( _wrap(lseek64); )
//_wrap(readlinkat); // special case char*
//_wrap(readlink);   // special case char*
_wrapspecial(ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz),
{
    ssize_t ret = ::readlinkat(dirfd, path, buf, bufsiz);
    if(ret == -1)
        throw se(strfunargs("readlinkat", dirfd, path, (void*)buf, bufsiz));
    return ret;
}
             )

_wrapspecial(ssize_t readlink(const char *path, char *buf, size_t bufsiz),
{
    ssize_t ret = ::readlink(path, buf, bufsiz);
    if(ret == -1)
        throw se(strfunargs("readlink", path, (void*)buf, bufsiz));
    return ret;
}
             )
// readlink and readlinkat are crazy.  They *never* append a NUL
// byte.  When they return bufsiz, there is no way to know whether the
// result has been truncated.  It's tempting to make the 'core123::sew'
// version better-behaved, but that's a slippery slope, and breaks the "works
// just like the non-core123" version rule.  So the 'core123::sew' version is
// just as pathological as the standard version, but can be used as a drop-in
// replacement without worry.  The str_ version sidesteps the whole
// ssize_t return value and returns a string.
_wrapspecial(std::string str_readlinkat(int dirfd, const char *pathname),
{
    size_t bufsz = 4097; // PATH_MAX+1.  should succeed on first try
    std::unique_ptr<char[]> p{new char[bufsz]};
    size_t sz = core123::sew::readlinkat(dirfd, pathname, p.get(), bufsz);
    while(sz>=bufsz && bufsz<8*4097){
        bufsz *= 2;
        p.reset(); // no need to keep old and new buffers.
        p.reset(new char[bufsz]);
        sz = core123::sew::readlinkat(dirfd, pathname, p.get(), bufsz);
    }
    if(sz >= bufsz){
        // We gave up before things got ridiculous.  Something is
        // clearly wrong.  Don't wait for a bad_alloc or an
        // OOM-killer to blow us out of the water.
        throw se(ENOMEM, strfunargs( __func__, dirfd, pathname));
    }
    return {p.get() _COMMA p.get()+sz};
}
             )

_wrapspecial(std::string str_readlink(const char *pathname),
{
    size_t bufsz = 4097; // PATH_MAX+1.  should succeed on first try
    std::unique_ptr<char[]> p{new char[bufsz]};
    size_t sz = core123::sew::readlink(pathname, p.get(), bufsz);
    while(sz>=bufsz && bufsz<8*4097){
        bufsz *= 2;
        p.reset(); // no need to keep old and new buffers.
        p.reset(new char[bufsz]);
        sz = core123::sew::readlink(pathname, p.get(), bufsz);
    }
    if(sz >= bufsz){
        // We gave up before things got ridiculous.  Something is
        // clearly wrong.  Don't wait for a bad_alloc or an
        // OOM-killer to blow us out of the water.
        throw se(ENOMEM, strfunargs(__func__, pathname));
    }
    return {p.get() _COMMA p.get()+sz};
}
             )
_wrap_void(fsync);
_NOT_GLIBC( _wrap_void(fdatasync); )
_wrap_void(pipe);
_wrap(dup);
_wrap(dup2);
_wrap_void(truncate);
_wrap_void(ftruncate);
_wrap_void(flock);
_wrap_void(chmod);
_wrap_void(fchmod);
_wrap_void(fchmodat);
//_wrap_void(lchmod); // "lchmod is not implemented and will always fail"
_wrap_void(chown);
_wrap_void(fchown);
_wrap_void(lchown);
_wrap(getxattr);
_NOT_GLIBC( _wrap(lgetxattr); )
_wrap(fgetxattr);
_wrap(listxattr);
_NOT_GLIBC( _wrap(llistxattr); )
_wrap(flistxattr);
_wrap_void(setxattr);
_NOT_GLIBC( _wrap_void(lsetxattr); )
_wrap_void(fsetxattr);
_wrap_void(removexattr);
_NOT_GLIBC( _wrap_void(lremovexattr); )
_wrap_void(fremovexattr);
_wrap_void(utime);
_wrap_void(utimes);
_wrap_void(futimes);
_NOT_GLIBC( _wrap_void(futimesat); )

// functions in fcntl.h (fcntl is below)
_wrap(creat);
_NOT_GLIBC( _wrap(creat64); )
_wrap_void(lockf);
_NOT_GLIBC( _wrap_void(lockf64); )
// MacOS (others?) doesn't have posix_fadvise or posix_fallocate. Test
// for whether there's a #define of one of the flag arguments to
// decide whether to try to wrap them...
#ifdef POSIX_FADV_NORMAL
_wrap_returns_errno(posix_fadvise);
_wrap_returns_errno(posix_fallocate);
#endif
_NOT_GLIBC( _wrap_returns_errno(posix_fadvise64); )
_NOT_GLIBC( _wrap_returns_errno(posix_fallocate64); )

// memory mapping
_wrapev(mmap,  MAP_FAILED);
_wrap_void(munmap);
_NOT_GLIBC( _wrap_void(brk); ) // actually, it is on apple, but it throws an error
_NOT_GLIBC( _wrapev(sbrk, (void*)-1); )
_wrap_void(madvise);
_wrap_void(mprotect);
_wrap_void(mincore);
_wrap_void(msync);
_wrap_void(mlock);
_wrap_void(munlock);
_wrap_void(mlockall);
_wrap_void(munlockall);

// pathname utilities
// access is invariably used in a query mode (i.e. if (access("foo") < 0) dosomething
// so there seems to be no point wrapping it
_wrap_void(link);
_wrap_void(linkat);
_wrap_void(symlink);
_wrap_void(symlinkat);
_wrap_void(unlink);
_wrap_void(unlinkat);
_wrap_void(rmdir);
_wrap_void(mkdir);
_wrap_void(mkdirat);
_wrap_void(mknod);
_wrap_void(mkfifo);
_NOT_GLIBC( _wrap_void(mknodat); )
_NOT_GLIBC( _wrap_void(mkfifoat); )
// Some of the pathname functions are in stdio.h
_wrap_void(rename);
_wrap_void(renameat);
_wrap_void(remove);

// mkdtemp and mkstemp are POSIX:
_wrapev(mkdtemp, (char*)0);
_wrap(mkstemp);
// tmpfile is defined with the other FILE* functions.
// tmpnam and tempnam are "obsolescent" according to POSIX-2008.
// mktemp was removed from POSIX-2008.  "Never use mktemp"

// system informational utilities
_wrap_void(getrusage);
_wrap(getpid);
//_wrapev(getcwd, (char *)0); // special case char*
_wrapspecial(char* getcwd(char *buf, size_t size),
{
    char *ret = ::getcwd(buf, size);
    if( ret == (char *)0 )
        throw se(strfunargs("getcwd", (void*)buf, size));
    return ret;
}
             )

//_wrapev(getenv, (char *)0); // doesn't set errno
// We overload ENOENT to indicate a missing value.
// It's unfortunate that perror(ENOENT) is "No such
// file or directory", but it's the best conceptual
// match among the POSIX errnos.  We originally used
// ENOKEY, which means "required key not available",
// in glibc, but it's not portable.
_wrapspecial(const char* getenv(const char *name),
{
    const char *ret = ::getenv(name);
    if( ret == (char *)0 )
        throw se(ENOENT, strfunargs("getenv", name));
    return ret;
 }
             )
//_wrap(gettid);
_wrap(getppid);
_wrap(getuid);
_wrap(geteuid);
_NOT_GLIBC( _wrap_void(getresuid); )
_wrap_void(setuid);
_wrap_void(seteuid);
_NOT_GLIBC( _wrap_void(setreuid); )
_NOT_GLIBC( _wrap(setresuid); )
_wrap(getgid);
_wrap(getegid);
_NOT_GLIBC( _wrap_void(getresgid); )
_wrap(setgid);
_wrap(setegid);
_wrap(setregid);
_NOT_GLIBC( _wrap(setresgid); )
_wrap(getgroups);
_wrap_void(setgroups);
_wrap(sysconf);
_wrap(pathconf);
_wrap(fpathconf);
// confstr // special case char*
_wrapspecial(size_t confstr(int name, char *buf, size_t len),
{
    errno = 0;
    size_t ret = ::confstr(name, buf, len);
    if(ret>len || errno==EINVAL)
        throw se(strfunargs("confstr", name, (void*)buf, len));
    return ret;
}
             )
// ignore ustat
_wrap_void(statfs);
_wrap_void(fstatfs);
_wrap_void(statvfs);
_wrap_void(fstatvfs);
_wrap_void(getrlimit);
_wrap_void(setrlimit);
_wrap_void(getpriority);
_wrap_void(setpriority);
_wrap_void(nice);

// session management and terminals
_wrap(daemon); // not posix, but it is in glibc and BSD
_wrap(getsid);
_wrap_void(setsid);
_wrap(getpgid);
_wrap_void(setpgid);
_wrap(getpgrp);   // technically, cannot fail?
_wrap_void(setpgrp);
_wrap(tcgetpgrp);
_wrap_void(tcsetpgrp);
_NOT_GLIBC( _wrap(vhangup); )

// time
_wrap(time);
_wrap_void(gettimeofday);
_wrap_void(settimeofday);
_wrap_void(getitimer);
_wrap_void(setitimer);
_wrap_void(nanosleep);
// these required -lrt before glib-2.17
_wrap(clock_getres);
_wrap(clock_gettime);
_wrap(clock_settime);

// directory utilities
// we ignore getdents and readdir(2).  Who knew there was readdir(2) and readdir(3)...
_wrap(dirfd);
_wrapev(opendir, (::DIR*)0);
// fdopendir needs special handling to avoid double-close when used with xfd_t.  See below.
//_wrapev(fdopendir, (::DIR*)0);
_NOT_GLIBC( _wrapev_check_errno1(readdir64, (dirent64*)0); )
_wrapev_check_errno1(readdir, (dirent*)0);

// readdir_r is another oddball.  It returns 0 on success but
// a positive value equal to errno on failure.  Are there others
// that behave this way?  Do we need another wrap_xxx macro?
_wrapspecial(void readdir_r(::DIR* dirp, struct dirent*entry, struct dirent** result),
{
    int ret = ::readdir_r(dirp, entry, result);
    if(ret)
        throw se(strfunargs("readdir_r", dirp, entry, result));
}
             )

_NOT_GLIBC( _wrapspecial(void readdir64_r(::DIR* dirp, struct dirent64*entry, struct dirent64** result),
{
    int ret = ::readdir64_r(dirp, entry, result);
    if(ret)
        throw se(strfunargs("readdir64_r", dirp, entry, result));
}
             )
	    )

_wrap(telldir);
// seekdir returns void, so it needs special handling.
//_wrap_void(seekdir);
_wrapspecial(void seekdir(::DIR* dirp, long offset),
{
    errno = 0;
    ::seekdir(dirp, offset);
    if(errno)
        throw se(strfunargs("seekdir", dirp, offset));
}
             )
// _wrap_void(closedir); // autocloser overload.  See below
_wrap_void(chdir);
_wrap_void(fchdir);
_wrap_void(chroot);

// FILE* utilities
_wrapev(fopen, (::FILE*)0);
_wrapev(tmpfile, (::FILE*)0);
// fdopen needs special handling to avoid double-close when used with xfd_t.  See below.
//_wrapev(fdopen, (::FILE*)0); 
_wrapev(freopen, (::FILE*)0);
_wrap_check_errno0(fread);
_wrap_void(fflush);
_wrap_void(fseek);
_wrap_void(fseeko);
_wrap(ftell);
_wrap(ftello);
_wrap_void(fgetpos);
_wrap_void(fsetpos);
_wrap_check_errno0(fwrite);
// _wrap_void(fclose); // autocloser overload.  See below
_wrapev(fputc, EOF);
_wrapev_void(fputs, EOF);
_wrapev_void(puts, EOF);
_wrapev(fgetc, EOF);
_wrapev(ungetc, EOF);
_wrap_check_errno0(fgets);
// XXX N.B. clearerr, feof, ferror can't fail.  Should we wrap them
// anyway?  fileno *might* conceivably return -1 and set errno to
// EBADF.

// file notification
_NOT_GLIBC( _wrap(inotify_init); )
_NOT_GLIBC( _wrap(inotify_add_watch); )
_NOT_GLIBC( _wrap_void(inotify_rm_watch); )

// XXX TODO thread calls: futex tkill set_thread_area set_tid_address .  pthreads_* are challenging, some return 0 for ok, errno for fail?
// XXX TODO sem, shm, msg
// XXX TODO realpath

// process utilities
_wrap(fork);
_wrap(vfork);
// XXX TODO clone and ptrace have ellipsis

// Special case the exec family because:
//  they have odd signatures
//  they're noreturn
[[noreturn]] _wrapspecial(int execve(const char *filename, char *const argv[], char *const envp[]),
{
    ::execve(filename, argv, envp);
    throw se(execv_what("execve", filename, argv, envp));
}
             )

[[noreturn]] _wrapspecial(int execv(const char *filename, char *const argv[]),
{
    ::execv(filename, argv);
    throw se(execv_what("execv", filename, argv));
}
             )

[[noreturn]] _wrapspecial(int execvp(const char *filename, char *const argv[]),
{
    ::execv(filename, argv);
    throw se(execv_what("execvp", filename, argv));
}
                          )

// templated functions must be defined in the hpp.
template <typename ... Args>
[[noreturn]] int execl(const char *path, const char *arg,  Args...args){
    ::execl(path, arg, args...);
    auto eno=errno;
    throw se(eno, strfunargs("execl", path, arg, args...));
}

template <typename ... Args>
[[noreturn]] int execlp(const char *path, const char *arg,  Args...args){
    ::execlp(path, arg, args...);
    auto eno = errno;
    throw se(eno, strfunargs("execlp", path, arg, args...));
}

template <typename ... Args>
[[noreturn]] int execle(const char *path, const char *arg,  Args...args){
    ::execle(path, arg, args...);
    auto eno = errno;
    throw se(eno, strfunargs("execle", path, arg, args...));
}
    
_NOT_GLIBC( _wrap_void(unshare); )
_wrap(umask);
_wrap(wait);
_wrap(wait3);
_wrap(wait4);
_wrap(waitpid);
_wrap_void(waitid);
_wrapev(signal, SIG_ERR);
_wrap_void(sigaction);
_wrap_void(sigprocmask);
_NOT_GLIBC( _wrap(sigwaitinfo); )
_NOT_GLIBC( _wrap(sigtimedwait); )
_NOT_GLIBC( _wrap_void(sigqueue); )
_wrap_void(sigpending);
_wrap_void(sigsuspend);
_wrap_void(pause);
_wrap_void(kill);
_wrap_void(killpg);

// FILE* PIPE utilities
_wrapev(popen, (::FILE *)0);
// _wrap_void(pclose); // autocloser overload.  See below

// Miscellaneous utilities
_wrap_check_errno0(strtol);

// Networking utilities
_wrap(socket);
_wrap_void(socketpair);
_wrap_void(bind);
_wrap_void(listen);
_wrap_void(accept);
_wrap_void(connect);
_wrap_void(shutdown);
_wrap(select);
_wrap(pselect);
_wrap(poll);
_NOT_GLIBC( _wrap(ppoll); )
_NOT_GLIBC( _wrap(epoll_create); )
_NOT_GLIBC( _wrap(epoll_wait); )
_NOT_GLIBC( _wrap_void(epoll_ctl); )
_wrap_void(send);
_wrap_void(sendto);
_wrap_void(sendmsg);
_wrap(sendfile);
_wrap(recv);
_wrap(recvfrom);
_wrap(recvmsg);
_wrap_void(getsockname);
_wrap_void(getpeername);
_wrap_void(getsockopt);
_wrap_void(setsockopt);
//_wrap_void(gethostname); // special case char*
_wrapspecial(void gethostname(char *name, size_t len),
{
    int ret = ::gethostname(name, len);
    if(ret == -1)
        throw se(strfunargs("gethostname", (void*)name, len));
}
             )
_wrap_void(sethostname);
//_wrap_void(getdomainname); // special case char*
_wrapspecial(void getdomainname(char *name, size_t len),
{
    int ret = ::getdomainname(name, len);
    if(ret == -1)
        throw se(strfunargs("getdomainname", (void*)name, len));
}
             )
_wrap_void(setdomainname);
_wrap_void(uname);
_wrapev_linkage(gethostbyname, (struct hostent *)0);
_wrapev_linkage(gethostbyname2, (struct hostent *)0);
_wrapev_linkage(gethostbyaddr, (struct hostent *)0);
_wrapev_linkage(gethostent, (struct hostent *)0);
_NOT_GLIBC( _wrap_void_linkage(gethostbyname_r); )
_NOT_GLIBC( _wrap_void_linkage(gethostbyname2_r); )
_NOT_GLIBC( _wrap_void_linkage(gethostent_r); )

// open, openat, fcntl and ioctl need special handling because they're declared with an
// ellipsis e.g. in fcntl.h, int open(const char *, int, ...);
// fcntl has three variants, use "man 2 ioctl_list" for the full horror of ioctl
// 
// Like the other 'special handling' functions, they participate in
// overload name resolution.
//#if !defined(_system_error_wrapper_cpp)
//int open(const char* name, int flags, mode_t mode=0);
//#else
static inline int open(const char* name, int flags, mode_t mode=0);
static inline int open(const char* name, int flags, mode_t mode){
    int ret = ::open(name, flags, mode);
    if( ret < 0 )
        throw se(strfunargs("open", name, flags, mode));
    return ret;
}
//#endif

//#if !defined(_system_error_wrapper_cpp)
//int openat(int dirfd, const char *pathname, int flags, mode_t mode=0);
//#else
static inline int openat(int dfd, const char *pathname, int flags, mode_t mode=0);
static inline int openat(int dfd, const char *pathname, int flags, mode_t mode){
    int ret = ::openat(dfd, pathname, flags, mode);
    if( ret < 0 )
        throw se(strfunargs("openat", dfd, pathname, flags, mode));
    return ret;
}
//#endif

_wrapspecial(void ioctl(int fd, int request),
{
    if (::ioctl(fd, request) < 0)
        throw se(strfunargs("ioctl", fd, request));
}
             )   

_wrapspecial(void ioctl(int fd, int request, int arg),
{
    if (::ioctl(fd, request, arg) < 0)
        throw se(strfunargs("ioctl", fd, request, arg));
}
             )   

_wrapspecial(void ioctl(int fd, int request, void *p),
{
    if (::ioctl(fd, request, p) < 0)
        throw se(strfunargs("ioctl", fd, request, p));
}
             )   

_wrapspecial(void fcntl(int fd, int request),
{
    if (::fcntl(fd, request) < 0)
        throw se(strfunargs("fcntl", fd, request));
}
             )   

_wrapspecial(void fcntl(int fd, int request, long arg),
{
    if (::fcntl(fd, request, arg) < 0)
        throw se(strfunargs("fcntl", fd, request, arg));
}
             )   

_wrapspecial(void fcntl(int fd, int request, void *p),
{
    if (::fcntl(fd, request, p) < 0)
        throw se(strfunargs("fcntl", fd, request, p));
}
             )   

// mremap is protected by #ifdef __USE_GNU in /usr/include/mman.h,
// so we had better do the same.
#ifdef __USE_GNU
_wrapspecial(void *mremap(void *a, size_t osz, size_t nsz, int f, void *newaddr=0),
{
    void *ret = ::mremap(a, osz, nsz, f, newaddr);
    if (ret == MAP_FAILED)
        throw se(strfunargs("mremap", a, osz, nsz, f, newaddr));
    return ret;
}
             )
#endif

_wrapspecial(int system(const char* cmd),
{
    // system is a weird hybrid.  It can "fail" for "systemic"
    // reasons, e.g., fork() failed or the system shell isn't
    // exectuable.  Or it can "fail" for "programmatic" reasons, e.g.,
    // system("exit 99").  The only unambiguous failure is if system
    // returns -1, which means that fork failed.  Otherwise, we let
    // the caller worry about it.  Bottom line: core123::sew::system won't
    // get in your way if you're trying to be careful, but it doesn't,
    // by itself, make system easy to use safely.
    //
    // Don't forget the cmd==nullptr special-case, which returns
    // "non-zero to indicate that the shell command processor is
    // available or zero if none is available" and furthermore "shall
    // always return non-zero when command is not NULL".  POSIX
    // resolves this apparent contradiction by pointing out that a
    // conforming POSIX system always has a command processor
    // available.  Nothing forbids the "non-zero" value from being
    // -1.  Sigh.
    errno = 0;
    int ret = ::system(cmd);
    if((ret == -1  && cmd != nullptr)|| errno != 0)
        throw se(strfunargs("system", cmd));
    return ret;
}
             )

// fdopen and fdopendir need special treatment because the conventional
// overloads would lead to a double-close in cases like:
//    ac::fd_t xfd = core123::sew::open("/some/where", O_RDONLY);
//    ac::FILE *fp = core123::sew::fdopen(xfd, "r");
//
// in which case the same underlying fd would be "owned" by both xfd
// and fp.  Part of the solution is in autoclosers.hpp, where there
// are overloads for ::fdopen(ac::fd_t, char*) and
// ::fdopendir(ac::fd_t).
//
// Similar overloads, in the system_error_wrapper namespace are provided here.
inline ::FILE *fdopen(int fd, const char *mode){
    ::FILE *ret = ::fdopen(fd, mode);
    if( ret == nullptr )
        throw se(strfunargs("fdopen", fd, mode));
    return ret;
}

template <typename CEH>
::FILE* fdopen(core123::ac::fd_t<CEH> fd, const char *mode){
    // N.B.  see comment in autoclosers.hpp about the semantics of
    // ::fdopen(ac::fd_t).
    ::FILE *fp  = ::fdopen(std::move(fd), mode);
    if( fp == nullptr )
        throw se(strfunargs("fdopen(core123::ac::fd_t, const char*)", int(fd), mode));
    return fp;
}

// Related considerations apply to close, fclose, pclose and closedir.
// It would be correct for the user to say:
//    ac::fd_t fd = ...
//    core123::sew::close(fd.release()); // ok
// But it would be bad to say:
//    core123::sew::close(fd);           // bad!  fd will be closed twice!
//
// There are overloads in autoclosers.hpp for ::close(ac::fd_t), etc.
// Here are the corresponding versions in the core123 namespace.
// If you want to explicitly call close yourself and get an exception
// thrown, these are all roughly equivalent:
//    fd.close()
// of
//    core123::sew::close(std::move(fd));
// or
//    core123::sew::close(fd.release());
// 
inline void close(int fd){
    if(::close(fd) == -1)
        throw se(strfunargs("close", fd));
}

template <typename CEH>
void close(ac::fd_t<CEH> fd){
    ::close(std::move(fd));
}

inline void fclose(::FILE *fp){
    if(::fclose(fp) == -1)
        throw se(strfunargs("fclose", fp));
}

template <typename E>
inline void fclose(ac::FILE<E> fp){
    ::fclose(std::move(fp));
}

inline void pclose(::FILE *fp){
    if(::pclose(fp) == -1)
        throw se(strfunargs("pclose", fp));
}

template <typename CEH>
void pclose(ac::PIPEFILE<CEH> fp){
    ::pclose(std::move(fp));
}

inline void closedir(::DIR *dp){
    if(::closedir(dp) == -1)
        throw se(strfunargs("closedir", dp));
}

template <typename CEH>
void closedir(ac::DIR<CEH> dp){
    ::closedir(std::move(dp));
}

// fdopendir: see comments above about fdopen, and in autoclosers.hpp
// about the ac::fd_t overload.
inline ::DIR *fdopendir(int fd){
    ::DIR *ret = ::fdopendir(fd);
    if(ret == 0)
        throw se(strfunargs("fdopendir", fd));
    return ret;
}

template <typename CEH>
::DIR* fdopendir(ac::fd_t<CEH> fd){
    // N.B.  see comment in autoclosers.hpp about the semantics of
    // ::fdopendir(ac::fd_t).
    ::DIR *ret = ::fdopendir(std::move(fd));
    if( ret == nullptr )
        throw se(strfunargs("fdopendir(core123::ac::fd_t)", int(fd)));
    return ret;
}

// forward declaration of core123::sew::openat
int openat(int dirfd, const char *pathname, int flags, mode_t mode);

// There's no opendirat, so we make one, with 'core123' semantics.
inline ::DIR* opendirat(int fd, const char* path) try {
    int oflag = O_DIRECTORY|O_NONBLOCK|O_RDONLY;
#ifdef O_LARGEFILE
    oflag |= O_LARGEFILE;
#endif
    core123::ac::fd_t<> dfd = core123::sew::openat(fd, path, oflag, 0);
    return fdopendir(std::move(dfd));
 }catch(...){
    std::ostringstream oss;
    oss << "opendirat(" << fd << ", " << path << ")";
    throw_with_nested(std::system_error(errno, std::system_category(), oss.str()));
}

#undef _wrap
#undef _wrap_void
#undef _wrap_errno0
#undef _wrap_errno1

#undef _wrapev
#undef _wrapev_void
#undef _wrapev_errno0
#undef _wrapev_errno1
#undef _wrapspecial
#undef _COMMA
#if !defined(_separate_static_linkage_cpp)
#undef __NOT_GLIBC
#endif

}} // namespace core123::sew

