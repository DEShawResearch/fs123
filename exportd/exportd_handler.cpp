#include "exportd_handler.hpp"
#include "fs123/stat_serializev3.hpp"
#include "fs123/acfd.hpp"
#include "fs123/sharedkeydir.hpp"
#include <core123/pathutils.hpp>
#include <core123/diag.hpp>
#include <core123/throwutils.hpp>
#include <core123/svto.hpp>
#include <core123/sew.hpp>
#include <core123/complaints.hpp>
#include <core123/netstring.hpp>
#include <core123/http_error_category.hpp>
#include <core123/threeroe.hpp>
#include <core123/syslog_number.hpp>
#include <core123/log_channel.hpp>
#include <chrono>
#if __has_include(<linux/fs.h>)
#include <linux/fs.h>
#endif

using namespace core123;

auto _exportd_handler = diag_name("exportd_handler");

namespace{
    // The sharedkeydir (i.e., the secret manager) is a singleton because
    // we have to open the file descriptor *before* we call chroot.
    // See exportd_global_setup.
    acfd the_sharedkeydir_fd;
    std::unique_ptr<sharedkeydir> the_sharedkeydir;

    // For debugging and bug-hunting!  Sleep for a random time to give
    // callers a chance to exercise timeout paths, expose data races,
    // etc.
    void random_sleep(double b){
        if(b==0.)
            return;
        thread_local std::mt19937 g(std::hash<std::thread::id>()(std::this_thread::get_id()));
        thread_local std::cauchy_distribution<> cd(0., b);  // cauchy is a very wide.  In fact, the mean is undefined/infinite.
        double howlong = std::abs(cd(g));
        DIAGf(_exportd_handler, "random_sleep for %f\n", howlong);
        std::this_thread::sleep_for(std::chrono::duration<double>(howlong));
    }
} // namespace <anon>

void exportd_handler::err_reply(fs123p7::req::up req, int eno){
    auto pi = req->path_info;
    errno_reply(std::move(req), eno, cache_control(eno, pi, nullptr));
}

// In general, it's not necessary to catch the handler callbacks.
// See the discussion in fs123server.hpp under 'Exceptions'.  If
// they throw, the caller (in libfs123) will log a complaint and carry
// on.  When the up_req goes out-of-scope, the req's destructor will
// call exception_reply with a 500 status code.
//
// On the other hand, detecting a failed system call
// and reporting the errno with err_reply is *not* exceptional.
// System errors in the system_category() should generally be caught
// and handled by calling err_reply.
//
// If we want to reply with some other http error (e.g., 40x), then
// call reply_exception and return.  
void
exportd_handler::a(fs123p7::req::up req){
    random_sleep(opts.debug_add_random_delay);
    auto full_path = opts.export_root + std::string(req->path_info);
    struct stat sb;
    if( ::lstat(full_path.c_str(), &sb) < 0 ){
        return err_reply(std::move(req), errno);
    }
    uint64_t esc = 0;
    if(S_ISREG(sb.st_mode) || S_ISDIR(sb.st_mode)){
        // getattr of regular file or directory requires an ESTALE-Cookie.
        esc = estale_cookie(sb, full_path);
    } else if (!S_ISLNK(sb.st_mode)) {
        return err_reply(std::move(req), EINVAL);
    }
    auto mv = monotonic_validator(sb);
    auto pi = req->path_info;
    a_reply(std::move(req), sb, mv, esc, cache_control(0, pi, &sb));
}

