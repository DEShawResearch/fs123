#include "distrib_cache_backend.hpp"
#include "fs123/httpheaders.hpp"
#include <core123/strutils.hpp>
#include <core123/diag.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>

// N.B.  It's confusing.  Extensive comments are in distrib_cache_backend.hpp.

using namespace core123;
using namespace fs123p7;
using namespace std;

auto _distrib_cache = diag_name("distrib_cache");
auto _shutdown = diag_name("shutdown");

distrib_cache_statistics_t distrib_cache_stats;

namespace {

// distrib_cache_message:  encapsulate some of the details of sending, receiving
// and "parsing" udp messages.  It's still *very* raw, but maybe better than
// just having this code inline.  "Messages" are concatenations of NUL-terminated
// strings.  They're (currently) limited to 512 bytes.

// To send a message, we take a [begin, end) collection of string_view's
// and bundle them up for sendmsg, making sure to NUL-terminate each one.
//
// To receive a message, we instantiate an empty distrib_cache_message
// and call its 'recv' method.  After which, the 'parts' member is a
// vector of str_views to a local copy of the sender's NUL-terminated
// str_views.
//
// N.B.  We're breaking our standing rule about exceptions here: send
// *only* throw an exception when the fd itself looks completely
// borked.  If the only problem is garbled data, e.g., missing NULs,
// it just returns with an empty 'parts'.
struct distrib_cache_message{
    std::array<char,512> data;
    std::vector<core123::str_view> parts;

    template <typename ITER>
    static void send(int sockfd, const struct sockaddr_in& dest, ITER b, ITER e);
    void recv(int fd);
};

template <typename ITER>
/*static */
void
distrib_cache_message::send(int sockfd, const struct sockaddr_in& dest,
                 ITER b, ITER e){
    char zero = '\0';
    struct msghdr msghdr = {};
    msghdr.msg_name = (void*)&dest;
    msghdr.msg_namelen = sizeof(struct sockaddr_in);
    msghdr.msg_iovlen = 2*(e-b);
    struct iovec iov[msghdr.msg_iovlen];
    msghdr.msg_iov = iov;
    int i=0;
    while(b != e){
        iov[i].iov_base = const_cast<char*>(b->data());
        iov[i].iov_len = b->size();
        iov[i+1].iov_base = &zero;
        iov[i+1].iov_len = 1;
        i+=2;
        b++;
    }
    DIAG(_distrib_cache, "sendmsg with msghhdr.iovlen = " << msghdr.msg_iovlen);
    core123::sew::sendmsg(sockfd, &msghdr, 0);
}

void distrib_cache_message::recv(int fd){
    using namespace core123;
    if(!parts.empty())
        throw std::logic_error("distrib_cache_messages::recv:  may only be called once");
    // MSG_DONTWAIT may be superfluous because we've just poll'ed,
    // but it shouldn't do any harm, and protects us against
    // "spurious" wakeups (can that happen?)
    auto recvd = ::recv(fd, data.data(), data.size(), MSG_DONTWAIT);
    if(recvd < 0){
        if(errno == EAGAIN){
            complain(LOG_WARNING, "udp_listener:  unexpected EAGAIN from recv(MSG_DONTWAIT)");
            return; // empty message
        }
        throw se("recv(udp_fd) in udp_listener");
    }
    if(recvd > 0 && data[recvd-1] != '\0'){
        complain(LOG_WARNING, "distrib_cache_message::recv:  message is not NUL-terminated.  Treating as empty.");
        return;
    }
    parts.reserve(3); // we're expecting 3
    char *b = &data[0];
    char *e = &data[recvd];
    while(b < e){
        char* nextnul = std::find(b, e, '\0');
        parts.push_back(str_view(b, nextnul-b)); // N.B.  The NUL isn't *in* the str_view, but it's guaranteed to follow it.
        b = nextnul+1;
    }
}

bool is_multicast(const sockaddr_in& sai){
    return (ntohl(sai.sin_addr.s_addr)>>28 == 14); // 224.X.X.X through 239.X.X.X, top 4 bits are 1110
}

string cache_control(const reply123& r) {
    using namespace chrono;
    return str_sep("", "max-age=", 
                   duration_cast<seconds>(r.max_age()).count(),
                   ",stale_while_revalidate=",
                   duration_cast<seconds>(r.stale_while_revalidate).count());
}
} // namespace <anon>

