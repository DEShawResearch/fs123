OPTION(bool, fake_ino_in_dirent, false, "if true, readir replies set each entry's estale-cookie to 0.")
OPTION(uint64_t, mtim_granularity_ns, 4000000, "the granularity , in nanoseconds) of values recorded in a struct stat's st_mtim.");
OPTION(bool, allow_unencrypted_requests, true, "if false, then only accept requests encoded in the /e/ envelope");
OPTION(bool, allow_unencrypted_replies, false, "allow unencrypted replies to clients that don't have Accept-encodings: fs123-secretbox.  WARNING - this defeats the use of a --sharedkeydir");
OPTION(uint64_t, nprocs, 4, "run with this many listening processes");
OPTION(std::string, bindaddr, "127.0.0.1", "bind to this address");
OPTION(int32_t, port, -1, "(required) bind to this port");
OPTION(std::string, diag_names, "", "string passed to diag_names");
OPTION(std::string, diag_destination, "", "file to append diag output to");
OPTION(bool, daemonize, false, "call daemon(3) before doing anything substantive");
OPTION(std::string, pidfile, "", "name of the file in which to write the lead process' pid.  Opened *before* chroot.");
OPTION(uint64_t, heartbeat, 60, "number of seconds between each heartbeat to syslog");
OPTION(uint64_t, max_http_headers_size, 2000, "maximum bytes in incoming request HTTP headers");
OPTION(uint64_t, max_http_body_size, 500, "maximum bytes in incoming request HTTP body");
OPTION(uint64_t, max_http_timeout, 120, "http timeout on incoming request being complete");
OPTION(std::string, chroot, "/exports123", "chroot and cd to this directory.  The --export-root and --cache-control-file are opened in the chrooted context.  If empty, then neither chroot nor cd will be attempted");
OPTION(std::string, log_destination, "%syslog%LOG_USER", "destination for log records.  Format:  \"filename\" or \"%syslog%LOG_facility\" or \"%stdout\" or \"%stderr\" or \"%none\"");
OPTION(std::string, log_min_level, "LOG_NOTICE", "only send complaints of this severity level or higher to the log_destination");
OPTION(double, log_max_hourly_rate, 3600., "limit log records to approximately this many per hour.");
OPTION(double, log_rate_window, 3600., "estimate log record rate with an exponentially decaying window of this many seconds.");
OPTION(bool, syslog_perror, false, "add LOG_PERROR to the syslog openlog options at program startup.");
OPTION(std::string, accesslog_destination, "%none", "access logs will be sent to a log_channel set to this destination.");
OPTION(std::string, sharedkeydir, "", "path to directory containing shared secrets (pre-chroot!)");
OPTION(uint64_t, max_single_write, 16*1024*1024, "maximum number of bytes in any single write to an http socket");
// setting TCP_NODELAY is another way to avoid the problems that arise
// from the Nagle algorithm when write()s smaller than MSS.  Other
// alternatives are to adjust the MTU with ifconfig or the MSS with ip
// route.  The evhttp API in libevent doesn't give us a per-accept
// callback, so --tcp_nodelay does a setsockopt TCP_NODELAY for every
// request, even for keepalive connections, which seems wasteful.
// max_single_write is definitely preferred.
OPTION(bool, tcp_nodelay, false, "set TCP_NODELAY on accepted sockets");
OPTION(std::string, export_root, "/", "root of exported tree (post-chroot)");
OPTION(std::string, estale_cookie_src, "ioc_getversion", "source for estale_cookie.  Must be one of ioc_getversion|getxattr|setxattr|st_ino|none");
OPTION(std::string, cache_control_file, "", "cache control file (post-chroot) - empty means all cache control replies will carry the short timeout");
OPTION(uint64_t, max_age_short, 60, "max-age (sec) for short-timeout objects");
OPTION(uint64_t, max_age_long, 86400, "max-age (sec) for long-timeout objects");
OPTION(uint64_t, stale_while_revalidate_short, 30, "stale-while-revalidate time (sec) for short-timeout objects");
OPTION(uint64_t, stale_while_revalidate_long, 43200, "stale-while-revalidate time (sec) for long-timeout objects");
// Unless you're doing something very fancy, you can't "rotate" keys much faster than
// max_age_long, so there's no point in refreshing much more often.
OPTION(uint64_t, sharedkeydir_refresh, 43200, "reread files in sharedkeydir after this many seconds");
OPTION(std::string, encoding_keyid_file, "encoding", "name of file containing the encoding secret. (if relative, then with respect to sharedkeydir, otherwise with respect to chroot)");
OPTION(std::string, cache_control_directives, "public", "extra cache-control directives to add to every reply");
OPTION(std::string, cache_control_regex, "", "three-part colon-separated arg CC-good:CC-enoent:regex.  CC-xx are strings, not integers and may contain either or both max-age and stale-while-revalidate directives");
OPTION(bool, decentralized_cache_control, false, "use rules found in .fs123_cc_rules files under <export_root> to construct cache-control headers.  Fall back to --cache_control_file if no rules are found");
OPTION(uint64_t, dcc_cache_size, 1000000, "maximum size of the cache of .fs123_cc_rules files");
OPTION(int32_t, dcc_rulesfile_max_age, -1, "recheck rules files after this many seconds unless explicitly specified in the rules-file itself.  Default value of -1 means same as --max-age-short");