void
exportd_handler::d(fs123p7::req::up req, uint64_t inm64, bool begin, int64_t offset) {
    random_sleep(opts.debug_add_random_delay);
    auto fname = opts.export_root + std::string(req->path_info);
    // use open+fdopendir so we can use O_NOFOLLOW for safety
    acfd xfd = open(fname.c_str(), O_DIRECTORY | O_NOFOLLOW | O_RDONLY);
    if(!xfd)
        return err_reply(std::move(req), errno);
    acDIR dir = sew::fdopendir(std::move(xfd));
    // call fstat again, just to be sure...
    struct stat sb;
    sew::fstat(sew::dirfd(dir), &sb);
    auto esc  = estale_cookie(sew::dirfd(dir), sb, fname);
    auto etag64 = compute_etag(sb, esc);
    auto cc = cache_control(0, req->path_info, &sb);
    if( etag64 == inm64 )
        return not_modified_reply(std::move(req), cc);

    struct ::dirent* de;
    if(!begin)
	sew::seekdir(dir, offset);
    while( (de = sew::readdir(dir)) ){
        uint64_t entry_esc;
        try{
            entry_esc = opts.fake_ino_in_dirent ? 0 : estale_cookie(fname + "/" + de->d_name, de->d_type);
        }catch(std::exception& e){
            // This might happen if the file was removed or replaced
            // between the readdir and whatever syscall we use to
            // get the esc.
            complain(e, "export_handler::d(): error obtaining esc for: "+fname + "/" + de->d_name  + ".  Setting entry esc to 0");
            entry_esc = 0;
        }
#ifndef __APPLE__
        if(!req->add_dirent(*de, entry_esc))
#else
	if(!req->add_dirent(*de, entry_esc, sew::telldir(dir)))
#endif
            break;
    }
    bool at_eof = !de;
    d_reply(std::move(req), at_eof, etag64, esc, cc);
 }

void
exportd_handler::f(fs123p7::req::up req, uint64_t inm64, size_t len, uint64_t offset, void *buf) try {
    random_sleep(opts.debug_add_random_delay);
    auto fname = opts.export_root + std::string(req->path_info);
    acfd fd = ::open(fname.c_str(), O_RDONLY | O_NOFOLLOW);
    if( !fd ){
	// we were presumably able to lstat it, but couldn't open it.
	// This happens "normally" when an unreadable file
	// is in a readable directory.
	return (void)err_reply(std::move(req), errno);
    }
    // call fstat again, just to be sure...
    struct stat sb;
    sew::fstat(fd, &sb);
    if(!S_ISREG(sb.st_mode))
	httpthrow(400, fmt("expected file, but mode is %o for %s", sb.st_mode, fname.c_str()));
    auto esc = estale_cookie(fd, sb, fname);
    auto etag64 = compute_etag(sb, esc);
    auto cc = cache_control(0, req->path_info, &sb);
    if( etag64 == inm64 )
        return not_modified_reply(std::move(req), cc);

    auto validator = monotonic_validator(sb);
    // read directly into a string.  Can't avoid filling with NULs.
    std::string in(len, '\0');
    // we always do the read exactly as requested, then
    // decide if we hit the eof after the read
    auto nread = sew::pread(fd, buf, len, offset);
    f_reply(std::move(req), nread, validator, etag64, esc, cc);
 }catch(std::system_error& se){
    auto& code = se.code();
    if(code.category() == std::system_category())
        return (void)err_reply(std::move(req), code.value());
    // throw prompts the caller to generate a complaint.
    std::throw_with_nested(std::runtime_error("in exportd_handler::f() path_info=" + std::string(req->path_info)));
 }

void
exportd_handler::l(fs123p7::req::up req) try {
    random_sleep(opts.debug_add_random_delay);
    auto fname = opts.export_root + std::string(req->path_info);
    auto pi = req->path_info;
    l_reply(std::move(req), sew::str_readlink(fname.c_str()), cache_control(0, pi, nullptr));
 }catch(std::system_error& se){
    auto& code = se.code();
    if(code.category() == std::system_category())
        return (void)err_reply(std::move(req), code.value());
    // throw prompts the caller to generate a complaint.
    std::throw_with_nested(std::runtime_error("in exportd_handler::l() path_info=" + std::string(req->path_info)));
 }

void
exportd_handler::s(fs123p7::req::up req){
    random_sleep(opts.debug_add_random_delay);
    struct statvfs svb;
    sew::statvfs(opts.export_root.c_str(), &svb);
    // The cache-control for stat isn't path-specific, so it isn't appropriate
    // to go looking for .fs123_cache_control files.  Another command line
    // option??
    s_reply(std::move(req), svb, "max-age=30,stale-while-revalidate=30");
}

