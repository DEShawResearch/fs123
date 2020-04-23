#include "fuseful.hpp"
#include "fs123/acfd.hpp"
#include <core123/complaints.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/threadpool.hpp>
#include <core123/sew.hpp>
#include <core123/exnest.hpp>
#include <core123/throwutils.hpp>
#include <core123/envto.hpp>
#include <core123/stats.hpp>
#include <core123/stacktrace.hpp>
#include <string>
#include <syslog.h>

using namespace core123;

std::string g_mountpoint;
struct fuse_session* g_session;
fuse_ino_t g_mount_dotdot_ino = 1;
std::atomic<bool> g_destroy_called;

// fuse_device_option will be set by fuse_parse_cmdline, called
// by fuse_main_ll.
std::string fuse_device_option;

namespace {
#define STATS_INCLUDE_FILENAME "fuseful_statistic_names"
#define STATS_STRUCT_TYPENAME fuseful_stats_t
#include <core123/stats_struct_builder>
fuseful_stats_t stats;

struct fuse_chan *g_channel;
void (*crash_handler)();
void (*g_ll_destroy)(void*);

// It's illegal to call the fuse_lowlevel_notify_inval_{entry,inode} functions
// from a filesystem 'op' callback.  So we need to background them somehow.
// Launching a detached thread would require the least extra machinery, but
// that's problematic for two reasons:
//  1 - detached threads that are still running when main exits cause
//      'undefined behavior'.  Machinery is required to prevent that.
//  2 - we encountered some never-explained stuck kernel threads.
//        https://sourceforge.net/p/fuse/mailman/message/34847520/
//      So it seems prudent to avoid doing lots of concurrent 'invals'.
// Conclusion:  use a threadpool with one thread and infinite backlog.
// BUT - don't initialize it here!  We can't have C++ threads in-scope
// across a fork(), and we fork() in fuse_daemonize().  So instantiation
// of invaltp  has to wait till we call fuse_daemonize.
std::unique_ptr<threadpool<void>> invaltp;

// <fuse argument handling>
// The code started with environment variables rather than -o options.
// Envvars are convenient, and they allow us to not think about
// argc/argv parsing.  But they don't work as options in fstab...  We
// also want to work with the command line options that fuse itself
// knows about, e.g., -f -m and those  that it passes through to mount,
// e.g., -o noatime.  
//
// So we work with the incredibly baroque fuse_arg/fuse_opt_parse
// machinery to convert command line options into env-vars.
//
// The resulting command line looks like:
//    fs123p7 mount -o GardenTimeoutCrfXz=/foo/bar/.timeout.crf.xz /path/to/GardenRemote /mount/point

enum{
    KEY_HELP,
    KEY_PUTENV,
};

// helptext and fuse_device_option will be filled in by
// fuse_options_to_envvars (below)
std::string helptext;

int gardenfs_opt_proc(void */*data*/, const char *arg, int key,
                             struct fuse_args *outargs){
    static int n_nonopt = 0;
    switch(key){
    case KEY_HELP:
        fprintf(stderr, "%s\n", helptext.c_str());
        return fuse_opt_add_arg(outargs, "-ho");

    case KEY_PUTENV:
        {
        std::string sarg(arg);
        auto eqpos = sarg.find('=');
        auto k = sarg.substr(0, eqpos);
        auto val = (eqpos==std::string::npos)? "" : sarg.substr(eqpos+1);
        if(0 != ::setenv(k.c_str(), val.c_str(), 1))
            throw se(errno, "setenv(" + k +", " + val + ")");
        }
        return 0;
    case FUSE_OPT_KEY_NONOPT:
        // We peel off the first non-option argument and call it
        // fuse_device_arg.
        if( n_nonopt++ == 0 ){
            fuse_device_option = arg;
            return 0;
        }else
            return 1;

    default:
        return 1;
    }
}
// </fuse argument handling>

std::terminate_handler sys_terminate_handler = nullptr;

// complain_and_abort - intended to be the C++ termination handler.
// Send a stack trace and the nested exception to the complaint
// channel, then call fuseful_teardown, and then fall through to the
// standard terminate handler (which should call abort, and might
// also write some info to stderr).
void complain_and_abort(){
    complain(LOG_CRIT, "std::terminate handler:  Backtrace:" + str(stacktrace_from_here()));
    
    std::type_info* t = abi::__cxa_current_exception_type();
    if(t){
        try{throw;}
        catch(const std::exception& e){
            complain(LOG_CRIT, e, "std::terminate handler.  current exception is:");
        }catch(...){
            complain(LOG_CRIT, "std::terminate handler.  current exception of type %s is not derived from std::exception", t->name());
        }
    }else{
        complain(LOG_CRIT, "std::terminate handler.  abi::__cxa_current_exception_type returned nullptr");
    }

    complain(LOG_CRIT, "std::terminate handler:  calling fuseful_teardown()");
    fuseful_teardown();

    if(sys_terminate_handler)
        (*sys_terminate_handler)();
    abort();
}

void do_fuse_unmount(){
    static std::atomic_flag entered = ATOMIC_FLAG_INIT;
    if(entered.test_and_set()){
        complain(LOG_NOTICE, "do_fuse_unmount:  Return immediately from re-invocation");
        return;
    }
    if(!g_channel){
        complain(LOG_NOTICE,  "do_fuse_unmount:  g_channel==nullptr.  Was it never mounted?  Return immediately");
        return;
    }
    // <diagnostic only>
    // follow/report the logic that fuse_unmount will use internally in libfuse-2.9.2.
    int fd = fuse_chan_fd(g_channel);
    const char *what_to_call = geteuid()? "fuse_mnt_umount" : "umount2 or exec(fusermount)";
    if(fd != -1){
        struct pollfd pfd = {};
        pfd.fd = fd;
        int ret = poll(&pfd, 1, 0);
        /* If file poll returns POLLERR on the device file descriptor,
           then the filesystem is already unmounted */
        if (1 == ret && (pfd.revents & POLLERR))
            complain(LOG_NOTICE, "do_fuse_unmount:  Not connected: This is often the result of an external fusermount -u. fuse_unmount will close fuse_chan_fd=%d but will not call %s.", fd, what_to_call);
        else
            complain(LOG_NOTICE, "do_fuse_unmount:  Still connected:  This often follows a call to fuse_session_exit, possibly via a signal handler. fuse_unmount will close fuse_chan_fd=%d (twice!) and call %s", fd, what_to_call);
    }else{
        complain(LOG_NOTICE, "do_fuse_unmount:  fuse_chan_fd == -1: This is rare, and not the typical result of either a signal or a fusermount -u.  It's not clear what will happen now");
    }
    // </diagnostic only>
    // N.B.  fuse_unmount will free(g_channel), so we null it out
    // before to avoid dereferencing it afterwards.
    auto tmp_channel = g_channel;
    g_channel = nullptr;
    fuse_unmount(g_mountpoint.c_str(), tmp_channel);
}

static void (*libfuse_handler)(int);
static const char *signal_filename;

void fuseful_handler(int signum){
    // syslog is not async-signal-safe.  Even if it were, our
    // 'complain' API isn't, and it would be A LOT of effort to make
    // it so.  Instead, we try to write a line to
    // 'signal_filename' (which was specified as an argument to
    // fuseful_main_ll).  If open() or write() fails, we just carry
    // on.  It's too late to try anything else...
    auto eno = errno;  // glibc docs advise that we restore errno before returning.
    int fd = -1;
    if(signal_filename)
        fd = ::open(signal_filename, O_CREAT|O_WRONLY|O_APPEND, 0666);
    if(fd >= 0){
        char msg[] = "fuseful_handler:  Caught signal NN\n";
        msg[sizeof(msg)-4] = '0' + (signum/10)%10;  // Tens-digit
        msg[sizeof(msg)-3] = '0' + signum%10;       // Ones-digit
        unused(::write(fd, msg, sizeof(msg)-1));
    }

    // N.B.  the tmp_xxx idiom is so we call the function no more than
    // once, even if it segfaults and otherwise brings us back here.
    if(libfuse_handler){
        auto tmp_hndlr = libfuse_handler;
        libfuse_handler = nullptr;
        (*tmp_hndlr)(signum);
    }

    // When used by fuse_set_signal_handlers, the libfuse_handler
    // returns control to "normal", non-signal-handling code.  We do
    // the same - when doing so does not result in undefined behavior.
    // But
    // https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html#tag_15_04_03
    // says that behavior is undefined if we return from SIGBUS,
    // SIGFPE, SIGILL or SIGSEGV.  We can't call fuseful_teardown()
    // because it's wildly async-signal-unsafe.  But doing nothing is
    // guaranteed to leave the mount point in a "Transport endpoint
    // not connected" state.  So when handling a "Program Error
    // Signal", we *try* to leave the mount-point not borked by
    // calling fuse_unmount before re-raising the signal with the
    // SIG_DFL handler.  fuse_unmount isn't async-safe either, so
    // this could hang (or worse), but it's still probably better
    // than leaving the endpoint disconnected.
    switch(signum){
    // These are the "Program Error Signals" according to:
    // https://www.gnu.org/software/libc/manual/html_node/Program-Error-Signals.html#Program-Error-Signals
    case SIGFPE:
    case SIGILL:
    case SIGSEGV:
    case SIGBUS:
    case SIGABRT:
#if defined(SIGIOT) && SIGABRT != SIGIOT
    case SIGIOT:
#endif
    case SIGTRAP:
#ifdef SIGEMT
    case SIGEMT:
#endif
    case SIGSYS:
        do_fuse_unmount();
#ifdef __GLIBC__
        // backtrace() is async-signal *un*safe.  But it's worth the
        // risk when we're handling a "Program Error Signal".
        if(fd >= 0){
            void* bt[50];
            int n = backtrace(&bt[0], 50);
#define MSG "Backtrace from backtrace_symbols_fd\n"
            unused(::write(fd, MSG, sizeof(MSG)-1));
            backtrace_symbols_fd(bt, n, fd);
#undef MSG
            ::close(fd);
            fd = -1;
        }
#endif // __GLIBC__
        if(crash_handler){
            auto tmp_hndlr = crash_handler;
            crash_handler = nullptr;
            (*tmp_hndlr)();
        }
        // reraise
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        ::sigaction(signum, &sa, nullptr);
        ::raise(signum);
    }
    if(fd>=0)
        ::close(fd);
    errno = eno;
}

void handle_all_signals(){
    sys_terminate_handler = std::set_terminate(complain_and_abort);

    // *Exactly* what happens in libfuse's handler changes from
    // version to version.  In general, the handler sets a flag
    // somewhere in libfuse's internal data structures, and the
    // presence of that flag causes the fuse_session_loop to exit on
    // its next iteration.  But libfuse is careful to make the details
    // opaque, so the only way we can get it to do its thing is to
    // call it.  But the handler itself is static.  We can't call it
    // directly.  So we first let fuse set up its signal handlers, and
    // then ask sigaction what fuse_set_signal_handlers has registered
    // for SIGHUP.
    if(fuse_set_signal_handlers(g_session) == -1)
        throw se(EINVAL, "fuse_set_signal_handlers failed");
    struct sigaction sa;
    sew::sigaction(SIGHUP, NULL, &sa);
    if(sa.sa_handler != SIG_IGN && sa.sa_handler != SIG_DFL)
        libfuse_handler = sa.sa_handler;
    complain(LOG_NOTICE, "signal receipt will be logged in %s", signal_filename?signal_filename:"<nowhere>");

    // Change the handler in sa to be *our* handler, and reinstall
    // it on (almost) all signals.
    sa.sa_handler = fuseful_handler;
    for(int sig=1; sig<32; ++sig){
        switch(sig){
        case SIGKILL: // We're powerless to change it.  Don't try.
        case SIGSTOP: //    ditto
#ifdef SIGTSTP
        case SIGTSTP: // Closely related to SIGSTOP.  Don't touch it.
#endif
#ifdef SIGTTIN
        case SIGTTIN: //    ditto
#endif
#ifdef SIGTTIN
        case SIGTTOU: //    ditto
#endif
        case SIGALRM: // We *hope* nobody's using it, but don't count on it.
        case SIGCHLD: // Let's not interfere with this one either...
        case SIGPIPE: // fuse_set_signal_handlers set this to a no-op. Leave it alone.
#ifdef SIGWINCH
        case SIGWINCH:// Normally Ignored.  Do not change.
#endif      
#ifdef SIGURG
        case SIGURG:  //     ditto
#endif
#ifdef SIGCONT
        case SIGCONT: // Normally continues a stopped process. Do not change.
#endif
            break;
        default:
            // The fuseful_handler will call the libfuse_handler,
            // before jumping through additional hoops.
            sew::sigaction(sig, &sa, NULL);
            break;
        }
    }
}

} // namespace <anon>

