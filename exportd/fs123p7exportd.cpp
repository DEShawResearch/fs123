// "Pre-thread" version of evhttp: multiple listeners in separate threads
// on same evhttp socket.  Analogous to pre-fork, just uses threads.
// Mark Moraes, D. E. Shaw  Research
#include <core123/diag.hpp>
#include <core123/autoclosers.hpp>
#include <thread>
#include <signal.h>

#include "fs123p7exportd_common.hpp"
#define PROGNAME "fs123p7exportd"

using namespace core123;

namespace {
auto _proc = diag_name("proc");
const unsigned long thread_done_delay_secs = 1;
// global everybody-please-exit flag for threads
std::atomic<bool> done{false};
std::vector< std::unique_ptr<ProcState> > ts;
} // namespace <anon>

void heartbeat(void *) {
    for (const auto& t : ts) {
	complain(LOG_INFO, "heartbeat: server thread %u on %s:%u handled %llu requests in %.9f secs",
                 t->tnum, gopts.bindaddr.c_str(), gopts.port,
                 t->tctr.load(), t->snt.elapsed()*1.e-9);
    }
}

int main(int argc, char **argv) try
{
    setup_common(PROGNAME, &argc, &argv);
				
    auto ebac = make_autocloser(event_base_new(), event_base_free);
    if (!ebac)
	throw se(errno, "event_base_new failed");

    ts.push_back(std::make_unique<ProcState>(0));
    setup_evtimersig(ebac, ts.back().get());
    
    auto ehac = make_autocloser(evhttp_new(ebac), evhttp_free);
    if (!ehac)
	throw se(errno, "evhttp_new failed");

    auto ehsock = setup_evhttp(ebac, ehac, sync_http_cb, ts.back().get());

    // Start additional http listener/server threads if needed
    std::vector<std::thread> threads;
    if (gopts.nprocs > 1) {
	// by using a separate event base and http listener for each thread,
	// all events for each thread are kept separate so no
	// inter-thread synchronization is needed (other than the done atomic).
	// the trick to starting additional libevent http servers on the
	// same (already bound) socket is to get the socket fd and
	// call accept_socket on it (rather than bind). Accepts() will
	// round-robin randomly across the different libevent http servers,
	// and the sockets produced by those accepts will then be handled
	// in the lucky thread that gets the fd on accept; the other threads
	// do  wakeup, but get -1 (EAGAIN) and dive back into epoll_wait.  Keepalive
	// should work fine too, since the same thread handles that socket
	// thereafter.  Each thread can handle lots of sockets, thanks
	// to each thread having a separate event loop.
	auto threadrun = [ehsock] (ProcState *tsp) {
	    try {
		auto ebthr = make_autocloser(event_base_new(), event_base_free);
		if (!ebthr)
		    throw se("thread failed to create event_base");
		auto ehthr = make_autocloser(evhttp_new(ebthr.get()), evhttp_free);
		if (!ehthr)
		    throw se("thread failed to create evhttp");
		setup_evhttp(ebthr, ehthr, sync_http_cb, tsp, ehsock);
		// timer callback to check done periodically and if done, kick
		// us out of main event dispatch loop
		auto timecb = [] (evutil_socket_t, short, void *arg) -> void {
		    if (done.load())
			event_base_loopbreak(static_cast<struct event_base *>(arg));
		};
		auto e = event_new(ebthr, -1, EV_PERSIST, timecb, ebthr.get());
		const struct timeval checkdone{thread_done_delay_secs, 0};
		if (event_add(e, &checkdone) < 0)
		    throw se("event_add on checkdone failed");
		if (event_base_dispatch(ebthr) < 0)
		    throw se("thead event_base_dispatch failed");
                event_free(e);
	    } catch(std::exception &e) {
		complain(e, "thread caught exception");
	    }
	    // may be redundant, but make sure other threads all know to finish up too.
	    done.store(true);
	    log_notice("thread %u on %s:%u handled %llu requests in %.9f secs",
                       tsp->tnum, gopts.bindaddr.c_str(), gopts.port,
                       tsp->tctr.load(), tsp->snt.elapsed()*1.e-9);
	};
	// already running one main thread so start count at 1
	for (decltype(gopts.nprocs) i = 1; !done.load() && i < gopts.nprocs; i++) {
	    ts.push_back(std::make_unique<ProcState>(i));
	    threads.emplace_back(threadrun, ts.back().get());
	}
    }

    log_notice("main thread started on %s:%u at %.9f",
               gopts.bindaddr.c_str(), gopts.port, ts[0]->snt.elapsed()*1.e-9);
    if (event_base_dispatch(ebac) < 0)
	complain("event_base_dispatch returned error %d : %m", errno);
    log_notice("main thread on %s:%u handled %llu requests in %.9f secs",
               gopts.bindaddr.c_str(), gopts.port, ts[0]->tctr.load(), ts[0]->snt.elapsed()*1.e-9);

    done.store(true);
    for (auto& t : threads) {
	t.join();
    }

    DIAGfkey(_proc, "finished main thread proc\n");
    teardown_common();
    return 0;
 }catch(std::exception& e){
    done.store(true);
    complain(e, "Shutting down because of exception caught in main");
    return 1;
 }