void
exportd_handler::n(fs123p7::req::up req){
    random_sleep(opts.debug_add_random_delay);
    n_reply(std::move(req), "exportd_handlers: 0\n", "max-age=1,stale-while-revalidate=1");
}

void
exportd_handler::x(fs123p7::req::up req, size_t len, std::string name){
    random_sleep(opts.debug_add_random_delay);
    std::string fname = opts.export_root + std::string(req->path_info);
    ssize_t sz;
    std::string buf(len, '\0');
#ifndef __APPLE__
    if (name.empty()){
	sz = llistxattr(fname.c_str(), &buf[0], len);
    } else {
        sz = lgetxattr(fname.c_str(), name.c_str(), &buf[0], len);
    }
#else
    if (name.empty()){
	sz = listxattr(fname.c_str(), &buf[0], len, XATTR_NOFOLLOW);
    } else {
        sz = getxattr(fname.c_str(), name.c_str(), &buf[0], len, 0, XATTR_NOFOLLOW);
    }
#endif
    if (sz < 0)
	return err_reply(std::move(req), errno);
    auto cc = cache_control(0, req->path_info, nullptr);
    if (len == 0)
        return x_reply(std::move(req), std::to_string(sz), cc);
    else
        return x_reply(std::move(req), std::move(buf), cc);
}

secret_manager*
exportd_handler::get_secret_manager() /* override */ {
    return the_sharedkeydir.get();
}

uint64_t
exportd_handler::compute_etag(const struct stat& sb, uint64_t estale_cookie){
    // Hash the stat's 'monotonic_validator', the estale_cookie, st_size
    // together.
    //
    // The estale cookie is necessary because it can change even if
    // the monotonic validator (typically mtime) doesn't, and in order
    // to be a "strong validator", the etag must reflect changes in
    // semantically important headers (such as fs123-estale-cookie).
    //
    // BEWARE: *nothing* is guaranteed if clocks go backwards or
    // if users maliciously (or accidentally) roll back mtimes.
    // To defend (however imperfectly) against backward-running
    // clocks, we mix in the st_size as well.  It's no more
    // effort, and it *might* avoid a nasty surprise one day.
    // Using ctim might defend against malicious/accidental
    // futimens(), but it would defeat the ability to sync an
    // export_root between servers.  The ctim/mtim choice could
    // plausibly be another config option.
    //
    uint64_t mtim64 = monotonic_validator(sb);
    // threeroe isn't cryptographic, so it theoretically "leaks" info
    // about mtim64 and st_size to eavesdroppers.  (estale_cookie is
    // public anyway).  BUT there's not a lot of entropy in mtim64 and
    // st_size, so even a much stronger hash, e.g., Blake2b in
    // crypto_generichash, would be invertible by brute-force: just
    // try plausible values mtim and st_size until one works.
    return threeroe(mtim64, estale_cookie).
        update(&sb.st_size, sizeof(sb.st_size)).
        hash64();
}

std::string
exportd_handler::cache_control(int eno, str_view path, const struct stat* sb){
    // If eno is non-zero, the reply will contain the specified
    // errno=eno, but it will *not* carry any data or metadata.
    // Nevertheless, we have to specify cache-control.  Current policy
    // is that the cache-control for an ENOENT should be the same as
    // for a successful reply.  This particular policy was very much
    // the original motivation for all of fs123!  Cache the ENOENTs so
    // that we pay for python's stupid search heuristics only once,
    // but then the negative result sits in cache (maybe even in the
    // kernel) for a long time thereafter.
    //
    // But what about other errnos?  If we're "certain" that the result
    // is non-transient (e.g., ENOTSUP), we give it a 1-day max-age:
    //   "max-age=86400,stale-while-revalidate=864000"
    //
    // If we don't "know" if it's transient, we give it the
    // --generic-error-cc option that defaults to:
    //   "max-age=30,stale-while-revalidate=30,stale-if-error=10000000")
    // The default is quite short because we wouldn't want a transient
    // EIO to stick around in caches for a long time.
    switch(eno){
    case 0:
    case ENOENT: // handle as if eno were 0.
        break; 
    case ENOTSUP: // seems unlikely to change...
        return "max-age=86400,stale-while-revalidate=864000";
    default:
        return opts.generic_error_cc;
    }

    // strip off the leading '/'.  Note that we've already 'validated'
    // the path_info to guarantee that it is either empty or starts
    // with '/'.
    if(!path.empty())
        path = path.substr(1);

    // gets called with sb==nullptr for /l, /n, etc.  But
    // none of those are directories, so:
    bool isdir = sb && S_ISDIR(sb->st_mode);
    return rule_cache->get_cc(std::string(path), isdir); 
}