void lowlevel_notify_inval_entry(fuse_ino_t pino, const std::string& name) noexcept{
    int ret = fuse_lowlevel_notify_inval_entry(g_channel, pino, name.c_str(), name.size());
    switch(ret){
    case 0:
        stats.inval_entry_successes++;
        // success!
        break;
    case -2:
        // This happens...  The kernel might keep asking us about
        // inos, even after we've told it to invalidate them.
        // Count them, but don't complain.
        stats.inval_entry_noents++;
        break;
    default:
        errno = -ret;
        stats.inval_entry_fails++;
        complain("fuse_lowlevel_notify_inval_entry(%lu, %s) returned %d: %m",
                 (unsigned long)pino, name.c_str(), -ret);
    }
}

void lowlevel_notify_inval_entry_detached(fuse_ino_t pino, const std::string& name){
    invaltp->submit([=](){
            lowlevel_notify_inval_entry(pino, name);
        });
}

void lowlevel_notify_inval_inode(fuse_ino_t ino, off_t off, off_t len) noexcept{
    int ret = fuse_lowlevel_notify_inval_inode(g_channel, ino, off, len);
    switch(ret){
    case 0:
        stats.inval_inode_successes++;
        break;
    case -2:
        // This happens...  The kernel might keep asking us about
        // inos, even after we've told it to invalidate them.
        // Count them, but don't complain.
        stats.inval_inode_noents++;
        break;
    default:
        errno = -ret;
        stats.inval_inode_fails++;
        complain("fuse_lowlevel_notify_inval_inode(%lu, %ld, %ld) returned %d: %m",
                 (unsigned long)ino, (long)off, (long)len, ret);
    }
}

