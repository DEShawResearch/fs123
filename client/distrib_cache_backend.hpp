#pragma once

// distributed_diskcache: a 'backend123' that forwards (some) requests
// to peers and allows for super-large LAN-spanning caches.
//
// 'distrib_cache_backend' inherits from backend123 and is "stackable"
// with the other classes that inherit from backend123 (diskcache and
// backend123_http).

// Thus, to enable distributed caching, the fs123 mount process
// creates a 'distrib_cache_backend' in its 'init' callback, and
// integrates it into its 'backend' chain.
//
// A distrib_cache_backend is more of a 'broker' than a cache in its own
// right.  It responds to two sources of requests:

//    1 - requests made through backend123's standard 'refresh' API.
//        It typically forwards these to another backend (e.g., a
//        diskcache or an http backend).  The http backend might be
//        a direct path to the origin server, or it might be 'peer'
//        listening for requests from 'clients'.

//    2 - requests made by its 'clients'.  These arrive over the
//        network in the form of fs123 /p(assthrough) requests.  The
//        distrib_cache_backend instantiates an fs123p7::server with a
//        custom 'handler' that strips off the /p, and then forwards
//        the rest of the request to a backend123 - the 'server
//        backend'.  Upon completion of the server backend refresh,
//        the handler reformats the 'reply123' and sends it back over
//        the network, to the client, encoded as an fs123 reply.

// Thus, a distrib_cache_backend must be configured with two 'backend123's:
//
//  - the 'origin backend', to which 'refresh' requests are forwarded
//  - the 'server backend', to which 'peer requests' are forwarded.
// These are *in addition* to any 'peer' backends that it discovers.

// There are several useful ways to configure the origin and server
// backends with existing diskcache and http backends:
//
// 1 - diskcache-in-front:  The stackable backends look something like
//     this
//                                orgin
//    diskcache -> distrib_cache -------> backend123_http(origin server)
//             \               |
//              \             ---------> backend123_http(peer)
//               \             peer (if discovered)
//                |   
//      server -> -\ (server backend)
//
//   In this configuration, there is only one disk cache on each node.
//   *All* traffic handled by that node flows through (and is cached
//   by) that one diskcache.  Data retrieved on behalf of fuse mounts
//   on the node itself is intermingled with data retrieved on behalf
//   of clients.  To avoid loops, requests that reach the
//   distrib_cache from the 'server' (via the diskcache) must always
//   be forwarded to the origin server, and never to another peer.
//
// 2 - two diskcaches:  the stackable backends look something like this:
//
//                      origin
//      distrib_cache ---------> diskcache ---------> backend123_http(origin server)
//                  |                                   /
//                  ---------> backend123_http(peer)   /
//                    peer (if discovered)            /
//                                                   /
//       server --------------> diskcache----------->
//             (server backend)
//
//   In this configuration, there are two diskcaches: one for data
//   retrieved on behalf of the local filesystem activity, and a
//   separate one for data retrieved on behalf of clients.  They can
//   be sized and managed differently.  This configuration is more
//   tolerant of workloads that aren't naturally partitioned across
//   nodes.  E.g., when every node eventually "sees" every item
//   in a large data set.  The 'peer diskcache' will eventually
//   hold about 1/N of the data, while the 'local diskcache' will
//   do its best to hold on to frequently used data, but will probably
//   be subject to significant eviction pressure.
//
// 3 - diskcache-behind:  the stackable backends look something like this:
//
//                      origin
//      distrib_cache ---------> ------------------ diskcache------> backend123_http(origin server)
//                  |                                   /
//                  ---------> backend123_http(peer)   /
//                    peer (if discovered)            /
//                                                   /
//       server -------------->----------->---------/
//             (server backend)
//
//   There is one diskcache "shared" by all peers.  Local requests are
//   either forwarded to the local diskcache (1/N of them) or forwarded
//   to a peer ((N-1)/N of them).  Requests received from clients are
//   routed through the same diskcache as local requests.  This
//   configuration has increased LAN traffic, but also prevents local
//   activity from evicting data cached on local disk.
//
// Only the 'diskcache-in-front' and 'diskcache-behind' configurations
// has been tested, but there's no reason the the two-diskcache
// configuration shouldn't work.
//
// There are several components:
//
//  1 - the machinery for talking to peers - making requests and
//      receiving replies.
//  2 - the logic that decides which peer to use for any given
//      request.
//  3 - the machinery that manages peer discovery, creation and destruction.