static uint64_t
timespec2ns(const struct timespec& ts){
    return uint64_t(1000000000)*ts.tv_sec + ts.tv_nsec;
}

uint64_t
exportd_handler::monotonic_validator(const struct stat& sb){
    // We once naively thought that we could rely on the
    // the 'nanosecond resolution' of st_mtim on modern Linux.
    // Nope!  Even though st_mtim has a nanosecond field, it
    // appears (on ext4 in the Linux-3.10 kernel) to have a
    // granularity of 1ms.  I.e., modifications made up to 1ms
    // apart may leave a file's mtim unchanged.  Later
    // modifications will cause the mtim to increase in increments
    // of roughly 1 million nanoseconds!  Consequently st_mtim
    // alone isn't a "strong validator" if writers might be making
    // modifications less than 1ms apart (which isn't hard at
    // all).
    //
    // To work around this, we have a command line option:
    //   --mtim_granularity_ns 
    // with default value 4 million, i.e., 4msec.
    //
    // Instead of using mtim directly as a validator, we instead
    // use:
    //    std::min( mtim, now-mtim_granularity ).
    // It produces a "never match" validator for the first few
    // milliseconds after a file is modified, but after the mtim
    // is safely in the past, (when it is no longer possible for a
    // modification to leave the mtim unchanged) it produces a
    // consistent, repeatable validator.
    //
    // FWIW, it looks like there has been some work on the kernel
    // timekeeping code in the 4.x kernels as part of the y2038
    // effort that *may* mitigate/fix the underlying granularity
    // problem.  I.e., with new (4.11) kernels, it might be
    // reasonable to set --mtim_granularity_ns=0.
    uint64_t mtim64 = timespec2ns(sb.st_mtim);
    // We're looking at st_mtim, not some fancy C++ <chrono>
    // thingy.  So use clock_gettime rather than std::chrono::whatever.
    struct timespec now_ts;
    sew::clock_gettime(CLOCK_REALTIME, &now_ts);
    uint64_t now64 = timespec2ns(now_ts);
    return std::min(mtim64, now64 - 2*opts.mtim_granularity_ns); 
}

// What should we do when we can't get an estale cookie?
// Unfortunately, we don't (May 2018) have a good way to distinguish
// between "this URL is borked" and "this server is borked".  So if we
// throw an exception, that typically results in us sending a 50x,
// which can cause all sorts of fire drills and immune responses in
// downstream caches, clients, proxies, forwarders, redirectors, etc.
// Until we have a better way to communicate the limited scope of the
// problem (this URL) let's just issue a 'complaint' and return 0.
// This choice runs the slight risk of 'incorrect' estale behavior,
// but has a much lower risk of disrupting wider operations.
//
// estale_cookie_catch is the called in the catch block of each
// of our estale_cookie overloads.  It assumes that there is
// a current exception.
uint64_t estale_cookie_catch(std::system_error& se, const std::string& fullpath){
    auto& code = se.code();
    if(code.category() == std::system_category()){
        complain(LOG_WARNING, se, "failed to get estale_cookie for fullpath=" + fullpath + " returning 0");
        return 0;
    }
    std::throw_with_nested(std::runtime_error("error thrown in estale_cookie_common(fullpath="+fullpath+")"));
}