void lowlevel_notify_inval_inode_detached(fuse_ino_t ino, off_t off, off_t len){
    invaltp->submit([=](){
            lowlevel_notify_inval_inode(ino, off, len);
        });
}
#if 0 // Not used, delete is not implemented and it should be deferred to the threadpool
int lowlevel_notify_delete(fuse_ino_t pino, fuse_ino_t cino, const char* name, size_t namelen){
    // N.B.  This always(?) returns ENOSYS on our 2.6.32 kernel.
    //  notify_delete wasn't implemented until 3.3.
    return ( fuse_lowlevel_notify_delete(g_channel, pino, cino, name, namelen) ) ?  -errno : 0;
}
#endif

// this is the externally visible interface to fuse argument processing.
void fuse_options_to_envvars(fuse_args* args, const std::string& desc, std::initializer_list<std::string> envvars){
    struct my_opts{
        int saw_opt_subtype;
    } mopts = {};
    helptext = desc + 
        "\n"
        "The following options may be set on the command line or in the environment\n\n";

    std::vector<fuse_opt> opts = { FUSE_OPT_KEY("--help", KEY_HELP),
                                   FUSE_OPT_KEY("-h", KEY_HELP),
                                   {"subtype=", offsetof(struct my_opts, saw_opt_subtype), 1},
                                   FUSE_OPT_KEY("subtype=", FUSE_OPT_KEY_KEEP),
                                   // CentOS7 calls /sbin/mount.type
                                   // with -n which is supposed to
                                   // mean "don't write to /etc/mtab".
                                   // We don't write to /etc/mtab.
                                   // Let's ignore -n and hope that nobody
                                   // else (libfuse, fusermount) writes to
                                   // etc/mtab on our behalf.
                                   FUSE_OPT_KEY("-n", FUSE_OPT_KEY_DISCARD),
    };

    for( const auto& e : envvars ){
        opts.push_back( FUSE_OPT_KEY(e.c_str(), KEY_PUTENV) );
        helptext += "    -o " + e + "VALUE\n";
    }
    fuse_opt foe = FUSE_OPT_END;
    opts.push_back(foe);

    if(fuse_opt_parse(args, &mopts, opts.data(), gardenfs_opt_proc) == -1)
        throw se(EINVAL, "fuse_opt_parse failed for unknown reasons.");

    // We install the binary as /sbin/mount.fs123pN, which makes it easy
    // for mount(8) to find it.  But fuse defaults its "subtype" to
    // the binary's name, mount.fs123pN, which is ugly.  So we change
    // the subtype here so that the output of mount looks pretty, e.g.,
    //
    //   fs123 on /some/where type fuse.fs123 (ro,nosuid,nodev,noatime,allow_other,default_permissions,user=salmonj)
    //
    // But only if the caller didn't explicitly specify -osubtype=
    // elsewhere on the command line.  
    //
    // N.B.  if the subtype is fs123, then we probably want
    // "fuse.fs123" in the PRUNEFS line in /etc/updatedb.conf.
    // N.B.  it's a conscious decision to *not* make -osubtype=fs123pN
    // because that would entail even more fiddling with /etc/updatedb.conf
    if(!mopts.saw_opt_subtype)
        fuse_opt_add_arg(args, "-osubtype=fs123");
}