distrib_cache_backend::distrib_cache_backend(backend123* upstream, backend123* server, const std::string& _scope, volatiles_t& volatiles) :
    upstream_backend(upstream),
    server_backend(server),
    scope(_scope),
    peer_handler(*this),
    vols(volatiles)
{
    // - instantiate an fs123p7::server.
    option_parser op;
    server_options sopts(op); // most of the defaults are  fine?
    op.set("bindaddr", "0.0.0.0");
    myserver = make_unique<fs123p7::server>(sopts, peer_handler);
    server_url = myserver->get_baseurl();
    sockaddr_in sain = myserver->get_sockaddr_in();
    char sockname[INET_ADDRSTRLEN];
    if(!inet_ntop(AF_INET, &sain.sin_addr, sockname, sizeof(sockname)))
        throw se("inet_ntop failed");
    complain(LOG_NOTICE, "Distributed cache server listening on %s port %d.  Unique name: %s\n", sockname, ntohs(sain.sin_port), get_uuid().c_str());

    auto self = make_unique<peer>(get_uuid(), server_url, upstream_backend);
    peer_map.insert_peer(move(self));

    // figure out where to send suggestions and discouragement packets:
    initialize_reflector_addr(envto<string>("Fs123DistribCacheReflector", "<unset>"));

    udp_fd = sew::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int yes = 1;
    sew::setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if(is_multicast(reflector_addr)){
        // Online advice suggests use of IP_MULTICAST_TTL,
        // IP_MULTICAST_IF and IP_MULTICAST_LOOP on the sending side.
        //
        // The default IP_MULTICAST_TTL is 1, which seems fine.
        //
        // Normally, we don't want to hear our own chatter, so we
        // disable IP_MULTICAST_LOOP by default.  But if we're running
        // multiple peers on the same host (e.g., for debugging or
        // regression testing), then we need to enable it.
        // WARNING - if we use this for testing, our regression config
        // will be meaningfully different from our production config.
        multicast_loop = envto<bool>("Fs123DistribCacheMulticastLoop", false);
        int enabled = multicast_loop;
        sew::setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &enabled, sizeof(enabled));
        //
        // We're not trying to bridge interfaces, so no need for
        // IP_MULTICAST_IF

        // Apparently, we have to bind the address *before* we join it?
        sew::bind(udp_fd, (const struct sockaddr*)&reflector_addr, sizeof(reflector_addr));

        // To receive packets, we have to join the multicast group:
        struct ip_mreq mreq;;
        mreq.imr_multiaddr = reflector_addr.sin_addr;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        sew::setsockopt(udp_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    }else{
        struct sockaddr_in recv_addr;
        // We're not sending to a multicast address, so assume that
        // there's a repeater out there that will send back to us
        recv_addr.sin_family = AF_INET;
        recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        recv_addr.sin_port = htons(0);
        sew::bind(udp_fd, (const struct sockaddr*)&recv_addr, sizeof(recv_addr));
    }

    // no throw after this point!  The destructor won't be called, so the
    // threads won't clean up properly.
    udp_future = async(launch::async, &distrib_cache_backend::udp_listener, this);
    server_future = async(launch::async,
                          [&]() { try {
                                  DIAGf(_distrib_cache, "calling myserver->run in thread");
                                  myserver->run();
                              }catch(exception& e){
                                  complain(e, "server thread exiting on exception.");
                              }
                          });
}

void
distrib_cache_backend::regular_maintenance() try {
    // suggest ourselves as a peer to our group.
    suggest_peer(server_url);
 }catch(exception& e){
    complain(e, "Exception thrown by distib_cache_backend::regular_maintenance:");
 }

std::ostream&
distrib_cache_backend::report_stats(std::ostream& os){
    os << distrib_cache_stats;
    peer_map.forall_peers([&os,this](const pair<string, peer::sp>& p){
                              if(p.second->be == upstream_backend)
                                  return;
                              os << "BEGIN_peer: " << p.first << "\n";
                              p.second->be->report_stats(os);
                              os << "END_peer: " << p.first << "\n";
                          });
    return os;
}