#include "backend123.hpp"
#include "backend123_http.hpp"
#include "volatiles.hpp"
#include "fs123/fs123server.hpp"
#include <core123/threeroe.hpp>
#include <core123/http_error_category.hpp>
#include <core123/sew.hpp>
#include <core123/complaints.hpp>
#include <core123/stats.hpp>
#include <mutex>
#include <memory>
#include <algorithm>
#include <sys/socket.h>
#include <sys/types.h>

#define DISTRIB_CACHE_STATISTICS \
    STATISTIC(distc_inserted_peers)    \
    STATISTIC(distc_removed_peers)     \
    STATISTIC(distc_replaced_peers)    \
    STATISTIC(distc_suggestions_sent)   \
    STATISTIC(distc_suggestions_recvd)  \
    STATISTIC(distc_suggestions_checked)  \
    STATISTIC(distc_discourages_sent)  \
    STATISTIC(distc_discourages_recvd) \
    STATISTIC(distc_self_discourages_recvd) \
    STATISTIC(distc_server_refreshes)   \
    STATISTIC(distc_server_refresh_not_modified) \
    STATISTIC_NANOTIMER(distc_server_refresh_sec) \
    STATISTIC(distc_server_refresh_bytes) \
    

#define STATS_STRUCT_TYPENAME distrib_cache_statistics_t
#define STATS_MACRO_NAME DISTRIB_CACHE_STATISTICS
#include <core123/stats_struct_builder>
#undef DISTRIB_CACHE_STATISTICS
extern distrib_cache_statistics_t distrib_cache_stats;

struct peer{
    // Peers must satisfy:
    //  uniqueness - The name is used to find peers in the consistent
    //    hash peer_map.  It would be very confusing if two peers had
    //    the same "name".
    //
    //  persistence: - Ideally, the name would persist along with the
    //   data in the cache associated with the name.  If we restart
    //   the server tomorrow on a different port, but with the same
    //   data in cache, we'd like to advertise the same name.
    //
    // We could invent something, but this is *exactly* what UUIDs were
    // invented for.  In the 'discovery' process, we must learn
    // the peer's basurl and its name (i.e., uuid).  Diskcaches now
    // have a 'get_uuid' member that can be used for this.
    //
    // But there's a complication: it's not really possible to
    // guarantee uniqueness over time.  E.g., if a peer crashes
    // and restarts, it will generally get a new port (and hence
    // a new URL), but it will have the same uuid.  So while it's
    // practical to enforce a one-to-one mapping from url to uuid
    // *at any one time*, the mapping may change with time.
    //
    // Also note that we routinely allow clients for different
    // mount-points to share a diskcache, which implies that they'll
    // share the same UUID.  This is still permitted, but they MUST
    // NOT share the same multicast reflector
    // (-oFs123DistribCacheReflector).
    std::string uuid;
    std::string url;
    // Question: Who owns the backend?  That depends on how we were
    // constructed...  If we were constructed with a bare pointer,
    // then it's whoever owns the bare pointer.  If we were
    // constructed with a unique_ptr, then it's us.
    // Too-clever-by-half??
    std::unique_ptr<backend123> be_owner;
    backend123* be;
    using sp = std::shared_ptr<peer>;
    using up = std::unique_ptr<peer>;
    peer(const std::string& _uuid, const std::string& _url, backend123* _be) :
        uuid(_uuid), url(_url), be_owner(), be(_be)
    {}
    peer(const std::string& _uuid, const std::string& _url, std::unique_ptr<backend123> _beowner) :
        uuid(_uuid),
        url(_url),
        be_owner(std::move(_beowner)),
        be(be_owner.get())
    {}
    friend std::ostream& operator<<(std::ostream& os, const peer& p){
        return os << p.uuid << " " << p.url;
    }
};