// estale_cookie_ - only called for 'S_IFREG' or 'S_ISDIR'
uint64_t
exportd_handler::estale_cookie(int fd, const struct stat& sb, const std::string& fullpath) try {
    // DANGER - the code in the d_e_c(fullpath, d_type) overload below
    // assumes that if src is neither "none" nor "st_ino", then the
    // only field that matters in the sb is the st_mode.  Don't
    // violate that assumption without changing code in both
    // locations!
    auto source = opts.estale_cookie_src;
    switch(source){
    case exportd_options::ESC_IOC_GETVERSION:
        // the third arg to ioctl(fd, FS_IOC_GETVERSION, &what) is a pointer
        // to what type?  The first clue is that FS_IOC_GETVERSION is defined
        // in headers as _IOR('v', 1, long).  That means the ioctl interface
        // is expecting a long.  BUT - the kernel code (in extN/ioctl.c, reiserfs/ioctl.c,
        // etc.) all looks like:
        //       return put_user(inode->i_generation, (int __user *)arg);
        // which means an int (not a long) will be transferred to
        // user-space.  Furthermore, inode::i_generation is declared
        // an __u32 in linux/fs.h.  So it looks like the kernel is
        // working with ints and u32s.  Finally, e2fsprogs uses an int
        // pointer.  Conclusion: it's an int - the definition as
        // _IOR(...,long) notwithstanding
        unsigned int generation;
#ifdef FS_IOC_GETVERSION
        sew::ioctl(fd, FS_IOC_GETVERSION, &generation);
#else
	throw se(EINVAL, "FS_IOC_GETVERSION not defined");
#endif
        return generation;
    case exportd_options::ESC_GETXATTR:
    case exportd_options::ESC_SETXATTR:
        // char buf[32] corresponds to the amount of space reserved
        // for the cookie in the client-side's struct reply123.
        char buf[32];
        static size_t bufsz = sizeof(buf)-1;  // -1 guarantees room for a terminal NUL
    tryagain:  // we may come back here if setxattr fails with EEXIST
        {
#ifndef __APPLE__
        auto ret = ::fgetxattr(fd, "user.fs123.estalecookie", buf, bufsz);
#else
        auto ret = ::fgetxattr(fd, "user.fs123.estalecookie", buf, bufsz, 0, 0);
#endif
        if( source == opts.ESC_SETXATTR && ret < 0 && errno == ENODATA){
            // getxattr failed, and we've been asked to do "setxattr",
            // so we (try to) set the attribute to the current time-of-day
            // now is decimal nanoseconds since the epoch
            using namespace std::chrono;
            auto value = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
            std::string now = std::to_string(value);
#ifndef __APPLE__
            auto setret = ::fsetxattr(fd, "user.fs123.estalecookie", now.c_str(), now.size(), XATTR_CREATE);
#else
            auto setret = ::fsetxattr(fd, "user.fs123.estalecookie", now.c_str(), now.size(), 0, XATTR_CREATE);
#endif	    
            if(setret<0 && errno == EEXIST){ // XATTR_CREATE noticed a race!
                source = opts.ESC_GETXATTR;
                goto tryagain;
            }
            if(setret < 0)
                throw se(errno, fmt("setxattr(%s) failed.  errno=%d\n", fullpath.c_str(), errno));
            return value;
        }
        if(ret<0)
            throw se(errno, "fgetxattr(" + fullpath + ", \"user.fs123.estalecookie\") failed");
        if(ret==0 || size_t(ret) > bufsz)
            throw se(EINVAL, "fgetxattr(" + fullpath + ", \"user.fs123.estalecookie\") returned unacceptable length: " +std::to_string(ret));
        return svto<uint64_t>(str_view(buf, ret));
        }
    case exportd_options::ESC_ST_INO:
        return sb.st_ino;
    case exportd_options::ESC_NONE:
        return 0;
    default:
        throw se(EINVAL, fmt("Unrecognized FS123_ESTALE_COOKIE_SRC: %d\n", source));
    }
 }catch(std::system_error& se){
    return estale_cookie_catch(se, fullpath);
 }

