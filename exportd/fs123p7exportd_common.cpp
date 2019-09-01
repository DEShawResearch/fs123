// Common code and functions used by all variants of fs123export7d_*.cpp
#include "fs123p7exportd_common.hpp"
#include "fs123/httpheaders.hpp"
#include "fs123/acfd.hpp"
#include <core123/exnest.hpp>
#include <core123/syslog_number.hpp>
#include <core123/sew.hpp>
#include <core123/http_error_category.hpp>
#include <core123/log_channel.hpp>
#include <event2/listener.h>
#include <fstream>

using namespace core123;

acfd sharedkeydir_fd;
server_stats_t server_stats;

namespace {
auto _proc = diag_name("proc");

log_channel accesslog_channel;

// Under high concurrency tests with ab, libevent spends too much time
// reporting "too many open files" errors, so we suppress consecutive
// duplicates to give it a chance to do some real work!
void dup_suppress_log(int severity, const char *msg) {
    static std::string prevmsg;
    static unsigned long prevcount{0};
    if (msg == prevmsg) {
	if (prevcount == 0) {
	    prevcount++;
	    complain("duplicate message, suppressing consecutive repeats after this...");
	}
	return;
    } else {
	prevmsg = msg;
	prevcount = 0;
    }
    complain("severity %d : %s", severity, msg);
}

void setup_evhttp_params(struct evhttp *eh) {
    // some default settings for all http listeners
    // max_http_headers_size should be enough for typical headers
    evhttp_set_max_headers_size(eh, FLAGS_max_http_headers_size);
    // max http_body_size can be small since fs123 has no incoming body
    evhttp_set_max_body_size(eh, FLAGS_max_http_body_size);
    // if unspecified, libevent times out connections after 50
    // seconds.  To avoid 'AH01102: error reading status line'
    // problems with httpd ProxyPass, it's necessary to make exportd's
    // timeout longer than httpd's.  httpd defaults to 60, so we
    // default to 120.
    evhttp_set_timeout(eh, FLAGS_max_http_timeout);
    evhttp_set_allowed_methods(eh, EVHTTP_REQ_GET|EVHTTP_REQ_HEAD);
}

} // namespace <anon>

std::unique_ptr<selector_manager> the_selmgr;

DEFINE_uint64(nprocs, 4, "run with this many listening processes");
DEFINE_string(bindaddr, "127.0.0.1", "bind to this address");
DEFINE_int32(port, -1, "(required) bind to this port");
DEFINE_string(diag_names, "", "string passed to diag_names");
DEFINE_string(diag_destination, "", "file to append diag output to");
DEFINE_bool(daemonize, false, "call daemon(3) before doing anything substantive");
DEFINE_string(pidfile, "", "name of the file in which to write the lead process' pid.  Opened *before* chroot.");
DEFINE_uint64(heartbeat, 60, "number of seconds between each heartbeat to syslog");
DEFINE_uint64(max_http_headers_size, 2000, "maximum bytes in incoming request HTTP headers");
DEFINE_uint64(max_http_body_size, 500, "maximum bytes in incoming request HTTP body");
DEFINE_uint64(max_http_timeout, 120, "http timeout on incoming request being complete");
DEFINE_string(chroot, "/exports123", "chroot and cd to this directory.  The --export-root and --cache-control-file are opened in the chrooted context.  If empty, then neither chroot nor cd will be attempted");
DEFINE_string(log_destination, "%syslog%LOG_USER", "destination for log records.  Format:  \"filename\" or \"%syslog%LOG_facility\" or \"%stdout\" or \"%stderr\" or \"%none\"");
DEFINE_string(log_min_level, "LOG_NOTICE", "only send complaints of this severity level or higher to the log_destination");
DEFINE_double(log_max_hourly_rate, 3600., "limit log records to approximately this many per hour.");
DEFINE_double(log_rate_window, 3600., "estimate log record rate with an exponentially decaying window of this many seconds.");
DEFINE_bool(syslog_perror, false, "add LOG_PERROR to the syslog openlog options at program startup.");
DEFINE_string(accesslog_destination, "%none", "access logs will be sent to a log_channel set to this destination.");