// The peer_map is accessed in several ways:
//  lookup(url) - find the peer that handles this url in the consistent cache, whereby we look up a url (or other string)
//     and the peer_map tells us which peer to talk to to retrieve it.
//  insert - we are given a peer (which contains a uuid, a url and
//     a backend) and we add it to the consistent hash, so it may be
//     found by future insertions
//  check_url - returns whether the url is already known to the peer map
//  remove - we are given a url.  We must remove the peer associated with
//     that url from the map so that it is not found by subsequent lookups.

class peer_map_t{
    mutable std::mutex mtx;
    std::map<uint64_t, peer::sp> ring;
    std::map<std::string, std::string> url_to_uuid;

    // For diagnostic purposes, e.g., report_statistics, it's useful to
    // be able to loop over the peers.  Doing so requires another
    // map.  It would be a shame if generating a diagnostic report held up
    // "real" work, so we give it a second mutex, which is held
    // by insert_peer, remove_peer and forall_peers, but *not* by lookup.
    mutable std::mutex uu2pmtx;
    std::map<std::string, peer::sp> uuid_to_peer;
    
    static const int Nrep = 100;

    static uint64_t hash(const std::string& s, uint64_t n1 = 0){
        return core123::threeroe(s, n1).hash64();
    }
    
public:
    void insert_peer(peer::up up){
        static auto _distrib_cache = core123::diag_name("distrib_cache");
        distrib_cache_stats.distc_inserted_peers++;
        if(!up)
            throw std::logic_error("insert_peer called with NULL peer::up");
        peer::sp sp = std::move(up);
        std::unique_lock<std::mutex> ulk(mtx);
        DIAGf(_distrib_cache, "peer_map::insert_peer %s %s", sp->url.c_str(), sp->uuid.c_str());
        for(int i=1; i<=Nrep; ++i){
            auto h = hash(sp->uuid, i);
            peer::sp& rhpeersp = ring[h];
            // All Nrep entries in the ring are shared pointers pointing to the
            // same peer, so we only have to check this once.
            if(rhpeersp && i==1){
                DIAG(_distrib_cache, "peer_map::insert_peer:  replacing existing peer: " << (*rhpeersp) << " with: " << (*sp));
                url_to_uuid.erase(rhpeersp->url);
                distrib_cache_stats.distc_replaced_peers++;
            }
            rhpeersp = sp;
        }
        url_to_uuid[sp->url] = sp->uuid;
        if(_distrib_cache>=2){
            for(const auto& e : url_to_uuid){
                DIAG(_distrib_cache>=2, "url_to_uuid[" << e.first << "] = " << e.second);
            }
        }
        ulk.unlock(); // DO NOT TOUCH url_to_uuid or ring or any reference into them!

        // See comment above about uuid_to_peer and uu2pmtx.
        std::lock_guard<std::mutex> uu2plg(uu2pmtx);
        uuid_to_peer[sp->uuid] = sp;
        if(_distrib_cache>=2){
            for(const auto& e : uuid_to_peer){
                DIAG(_distrib_cache>=2, "uuid_to_peer[" << e.first << "] = " << (*e.second));
            }
        }
    }

    void remove_url(const std::string& url){
        static auto _distrib_cache = core123::diag_name("distrib_cache");
        distrib_cache_stats.distc_removed_peers++;
        std::unique_lock<std::mutex> ulk(mtx);
        auto found = url_to_uuid.find(url);
        if(found == url_to_uuid.end()){
            // This is "normal" but if it probably indicates a lot of churn
            // in our peer set.  It means we saw an ABSENT before we ever
            // saw a PRESENT.
            DIAG(_distrib_cache, "peer_map::remove_url("+ url + "): no such url in url_to_uuid");
            return;
        }
        std::string uuid  = found->second;
        DIAGf(_distrib_cache, "peer_map::remove_url %s %s.", url.c_str(), uuid.c_str());
        for(int i=1; i<=Nrep; ++i){
            auto h = hash(uuid, i);
            ring.erase(h);
        }
        url_to_uuid.erase(found);
        if(_distrib_cache>=2){
            for(const auto& e : url_to_uuid){
                DIAG(_distrib_cache>=2, "url_to_uuid[" << e.first << "] = " << e.second);
            }                
        }
        ulk.unlock(); // DO NOT TOUCH url_to_uuid or ring or any references into them!

        // See comment above about uuid_to_peer and uu2pmtx.
        std::lock_guard<std::mutex> uu2plg(uu2pmtx);
        uuid_to_peer.erase(uuid);
        if(_distrib_cache >= 2){
            for(const auto& e : uuid_to_peer){
                DIAG(_distrib_cache>=2, "uuid_to_peer[" << e.first << "] = " << (*e.second));
            }
        }
    }

