#include "fs123/acfd.hpp"
#include <core123/sew.hpp>
#include <iostream>
#include <vector>
#include <cassert>
#include <algorithm>
#include <cstring>

using namespace core123;

int main(int argc, char **argv){
    acDIR dirp = sew::opendir(argc>1 ? argv[1] : ".");
    struct dirent* de;
    char space[sizeof(struct dirent) + 128] = {};
    struct dirent* eofdirent = (struct dirent*)&space[0];
    strcpy(eofdirent->d_name, "/EOF");
    std::cout.setf(std::ios::unitbuf);
    off_t last_d_off = 0;
    std::vector<std::pair<off_t, std::string> > offmap;
    do{
        auto off = sew::telldir(dirp);
        // check that the offsets reported by telldir
        // are 'consistent' with those in the dirent
        // returned by readdir.
        assert(off == last_d_off);
        //std::cout << "off = " << off;
        de = sew::readdir(dirp);
        if(!de)
            de = eofdirent;
#ifndef __APPLE__
        last_d_off = de->d_off;
#else
	last_d_off = sew::telldir(dirp);
#endif
        std::cout << " " << de->d_name << " de->d_ino=" << de->d_ino << " de->d_off=" << last_d_off << " de->d_type=" << (int)de->d_type << "\n";
        offmap.push_back( std::make_pair(off, de->d_name) );
    }while(de != eofdirent);
    
    // Now check that we an seek 'randomly' to the offsets
    // we extracted above:
    for(int i=0; i<100; ++i){
        std::cout << "Shuffle " << i << "\n";
        std::random_shuffle(offmap.begin(), offmap.end());
        for(const auto& kv : offmap){
            sew::seekdir(dirp, kv.first);
            de = sew::readdir(dirp);
            if(de){
                //std::cout << kv.first << " " << kv.second << " " << de->d_name << "\n";
                assert(kv.second == de->d_name);
            }else{
                //std::cout << kv.first << " " << kv.second << " " << "/EOF" << "\n";
                assert(kv.second == "/EOF");
            }
        }
    }
    return 0;
}
    