std::string fuseful_report(){
    return str(stats);
}

// fuseful_main_ll - inspired by examples/hello_ll.c in the fuse 2.9.2 tree.
int fuseful_main_ll(fuse_args *args, const fuse_lowlevel_ops& llops,
                    const char *_signal_filename,
                    void (*crash_handler_arg)()) try {
        signal_filename = _signal_filename;
        char *mountpoint = nullptr;
	int err = -1;

        int multithreaded; // ignored
        int foreground;
        if( fuse_parse_cmdline(args, &mountpoint, &multithreaded, &foreground) == -1 )
            throw se(EINVAL, "fuse_parse_cmdline returned -1.  Non-existent mountpoint?  Misspelled -option??");

        // fuse_parse_cmdline mallocs the mountpoint.  Copying it to
        // g_mountpoint and then freeing it silences the last valgrind
        // complaint!  N.B.  mountpoint might be NULL, e.g., if --help is on
        // the cmdline fuse_mount never looks at mountpoint and
        // doesn't mind NULL.
        if(mountpoint){
            g_mountpoint = mountpoint;
            free(mountpoint);
        }
        // The only place we ever use g_mount_dotdot_ino is when we're
        // populating the ".." entry of readdir(ino=1).  Most Linux
        // filesystems seem to report that the d_ino of
        // <mountpoint>/.. is the same as the ino of mountpoint
        // itself.  We'd do the same if we set g_mount_dotdot_ino=1
        // (or used 1 where we currently use g_mount_dotdot_ino).  But
        // it's not that hard to get it "right"...
	struct stat mount_dotdot_sb;
	if(lstat((g_mountpoint + "/..").c_str(), &mount_dotdot_sb) == 0)
	    g_mount_dotdot_ino = mount_dotdot_sb.st_ino;
	else{
	    complain(LOG_WARNING, "Couldn't lstat(%s/..): %m:  Setting g_mount_dotdot_ino=1 and hope for the best",
		   g_mountpoint.c_str());
	    g_mount_dotdot_ino = 1;
	}
        
        g_channel = fuse_mount(g_mountpoint.c_str(), args);
        if(g_channel == nullptr)
            throw se(EINVAL, fmt("fuse_mount(mountpoint=%s, args) failed.  No channel.  Bye.", g_mountpoint.c_str())); 
        fprintf(stderr, "mountpoint=%s, multithreaded=%d, foreground=%d\n", g_mountpoint.c_str(), multithreaded, foreground);

        // fuse_daemonize will do chdir("/") which makes sense for a
        // production daemon.  But for development, it's far more
        // convenient to run in ".", e.g., to be able to write core
        // dumps.  
        //
        // Our desired destination is specified by -oFs123Rundir.
        // which defaults to "." for foreground and "/" for background
        // processes.  Note that Fs123Rundir may be relative, in which
        // case it's relative to the process' initial cwd.
        //
        auto rundir = envto<std::string>("Fs123Rundir", foreground?".":"/");
        // We do openat to remember where we want to go before we call
        // fuse_daemonize, and then fchdir afterwards to take us
        // there.
        acfd cwdfd = sew::openat(AT_FDCWD, rundir.c_str(), 0);
        complain(LOG_NOTICE, "Calling daemonize at " + str(std::chrono::system_clock::now()));
        fuse_daemonize(foreground);
        complain(LOG_NOTICE, "Returned from daemonize at " + str(std::chrono::system_clock::now()));
        invaltp = std::make_unique<threadpool<void>>(1);
        sew::fchdir(cwdfd);
        cwdfd.reset();
        //
        // Note that if foreground is false, fuse_daemonize
        // opens("/dev/null"), and then dup2's it onto fd=0, 1 and 2.
        // So anything after this point that writes to stderr or
        // stdout is wasting its time unless -f is on the command
        // line.  But at least it won't get spurious errors or faults,
        // and it won't have open file descriptors that make umounting
        // hard.
        //
        // Unfortunately, fuse_lowlevel_new writes its complaints, and
        // even its --help output to stderr.  So unless we also add
        // -f, we daemonize and we never see its error messages or the
        // --help message from fuse_lowlevel_new.  Sigh...
        //
        // llops_no_destroy: we will take responsibility for calling the
        // lowlevel destroy op.  See the comments in fuse_teardown for why.
        auto llops_no_destroy = llops;
        llops_no_destroy.destroy = nullptr;
        g_ll_destroy = llops.destroy;
        g_session = fuse_lowlevel_new(args, &llops_no_destroy, sizeof(llops_no_destroy), nullptr);
        fuse_opt_free_args(args);
        // It seems that we don't detect bad command line args until
        // here!
        if( g_session == nullptr )
            throw se(EINVAL, "fuse_lowlevel_new failed.  Unrecognized command line options??");

        crash_handler = crash_handler_arg;
        handle_all_signals();
        fuse_session_add_chan(g_session, g_channel);
        complain(LOG_NOTICE, "starting fuse_session_loop: %s-threaded, %sground", 
               multithreaded?"multi":"single",
               foreground?"fore":"back");
        if(multithreaded)
            err = fuse_session_loop_mt(g_session);
        else
            err = fuse_session_loop(g_session);

        complain(LOG_NOTICE, "fuse_session_loop returned %d.  Calling fuseful_teardown", err);
        fuseful_teardown();
	return err ? 1 : 0;
 }catch(std::exception& e){
    std::throw_with_nested(std::runtime_error("exception thrown by fuseful_main_ll.  call fuseful_teardown and return 1"));
    fuseful_teardown();
    return 1;
 }
                           
