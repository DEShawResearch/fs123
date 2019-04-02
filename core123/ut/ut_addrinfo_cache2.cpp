#include "core123/addrinfo_cache.hpp"
#include "core123/periodic.hpp"
#include "core123/strutils.hpp"
#include "core123/scoped_nanotimer.hpp"
#include <arpa/inet.h>
#include <iostream>
#include <sstream>
#include <random>
#include <algorithm>

using core123::str;
core123::addrinfo_cache aic;

void
do_one_lookup(const char *line, bool chatty = true){
    std::ostringstream oss;
    auto res = aic.lookup(line, "");
    oss << line << ": status: " << res->status << " [";
    char buf[std::max(INET_ADDRSTRLEN, INET6_ADDRSTRLEN)];
    if(res->status==0){
        oss << "{";
        for(struct addrinfo* p = res->aip; p; p = p->ai_next){
            sockaddr_in* sin;
            sockaddr_in6* sin6;
            oss << "family:" << p->ai_family << " protocol:" << p->ai_protocol;
            switch(p->ai_family){
            case AF_INET:
                sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                oss << " addr:" << inet_ntop(p->ai_family, &sin->sin_addr, buf, sizeof(buf));
                break;
            case AF_INET6:
                sin6 = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
                oss << " addr:" << inet_ntop(p->ai_family, &sin6->sin6_addr, buf, sizeof(buf));
                break;
            default:
                oss << " addr:??";
                break;
            }
            oss << "}, {";
        }
        oss << "}";
    }
    oss << "]\n";
    if(chatty)
        std::cout << oss.str();
}


void
do_some_lookups(int N, int argc, const char **argv, bool chatty){
    static core123::scoped_nanotimer time_since_start;
    unsigned seed = time_since_start.elapsed();
    std::default_random_engine engine(seed);
    std::uniform_int_distribution<int> picker(1, argc-1);
    for(int i=0; i<N; ++i){
        auto j = picker(engine);
        //std::cout << "do_one_lookup(argv[" << j << "])" << std::endl;
        do_one_lookup(argv[j], chatty);
    }
    aic._check_invariant();
}

int main(int argc, const char **argv){
    const char* default_args[] = {"ut_addrinfo_cache", "example.com", "www.example.com", "nohost.nodomain", "yahoo.com",
                                  "enlogin1", "enlogin2", "enlogin3", "enlogin4", "enlogin5"};
    if(argc < 2){
        argv = default_args;
        argc = sizeof(default_args)/sizeof(*default_args);
    }

    // The memory bug in fs123 was exposed by valgrind when we did a
    // 'refresh' in the periodic thread before doing any lookups, and
    // then did a lookup in a fuse-responding thread.  Unfortunately,
    // this doesn't seem to expose it ???!!!
    std::cout << "This test is most informative when run under valgrind and/or tsan" << std::endl;

    core123::periodic refresh_thread{[&](){
                                         aic.refresh();
                                         std::cout << "refreshed" << std::endl;
                                         aic._check_invariant();
                                         return std::chrono::milliseconds(100);
                                     }};
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread([&](){
                    std::cout << "lookup(nyctestgardenproxy1) status: " << aic.lookup("nyctestgardenproxy1.nyc.desres.deshaw.com", {})->status << std::endl;;
                }).join();

    std::vector<std::thread> threads;
    for(int it=0; it<3; ++it){
        threads.push_back(std::thread(do_some_lookups, 10000, argc, argv, false));
    }
    aic._check_invariant();
    for(auto& t : threads){
        t.join();
    }

    return 0;
}
