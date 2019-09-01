#pragma once

#include "fs123request.hpp"
#include "do_request.hpp"
#include "selector_manager111.hpp"
#include <core123/complaints.hpp>
#include <core123/scoped_nanotimer.hpp>
#include <core123/diag.hpp>
#include <core123/stats.hpp>
#include <gflags/gflags.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <sys/types.h>


// gcc says union wait shadows wait(), sigh.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <sys/wait.h>
#pragma GCC diagnostic pop

#define STATS_STRUCT_TYPENAME server_stats_t
#define STATS_INCLUDE_FILENAME "server_statistic_names"
#include <core123/stats_struct_builder>
extern server_stats_t server_stats;


struct ProcState {
    unsigned tnum;
    std::atomic<unsigned long long> tctr;
    core123::scoped_nanotimer snt;
    ProcState(unsigned n = 0) : tnum{n}, tctr{0}, snt{} {}
};


DECLARE_bool(tcp_nodelay);
DECLARE_bool(sendfile);
DECLARE_bool(threeroe);
DECLARE_uint64(nprocs);
DECLARE_string(bindaddr);
DECLARE_int32(port);
DECLARE_string(diag_names);
DECLARE_bool(daemonize);
DECLARE_string(pidfile);
DECLARE_uint64(heartbeat);
DECLARE_uint64(max_http_headers_size);
DECLARE_uint64(max_http_body_size);
DECLARE_uint64(max_http_timeout);

// singleton selector_manager
extern std::unique_ptr<selector_manager> the_selmgr;

extern void sync_http_cb(evhttp_request* evreq, void *arg);
extern void setup_common(const char *progname, int *argcp, char ***argvp);
extern void teardown_common();
extern void setup_evtimersig(struct event_base *eb, ProcState *tsp);
extern struct evhttp_bound_socket *setup_evhttp(struct event_base *eb, struct evhttp *eh, 
					 void (*http_cb)(struct evhttp_request *, void *),
					 ProcState *tsp,
					 struct evhttp_bound_socket *ehsock = nullptr);

void accesslog(evhttp_request *, int status, size_t length);
int httpstatus_from_evnest(const std::exception& e);
