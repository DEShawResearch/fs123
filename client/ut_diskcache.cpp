// A unit test for the diskcache.

#include "valgrindhacks.hpp"
#include "diskcache.hpp"
#include "fs123/content_codec.hpp"
#include <core123/diag.hpp>
#include <core123/envto.hpp>
#include <iostream>

using namespace core123;

//auto _diskcache = diag::declare_name("diskcache");

const int N=100;

reply123 synthetic_reply(int i){
    // ttl is only one second.
    auto ret = reply123{0, 99, "the contents is " + std::to_string(i), content_codec::CE_IDENT, 0, 1, 0, 0};
    return ret;
}

bool operator!=(const reply123& a, const reply123& b){
    return a.content != b.content;
}

int main(int argc, char **argv){
    auto diagnames = envto<std::string>("Fs123DiagNames", "");
    if(!diagnames.empty()){
        set_diag_names(diagnames);
        set_diag_destination("%stderr");
        std::cerr << get_diag_names() << "\n";
    }
    if(argc != 2){
        std::cerr << "Usage: ut_diskcache cachedir\n";
        return 1;
    }
    std::unique_ptr<backend123> upstream;
    volatiles_t vols;
    vols.dc_maxfiles=100;
    vols.dc_maxmbytes=1000;
    diskcache dc(std::move(upstream), argv[1], 12345, vols); // tiny - 100 files and 1MB.

    // sleep for long enough to let the eviction thread run once
    // and set the injection_probability.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    for(int i=0; i<N; ++i){
        reply123 reply = synthetic_reply(i);
        std::string name = std::to_string(i);
        std::string h = dc.hash(name);
        dc.serialize(reply, h, name);
    }

    int ngood;
    ngood = 0;
    for(int i=0; i<N; ++i){
        reply123 reply = synthetic_reply(i);
        std::string name = std::to_string(i);
        std::string h = dc.hash(name);
        auto d = dc.deserialize(h);
        if(d.fresh()){
            if( d != synthetic_reply(i) )
                std::cerr << "Oops.  Mismatch on " << i << " got '" << d.content << "' expected '" << synthetic_reply(i).content << "'\n";
            ngood++;
        }
    }
    std::cout << "Hit " << ngood << "\n";
    
    // Now let's sleep for long enough that everything expiresand see what happens:
    std::cout << "Sleep for 2 seconds\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ngood = 0;
    for(int i=0; i<N; ++i){
        reply123 reply = synthetic_reply(i);
        std::string name = std::to_string(i);
        std::string h = dc.hash(name);
        auto d = dc.deserialize(h);
        if(d.fresh()){
            std::cout << i << " " << d.ttl().count() << "\n";
            if( d != synthetic_reply(i) )
                std::cerr << "Oops.  Mismatch on " << i << "\n";
            ngood++;
        }
    }
    std::cout << "Hit " << ngood << "\n";

    return 0;
}