uint64_t
exportd_handler::estale_cookie(const std::string& fullpath, int d_type) try {
    // When called from do_dir we don't even have a stat buffer.
    // Let's try to avoid unnecessary syscalls.
    if(opts.estale_cookie_src == opts.ESC_NONE)
        return 0;
    
    // Estale_cookie is only for DIRs and REGular files
    if(!(d_type == DT_DIR || d_type == DT_REG))
        return 0;
    struct stat sb;
    if(opts.estale_cookie_src == opts.ESC_ST_INO){
        sew::lstat(fullpath.c_str(), &sb);
        return sb.st_ino;
    }
    // DANGER - this assumes that the only thing in the sb
    // that matters to d_e_c_common is the st_mode.  Be
    // careful when changing the code!
    sb.st_mode = dtype_to_mode(d_type);
    return estale_cookie(sb, fullpath);
 }catch(std::system_error& se){
    return estale_cookie_catch(se, fullpath);
 }
   
uint64_t
exportd_handler::estale_cookie(const struct stat& sb, const std::string& fullpath) try {
    // We don't have an fd, but we might need one...
    // N.B.  any errors encountered here,  e.g., inability to open the file,
    // missing xattrs, etc. get reported with an HTTP 500 Internal Server Error.
    // A proxy cache might transform that into 502 Bad Gateway.
    // When handling them on the client-side, take note of the fact
    // that these are *request-specific* errors.  Yes - the server is
    // misconfigured.  But No - it's probably not a good idea to
    // defer all requests to this server.
    acfd fd;
    if(opts.estale_cookie_src == opts.ESC_IOC_GETVERSION ||
       opts.estale_cookie_src == opts.ESC_GETXATTR ||
       opts.estale_cookie_src == opts.ESC_SETXATTR) {
	// sanity check to avoid reading through symlink etc.
	if (!S_ISREG(sb.st_mode) && !S_ISDIR(sb.st_mode))
	    throw se(EINVAL, fmt("was asked for estale_cookie when !S_ISREG && !ISDIR(%o): %s",
					 sb.st_mode, fullpath.c_str()));
        fd = sew::open(fullpath.c_str(), O_RDONLY | O_NOFOLLOW);
    }
    return estale_cookie(fd, sb, fullpath);
 }catch(std::system_error& se){
    return estale_cookie_catch(se, fullpath);
 }

void
exportd_handler::logger(const char* remote, fs123p7::method_e method, const char* uri, int status, size_t length, const char* date){
    accesslog_channel.send(fmt("%s [%s] \"%s %s\" %u %zd",
                               remote, date,
                               (method==fs123p7::GET)? "GET" : (method==fs123p7::HEAD)? "HEAD" : "OTHER",
                               uri,
                               status, length));
}

exportd_handler::exportd_handler(const exportd_options& _opts) : opts(_opts)
{
    rule_cache = std::make_unique<cc_rule_cache>(opts.export_root, opts.rc_size, opts.default_rulesfile_maxage, opts.no_rules_cc);
    accesslog_channel.open(_opts.accesslog_destination, 0666);        
}