distrib_cache_backend::~distrib_cache_backend(){
    // Tell the world we're closing up shop
    DIAG(_shutdown, "~distrib_cache_backend: discourage_peer(self)");
    discourage_peer(server_url);
    // Shut down the server
    DIAG(_shutdown, "~distrib_cache_backend: myserver->stop");
    myserver->stop();
    DIAG(_shutdown, "~distrib_cache_backend: server_future.wait()");
    server_future.wait();
    // Shutting down the udp listener is tricky.  It will exit on the
    // next loop after we set udp_done.  That's under the control
    // of the poll(..., timeout), which is 100msec, so we shouldn't
    // have to wait long.
    udp_done = true;
    // We could *try* to wake the udp listener sooner by sending a
    // packet to it.  But we'd have to jump through hoops if
    // IP_MULTICAST_LOOP isn't enabled (it usually isn't), and it
    // would be unreliable anyway (c.f. the U in UDP).
    //
    // What if udp_listner is hung?  We can't carry on with the
    // destructor because udp_listener would access free'ed memory if
    // and when it ever wakes up.  "Hung" is tricky, though.  See the
    // comment in udp_listener about suggested_peer.
    //
    // This may be a situation where std::terminate is the right/only
    // answer?
    chrono::seconds how_long(vols.connect_timeout + vols.transfer_timeout + 10);
    DIAG(_shutdown, "~distrib_cache_backend: begin loop on udp_future.wait_for(" << ins(how_long) << ")");
    while(udp_future.wait_for(how_long) == future_status::timeout){
        complain(LOG_CRIT, "~distrib_cache_backend's udp_listner is hung.  You may have to kill -9 this process.");
        DIAG(_shutdown, "~distrib_cache_backend: iterate loop udp_future.wait_for(" << ins(how_long) << ")");
        //std::terminate();
    }
    try{
        DIAG(_shutdown, "~distrib_cache_backend: udp_future.get()");
        udp_future.get();
        complain(LOG_NOTICE, "distrib_cache_backend: udp_listener exited cleanly");
    }catch(exception& e){
        complain(e, "distrib_cache_backend: udp_listener exited on an exception.  Something is probably wrong but carry on and hope for the best.");
    }
    DIAG(_shutdown, "~distrib_cache_backend:  done!");
}

void
distrib_cache_backend::initialize_reflector_addr(const std::string& reflector) /* private */ try {
    // Aack.  Nothing but boilerplate.  Lots of boilerplate...
    auto first_colon = reflector.find(':');
    if(first_colon == string::npos)
        throw invalid_argument("No colon found.  Expected IP:PORT");
    std::string reflector_ip = reflector.substr(0, first_colon);
    std::string reflector_port = reflector.substr(first_colon+1);
    struct addrinfo* addrinfo;
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    // FIXME:  sew::getaddrinfo and sew::getnameinfo
    auto gairet = ::getaddrinfo(reflector_ip.c_str(), reflector_port.c_str(),
                              &hints, &addrinfo);
    if(gairet)
        throw runtime_error(strfunargs("getaddrinfo", reflector_ip, reflector_port, "...") + ": " + gai_strerror(gairet));
    if(addrinfo->ai_addrlen > sizeof(reflector_addr))
        throw runtime_error("getaddrinfo returned a struct bigger than a sockaddr_in ??");
    ::memcpy(&reflector_addr, addrinfo->ai_addr, addrinfo->ai_addrlen);
    ::freeaddrinfo(addrinfo);
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    gairet = ::getnameinfo((const sockaddr*)&reflector_addr, sizeof(reflector_addr),
                            hbuf, sizeof(hbuf),
                            sbuf, sizeof(sbuf),
                            NI_NUMERICHOST | NI_NUMERICSERV);
    if(gairet)
        throw runtime_error("getnameinfo couldn't make sense of reflector_addr");
    complain(LOG_NOTICE, "Sending distrib_cache peer discovery messages to %s:%s\n", hbuf, sbuf);
 }catch(std::exception&  e){
    throw_with_nested(runtime_error("error in distrib_cache_backend::initialize_reflector_addr(" + reflector + ")"));
 }