// fuseful_teardown is called "normally" immediately after
// fuse_session_loop returns.  It's also called "abnormally" when
// something throws unexpectedly in fuse_main_ll, by the top-level
// exception handler around app_mount (i.e., "main"), by our terminate
// handler and by the Fs123Subprocess-running thread.  It uses a
// std::atomic flag to guarantee that it returns immediately with no
// side-effects after the first call.  When it runs, it also takes
// extra care to zero out g_session and g_channel after it has
// "destroyed" them.
bool fuseful_teardown() try {
    // ????? What's the right order here ?????
    // It seems that the shutdown sequence suggested by examples/hello_ll.c
    // results in spurious calls to fusermount /path/to/mountpoint, which
    // are troublesome if you use 'umount -l' to permit client upgrades
    // without requiring termination of long-running processes.  The
    // conventional sequence is:
    //    fuse_remove_signal_handlers(g_session)
    //    fuse_session_remove_chan(ch)
    //    fuse_session_destroy(g_session)
    //    fuse_unmount(mountpoint, ch)
    //    fuse_opt_free_args(&args)
    //
    // The problem is that session_destroy closes the channel.  With a
    // destroyed channel as its second argument, fuse_unmount always
    // kernel-unmounts whatever is mounted at mountpoint.  But if
    // 'this' process has been umount -l'ed, some *other* process
    // might be managing that mount-point, and it's wrong for us to
    // unmount it.  On the other hand, if fuse_unmount is called with
    // a still-intact channel, it carefully avoids unmounting too
    // soon.
    //
    // So it looks like the solution is to call fuse_unmount before
    // fuse_session_destroy.  Almost ...
    //
    // In libfuse-2.9.2 (and earlier, probably fixed in 2.9.3)
    // fuse_unmount *incorrectly* closes the fuse_chan_fd twice: once
    // in fuse_kern_unmount and then a second time in
    // fuse_kern_chan_destroy.  If there are other threads running
    // (e.g., maintenance threads, eviction threads, etc.), they might
    // open an fd, and then find it closed "behind their back" by the
    // second, erroneous close in fuse_kern_chan_destroy.  If we
    // followed the conventional sequence this wouldn't be a problem
    // because fuse_session_destroy would have called the
    // fuse_lowlevel_ops destroy callback and we wouldn't have any
    // other threads running that could be confused by this bug.  To
    // work around, we arrange to call the destroy callback,
    // g_ll_destroy, *before* calling fuse_unmount.  We avoid calling
    // it again by nulling it out of the callback in the lowlevel_ops
    // structure we pass to fuse_lowelevel_new.
    static std::atomic_flag entered = ATOMIC_FLAG_INIT; // N.B. ATOMIC_FLAG_INIT is unnecessary and deprecated in C++20
    if(entered.test_and_set()){
        complain(LOG_WARNING, "fuseful_teardown:  Return immediately from re-invocation");
        return false;
    }
    invaltp.reset();
    if(g_session)
        fuse_remove_signal_handlers(g_session);
    libfuse_handler = nullptr;
    if(g_ll_destroy){
        auto tmp = g_ll_destroy;
        g_ll_destroy = nullptr;
        complain(LOG_NOTICE, "fuseful_teardown:  calling llops->destroy directly");
        (*tmp)(nullptr);
    }
    do_fuse_unmount();
    if(g_session){
        fuse_session_destroy(g_session);
        g_session = nullptr;
    }
    return true;
 }catch(std::exception& e){
    complain(LOG_CRIT, e, "fuseful_teardown: ignoring exception.  This probably won't end well.");
    return false;
 }