void exportd_global_setup(const exportd_options& exportd_opts){
    // Configure things that aren't associated with a particular
    // handler, but that aren't really part of the fs123server either.
    // E.g., configuring the destination for complaints and
    // diagnostics, daemonizing, chroot-ing, etc.
    //
    // Since the chroot is here, anything we have to do *before*
    // chroot must be here too, e.g., setting up our secret_manager.
    // And that means singletons (sharedkeydir and sharedkeydir_fd).
    //
    // FIXME - This is a sure sign that something is deeply wrong.
    if(exportd_opts.daemonize && exportd_opts.pidfile.empty())
        throw se(EINVAL, "You must specify a --pidfile=XXX if you --daemonize");
    
    if(exportd_opts.daemonize){
        // We'll do the chdir ourselves after chroot.
        // and we'll keep stdout open for diagnostics.
#ifndef __APPLE__
        sew::daemon(true/*nochdir*/, true/*noclose*/);
#else
	throw se(EINVAL, "MacOS deprecates daemon().  Run in foreground and use launchd");
#endif
    }

    // N.B.  core123's log_channel doesn't call openlog.  But it
    // *does* explicitly provide a facility every time it calls
    // syslog.  So the third arg to openlog shouldn't matter.  But
    // just in case, glibc's openlog(...,0) leaves the default
    // facility alone if it was previously set, and sets it to
    // LOG_USER if it wasn't.
    unsigned logflags = LOG_PID|LOG_NDELAY;  // LOG_NDELAY essential for chroot!
    openlog(exportd_opts.PROGNAME, logflags, 0);
    auto level = syslog_number(exportd_opts.log_min_level);
    set_complaint_destination(exportd_opts.log_destination, 0666);
    set_complaint_level(level);
    set_complaint_max_hourly_rate(exportd_opts.log_max_hourly_rate);
    set_complaint_averaging_window(exportd_opts.log_rate_window);
    if(!startswith(exportd_opts.log_destination, "%syslog"))
        start_complaint_delta_timestamps();

    if(!exportd_opts.diag_names.empty()){
        set_diag_names(exportd_opts.diag_names);
        set_diag_destination(exportd_opts.diag_destination);
        DIAG(true, "diags:\n" << get_diag_names() << "\n");
    }
    the_diag().opt_tstamp = true;

    if(!exportd_opts.pidfile.empty()){
        std::ofstream ofs(exportd_opts.pidfile.c_str());
        ofs << sew::getpid() << "\n";
        ofs.close();
        if(!ofs)
            throw se("Could not write to pidfile");
    }

    if(!exportd_opts.sharedkeydir.empty()){
        the_sharedkeydir_fd = sew::open(exportd_opts.sharedkeydir.c_str(), O_DIRECTORY|O_RDONLY);
        the_sharedkeydir = std::make_unique<sharedkeydir>(the_sharedkeydir_fd, exportd_opts.encoding_keyid_file, exportd_opts.sharedkeydir_refresh);
    }
    
    // If --chroot is empty (not the default, but it can be set to the
    // empty string), then do neither chdir nor chroot.  The process
    // stays in its original cwd, relative paths are relative to cwd,
    // etc.
    //
    // If --chroot is non-empty, then chdir first and, if chroot
    // is not "/", then chroot(".").  Thus it's possible to say
    // --chroot=/ even without cap_sys_chroot, but
    // --chroot=/anything/else requires cap_sys_chroot.
    // 
    // There is no option to ignore chroot errors.  If there were,
    // overall behavior would depend on the presence/absence of
    // capabilities, which would be bad.
    if(!exportd_opts.chroot.empty()){
        sew::chdir(exportd_opts.chroot.c_str());
        log_notice("chdir(%s) successful",  exportd_opts.chroot.c_str());
        if(exportd_opts.chroot != "/"){
            try{
                sew::chroot(".");
                log_notice("chroot(.) (relative to chdir'ed cwd) successful");
            }catch(std::system_error& se){
                std::throw_with_nested(std::runtime_error("\n"
"chroot(.) failed after a successful chdir to the intended root\n"
"Workarounds:\n"
"   --chroot=/      # chdir(\"/\") but does not make chroot syscall\n"
"   --chroot=       # runs in cwd.  Does neither chdir nor chroot\n"
"  run with euid=0  # root is permitted to chroot\n"
"  give the executable the cap_sys_chroot capability, e.g.,:\n"
"    sudo setcap cap_sys_chroot=pe /path/to/executable\n"
"  but not if /path/to/executable is on NFS.\n"));
                // P.S.  There may be a way to do this with capsh, but only
                // if the kernel supports 'ambient' capabilities (>=4.3).
                // sudo capsh --keep=1 --uid=$(id -u) --caps="cap_sys_chroot=pei"  -- -c "obj/fs123p7exportd --chroot=/scratch ..."
                // only gets us '[P]ermitted' cap_sys_chroot, but not [E]ffective.
                // Maybe with more code we could upgrade from P to E?
            }
        }
    }
}
