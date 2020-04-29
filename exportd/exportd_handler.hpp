#pragma once

#include "fs123/fs123server.hpp"
#include "exportd_cc_rules.hpp"
#include <core123/opt.hpp>
#include <core123/strutils.hpp>
#include <core123/str_view.hpp>
#include <core123/log_channel.hpp>
#include <memory>
#include <sys/stat.h>

struct exportd_options;

struct exportd_handler: public fs123p7::handler_base{
    bool strictly_synchronous() override { return true; }
    void a(fs123p7::req::up) override;
    void d(fs123p7::req::up, uint64_t inm64, bool begin, int64_t offset) override;
    void f(fs123p7::req::up, uint64_t inm64, size_t len, uint64_t offset, void* buf) override;
    void l(fs123p7::req::up) override;
    void s(fs123p7::req::up) override;
    void n(fs123p7::req::up) override;
    void x(fs123p7::req::up, size_t len, std::string name) override;
    void logger(const char* remote, fs123p7::method_e method, const char* uri, int status, size_t length, const char* date) override;
    secret_manager* get_secret_manager() override;

    const exportd_options& opts;
    std::unique_ptr<cc_rule_cache> rule_cache;
    core123::log_channel accesslog_channel;

    exportd_handler(const exportd_options&);
    ~exportd_handler(){}
protected:
    void err_reply(fs123p7::req::up, int eno);
    std::string cache_control(int eno, core123::str_view path, const struct stat* sb);
    uint64_t estale_cookie(int fd, const struct stat& sb, const std::string& fullpath);
    uint64_t estale_cookie(const std::string& fullpath, int d_type);
    uint64_t estale_cookie(const struct stat& sb, const std::string& fullpath);
    uint64_t monotonic_validator(const struct stat& sb);
    uint64_t compute_etag(const struct stat& sb, uint64_t estale_cookie);
};