    bool check_url(const std::string& url) const{
        std::lock_guard<std::mutex> lg(mtx);
        return url_to_uuid.count(url);
    }

    peer::sp lookup(const std::string& key) const {
        std::lock_guard<std::mutex> lg(mtx);
        auto h = hash(key);
        auto p = ring.upper_bound(h);
        if(p == ring.end())
            p = ring.begin();
        if(p == ring.end())
            throw std::runtime_error("peer_map::lookup empty cache?  That shouldn't happen");
        return p->second;
    }

    void forall_peers(std::function<void(const decltype(uuid_to_peer)::value_type&)> F) const {
        std::lock_guard<std::mutex> lg(uu2pmtx);
        for(const auto& e : uuid_to_peer){
            F(e);
        }
    }
};

struct distrib_cache_backend;
class peer_handler_t: public fs123p7::handler_base{
    using req_up = fs123p7::req::up;
public:
    peer_handler_t(distrib_cache_backend& _be) : be(_be){}
    bool strictly_synchronous() override { return true; }
    void a(req_up req) override { not_found(std::move(req)); }
    void d(req_up req, uint64_t /*inm64*/, bool /*begin*/, int64_t /*offset*/) override { not_found(std::move(req)); }
    void f(req_up req, uint64_t /*inm64*/, size_t /*len*/, uint64_t /*offset*/, void */*buf*/) override { not_found(std::move(req)); }
    void l(req_up req) override { not_found(std::move(req)); }
    void s(req_up req) override { not_found(std::move(req)); }
    void p(req_up, uint64_t, std::istream& ) override;
private:
    static void not_found(req_up req){
        exception_reply(std::move(req), http_exception(404, "Unrecognized request for peer handler"));
    }
    distrib_cache_backend& be;
};

struct distrib_cache_backend : public backend123{
    distrib_cache_backend(backend123* _upstream_backend, backend123* _server_backend, const std::string& scope, volatiles_t& volatiles);
    virtual ~distrib_cache_backend();
    bool refresh(const req123&, reply123*) override;
    std::ostream& report_stats(std::ostream& os) override;
    // *this is the subject of suggest_peer and discourage_peer.  I.e.,
    // we are making the suggestion.
    void suggest_peer(const std::string& peerurl) const;
    void discourage_peer(const std::string& peerurl) const;
    // *this is the object of suggested_peer and discouraged_per.  I.e.,
    // the suggestion is being made to us.
    void suggested_peer(const std::string& peerurl);
    void discouraged_peer(const std::string& peerurl);
    
    std::string get_url() const { return server_url; }
    std::string get_uuid() override { return server_backend->get_uuid(); }
    void regular_maintenance();
    friend class peer_handler_t;
private:
    backend123* upstream_backend;
    backend123* server_backend;
    std::string scope;
    peer_map_t peer_map;
    peer_handler_t peer_handler;
    std::unique_ptr<fs123p7::server> myserver;
    std::string server_url;
    std::future<void> server_future;
    volatiles_t& vols;
    std::future<void> udp_future;
    void udp_listener(); // returns to the udp_future
    std::atomic<bool> udp_done{false};
    acfd udp_fd; // used by udp_listener and {suggest,discourage}_peer
    struct sockaddr_in reflector_addr;
    void initialize_reflector_addr(const std::string&);
    bool multicast_loop;
};