bool
distrib_cache_backend::refresh(const req123& req, reply123* reply) /*override*/ {
    if(req.no_peer_cache)
        return upstream_backend->refresh(req, reply);
    // Figure out which peer.
    peer::sp p = peer_map.lookup(req.urlstem);
    if(p->be == upstream_backend)
        return upstream_backend->refresh(req, reply);
    // We replace the /urlstem in 'req' with /p/urlstem
    DIAG(_distrib_cache, "forwarding to " << ((p->be == upstream_backend) ? "local: " : "remote: ") << p->uuid);
    auto myreq = req;
    try{
        myreq.urlstem = "/p" + req.urlstem;
        return p->be->refresh(myreq, reply);
    }catch(exception& e){
        complain(LOG_WARNING, e, "peer->be->refresh threw.  Discouraging future attempts to use that peer: " + p->url);
        discourage_peer(p->url);
        discouraged_peer(p->url);
        return upstream_backend->refresh(req, reply);
    }
 }

void
distrib_cache_backend::suggested_peer(const string& peerurl){
    //
    // accept_encodings is empty.  We make '/p' requests and get uninterpreted binary data
    // back.  The data *may* have an encoding, but we're oblivious to that, and we
    // don't want another layer of encryption or encoding added.
    //
    // vols was a constructor argument.

    // If it's already in the peer_map there's nothing to do.
    if(peer_map.check_url(peerurl)){
        DIAG(_distrib_cache, "suggested_peer(" +  peerurl +"): already known");
        return;
    }

    reply123 rep;
    unique_ptr<backend123_http> be;
    try{
        be = make_unique<backend123_http>(add_sigil_version(peerurl), "", vols);
        // Get the uuid, which also checks connectivity.
        req123 req("/p/p/uuid", req123::MAX_STALE_UNSPECIFIED);
        be->refresh(req, &rep);
        DIAG(_distrib_cache, "suggested_peer: new url: " + peerurl + " uuid: " + rep.content);
    }catch(exception& e){
        DIAGf(_distrib_cache, "Failed to connect with suggested peer: %s, calling discourage_peer", peerurl.c_str());
        return discourage_peer(peerurl);
    }
    // More checks??  E.g., check that /p/a should give us something that is
    // consistent with our own notion of the root's attributes?
    peer_map.insert_peer(make_unique<peer>(rep.content, peerurl, move(be)));
}

void
distrib_cache_backend::discouraged_peer(const string& peerurl){
    // Should we check first?  Or should we remove it immediately?
    // Checking first prevents "whiplash" when peers look down from
    // some places and up from others.  But it also potentially leads
    // to weird "split-brain" configurations.  Not checking more
    // aggressively falls back to using the 'upstream' which is
    // good if we can trust the 'discourage' notifications, but bad
    // if they're untrustworthy.
    complain(LOG_NOTICE, "discouraged_peer(" + peerurl + ")");
    if(peerurl == server_url){
        if(!multicast_loop)
            complain(LOG_WARNING, "Somebody is discouraging me from talking to my own upstream.  Nope...  Not gonna' do that.");
        return;
    }
    peer_map.remove_url(peerurl);
}

void
distrib_cache_backend::suggest_peer(const string& peer_url) const{
    DIAG(_distrib_cache, "suggest_peer(" + peer_url + "), scope=" + scope);
    str_view parts[3] = {str_view("P"), str_view(peer_url), str_view(scope)};
    distrib_cache_message::send(udp_fd, reflector_addr, &parts[0], &parts[3]);
}

void
distrib_cache_backend::discourage_peer(const string& peer_url) const{
    complain(LOG_NOTICE, "discourage_peer(" + peer_url + "), scope=" + scope);
    str_view parts[3] = {str_view("A"), str_view(peer_url), str_view(scope)};
    distrib_cache_message::send(udp_fd, reflector_addr, &parts[0], &parts[3]);
}