struct exportd_options{
    enum esc_source_e{
                     ESC_IOC_GETVERSION,
                     ESC_GETXATTR,
                     ESC_SETXATTR,
                     ESC_ST_INO,
                     ESC_NONE};
    // Configurable "options":
    bool help = false;
    const char* PROGNAME;
#define ADD_ALL_OPTIONS \
        /* options related to startup */                                    \
        ADD_OPTION(std::string, export_root, ".", "root of exported tree"); \
        ADD_OPTION(std::string, chroot, "", "If non-empty, chroot and cd to this directory.  The --export-root and is opened in the chrooted context.  If empty, then neither chroot nor cd will be attempted"); \
        ADD_OPTION(bool, daemonize, false, "call daemon(3) before doing anything substantive"); \
        ADD_OPTION(std::string, pidfile, "", "name of the file in which to write the lead process' pid.  Opened *before* chroot."); \
        ADD_OPTION(std::string, portfile, "", "the bound port number is written to this file.  Opened *before* chroot."); \
        /* options related to staleness */                              \
        ADD_OPTION(uint64_t, mtim_granularity_ns, 400000, "granularity of st_mtim.  Files modified more recently than this are considered in-flux"); \
        ADD_OPTION(esc_source_e, estale_cookie_src, ESC_IOC_GETVERSION, "default source for estale cookies"); \
        ADD_OPTION(bool, fake_ino_in_dirent, false, "if true, the ino reported by dirent will not be the same as the file's ino"); \
        /* options related to default rules */                          \
        ADD_OPTION(int, default_rulesfile_maxage, 90, "cache-control rules files will be refreshed when they're this many seconds old"); \
        ADD_OPTION(std::string, no_rules_cc, "max-age=120,stale-while-revalidate=120,stale-if-error=10000000", "cache-control header used when no .fs123_cc_rules file is found.  This can be very long to encourage caching of (practically) immutable filesystems."); \
        ADD_OPTION(std::string, generic_error_cc, "max-age=30,stale-while-revalidate=30,stale-if-error=1000000", \
                   "cache-control header used when an error *other than ENOENT* is encountered.  It's not uncommon for such errors to be the result of server-side mis-configuration, so a long timeout is undesirable because it would lock in the error"); \
        ADD_OPTION(size_t, rc_size, 10000, "size of rules-cache");      \
        /* options related to shared secrets and fs123-secretbox */ \
        ADD_OPTION(std::string, sharedkeydir, "", "path to directory containing shared secrets (pre-chroot!)"); \
        ADD_OPTION(std::string, encoding_keyid_file, "encoding", "name of file containing the encoding secret. (if relative, then with respect to sharedkeydir, otherwise with respect to chroot)"); \
        ADD_OPTION(uint64_t, sharedkeydir_refresh, 43200, "reread files in sharedkeydir after this many seconds"); \
        /* options controlling the threadpool */                        \
        ADD_OPTION(size_t, threadpool_max, 0, "maximum number of threads in request handler threadpool"); \
        ADD_OPTION(size_t, threadpool_idle, 0, "number of idle threads in request handler threadpool.  0 means handle requests synchronously"); \
        /* options related to logging and diagnostics */                \
        ADD_OPTION(std::string, diag_names, "", "string passed to diag_names"); \
        ADD_OPTION(std::string, diag_destination, "", "log_channel destination for diagnostics"); \
        ADD_OPTION(std::string, accesslog_destination, "%none", "log_channel destination for access logs"); \
        ADD_OPTION(std::string, log_destination, "%syslog%LOG_USER", "log_channel destination for 'complaints'.  Format:  \"filename\" or \"%syslog%LOG_facility\" or \"%stdout\" or \"%stderr\" or \"%none\""); \
        ADD_OPTION(std::string, log_min_level, "LOG_NOTICE", "only send complaints of this severity level or higher to the log_destination"); \
        ADD_OPTION(double, log_max_hourly_rate, 3600., "limit log records to approximately this many per hour."); \
        ADD_OPTION(double, log_rate_window, 3600., "estimate log record rate with an exponentially decaying window of this many seconds."); \
        /* options for debug/development/testing only */                \
        ADD_OPTION(bool, argcheck, false, "Parse arguments and construct the server, but don't run it."); \
        ADD_OPTION(double, debug_add_random_delay, 0., "DEVEL/DEBUG ONLY.  NOT FOR PRODUCTION - A non-zero value will introduce random delays in every callback.  Potentially useful for exposing bugs related to threading, timeouts, etc.  The value is used as the 'b' parameter of a Cauchy distribution for the delay in seconds.");

#define ADD_OPTION(TYPE, NAME, DEFAULT, DESC) TYPE NAME = DEFAULT
    ADD_ALL_OPTIONS;
#undef ADD_OPTION
    exportd_options(core123::option_parser& p,  const char* progname) : PROGNAME(progname){
        p.add_option("help", "print this message to stderr", core123::opt_true_setter(help));
#define ADD_OPTION(TYPE, NAME, DEFAULT, DESC) p.add_option(#NAME, core123::str(DEFAULT), DESC, core123::opt_setter(NAME))
        ADD_ALL_OPTIONS;
#undef ADD_OPTION
#undef ADD_ALL_OPTIONS
    }
};

void exportd_global_setup(const exportd_options&);

// When we're comfortable with C++17, we might want to try:
//  https://github.com/Neargye/magic_enum
// Or one of the other suggestions at:
//  https://stackoverflow.com/questions/28828957/enum-to-string-in-modern-c11-c14-c17-and-future-c20
inline std::ostream& operator<<(std::ostream& os, exportd_options::esc_source_e e){
    switch(e){
#define CASE(name, enum) case exportd_options::enum: return os << name
        CASE("ioc_getversion", ESC_IOC_GETVERSION);
        CASE("getxattr", ESC_GETXATTR);
        CASE("setsattr", ESC_SETXATTR);
        CASE("st_ino", ESC_ST_INO);
        CASE("none", ESC_NONE);
#undef CASE
    default:
        throw std::runtime_error("impossible value in operator<<(exportd_options::esc_source_e): " + core123::str(int(e)));
    }
}

inline std::istream& operator>>(std::istream& is, exportd_options::esc_source_e& e){
    std::string s;
    if(is >> s){
#define CASE(name, enum) if(s == name){ e = exportd_options::enum;} else
            CASE("ioc_getversion", ESC_IOC_GETVERSION)
            CASE("getxattr", ESC_GETXATTR)
            CASE("setxattr", ESC_SETXATTR)
            CASE("st_ino", ESC_ST_INO)
            CASE("none", ESC_NONE)
        {
            is.setstate(std::ios::failbit);
        }
#undef CASE
    }
    return is;
}