DECLARE_string(cache_control_file);
DECLARE_string(export_root);
DECLARE_bool(tcp_nodelay);

// N.B.  other sharedkeydir options are DEFINE'ed in selector_manager111.cpp.
// The sharedkeydir is here so we can open it before chroot.
DEFINE_string(sharedkeydir, "", "path to directory containing shared secrets (pre-chroot!)");

void setup_common(const char *progname, int *argcp, char ***argvp) {
    gflags::SetUsageMessage("Usage: " +std::string(progname)+ " [--options]");
    gflags::ParseCommandLineFlags(argcp, argvp, true);

    // any inconsistencies in "args"?
    if(FLAGS_daemonize && FLAGS_pidfile.empty())
        throw se(EINVAL, "You must specify a --pidfile=XXX if you --daemonize");
    if(FLAGS_port <= 0)
        throw se(EINVAL, "You must specify a positive --port");
    if(uint16_t(FLAGS_port) != FLAGS_port)
        throw se(EINVAL, "--port out of range");
    
    if(FLAGS_daemonize){
        // FIXME - daemon should be in sew.
        // We'll do the chdir ourselves after chroot.
        // and we'll keep stdout open for diagnostics.
        if(daemon(true/*nochdir*/, true/*noclose*/)!=0)
            throw se("daemon(true, true) failed");
    }

    unsigned logflags = LOG_PID|LOG_NDELAY;  // LOG_NDELAY essential for chroot!
    if(FLAGS_syslog_perror)
        logflags  |= LOG_PERROR;
    // N.B.  glibc's openlog(...,0) leaves the default facility alone
    // if it was previously set, and sets it to LOG_USER if it wasn't.
    openlog(progname, logflags, 0);
    auto level = syslog_number(FLAGS_log_min_level);
    set_complaint_destination(FLAGS_log_destination, 0666);
    set_complaint_level(level);
    set_complaint_max_hourly_rate(FLAGS_log_max_hourly_rate);
    set_complaint_averaging_window(FLAGS_log_rate_window);
    if(!startswith(FLAGS_log_destination, "%syslog"))
        start_complaint_delta_timestamps();
    
    accesslog_channel.open(FLAGS_accesslog_destination, 0666);

    if(!FLAGS_diag_names.empty()){
        set_diag_names(FLAGS_diag_names);
        set_diag_destination(FLAGS_diag_destination);
        DIAG(true, "diags:\n" << get_diag_names() << "\n");
    }
    diag_opt_tstamp = true;

    if(!FLAGS_pidfile.empty()){
        std::ofstream ofs(FLAGS_pidfile.c_str());
        ofs << sew::getpid() << "\n";
        ofs.close();
        if(!ofs)
            throw se("Could not write to pidfile");
    }

    if(!FLAGS_sharedkeydir.empty())
        sharedkeydir_fd = sew::open(FLAGS_sharedkeydir.c_str(), O_DIRECTORY|O_RDONLY);

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
    if(!FLAGS_chroot.empty()){
        sew::chdir(FLAGS_chroot.c_str());
        log_notice("chdir(%s) successful",  FLAGS_chroot.c_str());
        if(FLAGS_chroot != "/"){
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
    the_selmgr = std::make_unique<selector_manager111>(sharedkeydir_fd);    //  might throw!
    // somebody's going to write to a pipe with nobody at the
    // other end.  We don't want to hear about it...
    struct sigaction sa = {};
    sa.sa_handler = SIG_IGN;
    sew::sigaction(SIGPIPE, &sa, 0);

    event_set_log_callback(dup_suppress_log);

}

std::vector<event*> events2befreed;

void teardown_common(){
    // Only called once, in "main" thread, balancing 'setup_common'
    for(auto e : events2befreed)
        event_free(e);
    events2befreed.clear();
    gflags::ShutDownCommandLineFlags();
    sharedkeydir_fd.close(); // ok, even if bool(sharedkeydir_fd) is false.
}

extern void heartbeat(void *);

void setup_evtimersig(struct event_base *eb, ProcState *tsp) {
    auto sigcb = [] (evutil_socket_t, short signum, void *arg) -> void {
        try{
            auto b = static_cast<struct event_base *>(arg);
            complain(LOG_NOTICE, "Caught signal %d.  calling event_base_loopbreak(%p)", signum, (void*)b);
            event_base_loopbreak(b);
        }catch(std::exception& e){
            complain(e, "caught exception in sigcb");
        }
    };

    auto timecb = [] (evutil_socket_t, short, void *arg) -> void {
        try{
            heartbeat(arg);
            the_selmgr->regular_maintenance();
        }catch(std::exception& e){
            complain(e, "caught exception in timecb");
        }
    };
    auto e = event_new(eb, -1, EV_PERSIST, timecb, tsp);
    events2befreed.push_back(e);
    const struct timeval heartbeat{time_t(FLAGS_heartbeat), 0};
    if (e == nullptr)
	throw se("event_new on timeout failed");
    if (event_add(e, &heartbeat) < 0)
	throw se("event_add on timeout failed");
    for (auto sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) {
	auto flags = EV_SIGNAL;
	e = event_new(eb, sig, flags, sigcb, eb);
        events2befreed.push_back(e);
	if (e == nullptr)
	    throw se(errno, "event_new on signal failed");
	if (event_add(e, nullptr) < 0)
	    throw se(errno, "event_add on signal failed");
    }
    // signal handler for SIGUSR1 to reopen log
    auto sigusr1 = [] (evutil_socket_t, short /*what*/, void */*arg*/) -> void {
        try{
            complain(LOG_NOTICE, "SIGUSR1 caught.  reopening logs (including this one).");
            accesslog_channel.open(FLAGS_accesslog_destination, 0666);
            set_complaint_destination(FLAGS_log_destination, 0666);
            if(!startswith(FLAGS_log_destination, "%syslog"))
                start_complaint_delta_timestamps();
            complain(LOG_NOTICE, "SIGUSR1 caught.  reopened logs %s (this one) and accesslog: %s",
                     FLAGS_log_destination.c_str(),
                     FLAGS_accesslog_destination.c_str());
        }catch(std::exception& ex){
            complain(ex, "Error caught while handling SIGUSR1");
        }
    };
        
    e = event_new(eb, SIGUSR1, EV_SIGNAL|EV_PERSIST, sigusr1, eb);
    if(e == nullptr)
        throw se(errno, "failed to create sigusr handler event");
    events2befreed.push_back(e);
    if(event_add(e, nullptr) < 0)
        throw se(errno, "failed to add sigusr handler");
}

void accesslog(evhttp_request* evreq, size_t length, int status){
    // - Evhttp doesn't tell us the HTTP version.  Internally, it has
    // req->major and req->minor, but they're carefully hidden from
    // us.  Just write - instead of HTTP/%d.%d.
    server_stats.reply_bytes += length;
    if(FLAGS_accesslog_destination != "%none"){
        auto method = evhttp_request_get_command(evreq);
        auto evcon = evhttp_request_get_connection(evreq);
        char *remote;
        uint16_t port;
        evhttp_connection_get_peer(evcon, &remote, &port);
#if 0
        // Common Log Format (almost):
        //
        // I refuse to format the time *again*.  We've already
        // laboriously formatted the Date field in the header, but
        // Common Log Format requires a different format!  If we're
        // writing to syslog, it will prepend a timestamp.  If not,
        // too bad.
        auto record = fmt("%s - - [-] \"%s %s -\" %u %zd",
                                   remote,
                                   (method==EVHTTP_REQ_GET)? "GET" : (method==EVHTTP_REQ_HEAD)? "HEAD" : "OTHER",
                                   evhttp_request_get_uri(evreq),
                                   status, length);
#else
        // Our own format:  <ip> [<http-date>] "<url>" <status> <len> <fs123-errno>
        auto hdrs = evhttp_request_get_output_headers(evreq);
        const char *eno = evhttp_find_header(hdrs, HHERRNO);
        const char *date = evhttp_find_header(hdrs, "Date");
        auto record = fmt("%s [%s] \"%s %s\" %u %zd %s",
                                   remote, date?date:"-",
                                   (method==EVHTTP_REQ_GET)? "GET" : (method==EVHTTP_REQ_HEAD)? "HEAD" : "OTHER",
                                   evhttp_request_get_uri(evreq),
                                   status, length, eno?eno:"-");
#endif
        accesslog_channel.send(record);
    }
}

int http_status_from_evnest(const std::exception& e){
    // Work from the bottom up...
    // If we see a system_error in the http category, then take the status from the http error.
    // If we see a system_error in another category (e.g., system), then it's a 500.
    // If we get all the way to the top, the status is 500.
    for(auto& er : rexnest(e)) {
        const std::system_error *sep = dynamic_cast<const std::system_error*>(&er);
        if(sep){
            return (sep->code().category() == http_error_category()) ?
                sep->code().value() :
                500;
        }
    }
    return 500;
}    

// simple synchronous evhttp callback processes request and sends reply
// used by prefork and prethread.
void sync_http_cb(evhttp_request* evreq, void *arg) {
    auto tsp = static_cast<ProcState*>(arg);
    tsp->tctr++;
    int status;
    try{
	fs123Req req123{evreq};
	status = do_request(&req123, the_selmgr.get(), tsp->tctr);
    }catch(std::exception& e){
        complain(e, "std::exception caught in sync_http_cb:");
        // If any of the nested exceptions was a system error in the
        // http category we use the status of the deepest such
        // exception.
        status = http_status_from_evnest(e);
        // clear any headers or content that had already been
        // associated with evreq before the throw:
        evhttp_clear_headers(evhttp_request_get_output_headers(evreq));
        auto evb = evhttp_request_get_output_buffer(evreq);
        evbuffer_drain(evb, evbuffer_get_length(evb));
        // Add some content from the thrown exception
        for(auto& ep : exnest(e)){
            evbuffer_add_printf(evb, "%s\n", ep.what());
        }
        // Set Content-Type.  text/plain displays in browsers, but
        // application/octet-stream tries to save a file (libevent
        // defaults to Content-Type: text/html; charset=ISO-8859-1 )
        evhttp_add_header(evhttp_request_get_output_headers(evreq), "Content-Type", "text/plain");
    }
    // evhttp_send_reply with a null databuf is undocumented, but
    // looking at the code, it "clearly" sends the data already
    // associated with evhttp_request_get_output_buffer(evreq_).
    // It modifies the output_buffer, though, so get the length
    // before calling evhttp_send_reply.
    auto length = evbuffer_get_length(evhttp_request_get_output_buffer(evreq));
    evhttp_send_reply(evreq, status, nullptr, nullptr);
    accesslog(evreq, length, status);
};


struct evhttp_bound_socket *setup_evhttp(struct event_base *eb, struct evhttp *eh, 
					 void (*http_cb)(struct evhttp_request *, void *),
					 ProcState *tsp,
					 struct evhttp_bound_socket *ehsock) {
    evhttp_set_gencb(eh, http_cb, tsp);
    if (ehsock == nullptr) {
	// main thread, new socket
	ehsock = evhttp_bind_socket_with_handle(eh, FLAGS_bindaddr.c_str(), FLAGS_port);
	if (ehsock == nullptr)
	    throw se(errno, "evhttp_bind_socket failed");
	evutil_make_listen_socket_reuseable(evhttp_bound_socket_get_fd(ehsock));
    } else {
	// additional thread, existing socket.
        //
        // We can't call evhttp_accept_socket_with_handle because it
        // creates a listener with the LEV_OPT_CLOSE_ON_FREE flag set,
        // and that introduces a race condition when the thread shuts
        // down and tries to close the common socket.  So we do
        // exactly what evhttp_accept_socket_with_handle would have
        // done - except that we don't set LEV_OPT_CLOSE_ON_FREE in
        // flags.
        const int flags = LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_EXEC; // |LEV_OPT_CLOSE_ON_FREE;
        auto listener = evconnlistener_new(eb, NULL, NULL, flags, 0, evhttp_bound_socket_get_fd(ehsock));
        if(!listener)
            throw se("thread failed in evconnlistener_new");
        auto bound = evhttp_bind_listener(eh, listener);
        if(!bound){
            evconnlistener_free(listener);
	    throw se("thread failed in evhttp_bind_listener");
        }
    }
    setup_evhttp_params(eh);

    return ehsock;
}
