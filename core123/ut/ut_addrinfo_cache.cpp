#include "core123/addrinfo_cache.hpp"
#include "core123/periodic.hpp"
#include "core123/strutils.hpp"
#include <arpa/inet.h>
#include <iostream>
#include <sstream>
#include <random>
#include <algorithm>

using core123::str;
core123::addrinfo_cache aic;

void
do_lookups(int argc, const char **argv, bool chatty = true){
    for(int i = 1; i<argc; ++i){
        std::ostringstream oss;
        const char *line = argv[i];
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
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void
do_some_lookups(int N, int argc, const char **argv, bool chatty){
    const char* argv_copy[argc+1];
    for(int i=1; i<argc; ++i)
        argv_copy[i] = argv[i];
    // Is this really the easiest way to get a per-thread random engine??
    auto tidstr = str(std::this_thread::get_id());
    std::seed_seq ss(std::begin(tidstr), std::end(tidstr));
    std::default_random_engine shuffle_engine(ss);
    for(int i=0; i<N; ++i){
        std::shuffle(argv_copy+1, argv_copy+argc, shuffle_engine);
        do_lookups(argc, argv_copy, chatty);
    }
}

int main(int argc, const char **argv){
    core123::periodic periodic_update_thread{[&](){aic.refresh(); return std::chrono::seconds(10);}};
    
    const char* default_args[] = {"ut_addrinfo_cache", "example.com", "www.example.com", "nohost.nodomain", "yahoo.com",
                                  "enlogin1", "enlogin2", "enlogin3", "enlogin4", "enlogin5"};
    if(argc < 2){
        argv = default_args;
        argc = sizeof(default_args)/sizeof(*default_args);
    }
    do_lookups(argc, argv);
    std::cout << "After lookups 1, aic.size: " << aic.size() << "\n";
    do_lookups(argc, argv);
    std::cout << "After second round of lookups, aic.size: " << aic.size() << "\n";
    aic.refresh(5);
    std::cout << "After refresh 2, aic.size: " << aic.size() << "\n";
    do_lookups(argc, argv);
    std::cout << "After lookups 3, aic.size: " << aic.size() << "\n";
    aic.refresh(0);
    std::cout << "After 4 setting max_size to 0 and doing refresh.  aic.size: " << aic.size() << "\n";
    do_lookups(argc, argv);
    std::cout << "After do_lookups 5.  aic.size: " << aic.size() << "\n";
    aic.refresh(0);

    // Now let's try to exercise things at fairly high speed -
    // multiple threads doing lookups, another thread frequently doing
    // aggressive refreshes to random max_sizes.  With luck, any bugs
    // in addrinfo_cache would segfault, but the best way to gain
    // confidence is to run this under valgrind and/or tsan.
    std::default_random_engine refresh_engine;
    std::uniform_int_distribution<> refresh_dist(0, argc+1);
    std::cout << "This test is most informative when run under valgrind and/or tsan" << std::endl;
    core123::periodic refresh_thread{[&](){
                                         auto n = refresh_dist(refresh_engine);
                                         aic.refresh(n);
                                         std::cout << "refresh(" << n << ")" << std::endl;
                                         return std::chrono::milliseconds(1);
                                     }};

    std::vector<std::thread> threads;
    for(int it=0; it<10; ++it){
        threads.push_back(std::thread(do_some_lookups, 2000, argc, argv, false));
    }
    for(auto& t : threads){
        t.join();
    }

    return 0;
}