void
distrib_cache_backend::udp_listener() try {
    struct pollfd pfds[1];
    pfds[0].fd = udp_fd;
    pfds[0].events = POLLIN;
    while(!udp_done){
        distrib_cache_message msg;
        // N.B.  we can't just call blocking recv because if no
        // messages arrive, we would never check udp_done.
        if(sew::poll(pfds, 1, 100) == 0)
            continue; // check udp_done if quiet for 100msec.
        msg.recv(udp_fd);
        // messages have three parts:
        //   parts[0]: A command, either 'P'resent, 'A'bsent' or 'C'heck
        //   parts[1]: A URL
        //   parts[2]: The scope of the sender (to avoid crosstalk);
        if(msg.parts.size() != 3){
            complain("udp_listener: garbled msg with %zd NUL-terminated parts (expected 3)", msg.parts.size());
            continue;
        }
        if(msg.parts[2] != scope){
            // FIXME - it would be nice to report what what we know about ports and IP addresses,
            // but unfortunately, that's all now "hidden" inside msg.rec.
            complain(LOG_WARNING, "udp_listener: received message with incorrect scope. Got %s, expected %s. Is somebody else on our channel?",
                     msg.parts[2].data(), scope.c_str());
            continue;
        }
        // N.B.  suggested_peer can take a long time.  It might have to wait
        // for a refresh on the new peer to time out.  We can spin up
        // yet another thread,  or we can live with it.  The consequences
        // of living with it are:
        //  a) when one peer is flakey, we might not quickly respond
        //     to other peers coming and going.
        //  b) when a peer is flakey, it might take us approximately
        //     the http-timeout to shut down.
        // Neither of these seem worth adding more complexity to solve.
        switch(msg.parts[0][0]){
        case 'P': suggested_peer(string(msg.parts[1])); break;
        case 'A': discouraged_peer(string(msg.parts[1])); break;
        case 'C': break;
        default:
            complain("udp_listener: garbled msg: " + strbe(msg.parts));
            break;
        }
    }
 }catch(std::exception& e){
    complain(e, "udp_listener:  returning on exception.  No longer listening for peer discovery messages.");
    throw; // will be caught by udp_future.get() in ~distrib_cache_backend.
 }

void
peer_handler_t::p(req::up req, uint64_t etag64, istream&) try {
    string url = urlescape(req->path_info);
    if(req->query.data()) // req->query is non-null.  It might still be 0-length
        url += "?" + string(req->query);
    req123 myreq(url, req123::MAX_STALE_UNSPECIFIED);
    myreq.no_peer_cache = true;
    reply123 reply123;
    if(etag64){
        // make the reply 'valid' by setting eno and set a non-zero
        // etag64 so that backend_http adds an INM header.
        reply123.eno = 0;
        reply123.etag64 = etag64;
    }
    if(startswith(myreq.urlstem, "/p")){
        // We're looking at a /p/p/XXX request.  I.e., a request
        // that we are *not* supposed to forward to server_backend!
        // It's unlikely that there will ever be more than a couple
        // of these, so an if/else if/else pattern should do fine...
        if(myreq.urlstem == "/p/uuid"){
            req->add_header(HHERRNO, "0"); // needed to get through backend_http on the other end.
            return p_reply(move(req), be.get_uuid(), 0, "max-age=86400");
        }
        return exception_reply(move(req), http_exception(404, "Unknown /p request: " + myreq.urlstem));
    }
        
    DIAG(_distrib_cache, "/p request for " << myreq.urlstem);
    // N.B.  These requests will also be tallied in the statistics of
    // the server_backend, but the server_backend may also be getting
    // requests from others (see the ascii art in
    // distrib_cache_backend.hpp).
    atomic_scoped_nanotimer _t(&distrib_cache_stats.distc_server_refresh_sec);
    bool modified = be.server_backend->refresh(myreq, &reply123);
    distrib_cache_stats.distc_server_refreshes++;
    distrib_cache_stats.distc_server_refresh_bytes += reply123.content.size();
    string cc = cache_control(reply123);
    if(!modified){
        distrib_cache_stats.distc_server_refresh_not_modified++;
        return not_modified_reply(move(req), cc);
    }
    req->add_header(HHCOOKIE, str(reply123.estale_cookie));
    req->add_header(HHERRNO, str(reply123.eno));
    if(reply123.chunk_next_meta != reply123::CNO_MISSING){
        // Ugh...
        const char *xtra = (reply123.chunk_next_meta == reply123::CNO_EOF) ?
            " EOF" : "";
        req->add_header(HHNO, str(reply123.chunk_next_offset) + xtra);
    }
    // HHTRSUM
    return p_reply(move(req), reply123.content, reply123.etag64, cc);
 }catch(std::exception& e){
    complain(e, "Exception thrown by distrib_cache_backend::peer_handler::p.  Client will see 500 and will discourage others from connecting to us");
    exception_reply(move(req), e);
 }
