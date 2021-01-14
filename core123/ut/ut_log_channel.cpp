// This one is hard to test properly.  To really
// be sure, we'd have to figure out how syslogs are actually
// delivered and go look there.

#include "core123/log_channel.hpp"
#include "core123/strutils.hpp"
#include "core123/sew.hpp"
#include "core123/exnest.hpp"
#include "core123/datetimeutils.hpp"
#include "core123/ut.hpp"
#include <numeric>
#include <time.h>
#include <stdlib.h>

using core123::log_channel;
using core123::tp2dbl;
using core123::fmt;
using core123::circular_shared_buffer;
namespace sew = core123::sew;

int main(int, char **) try {
    log_channel lc("%syslog%LOG_INFO%LOG_USER", 0666);
    lc.send("This should go to syslog LOG_USER with level LOG_INFO");

    sew::umask(0); // we really mean 0666, not 0666 & ~umask
    lc.open("/tmp/logchannel.test", 0666);
    
    time_t rawtime ;
    sew::time(&rawtime);
    lc.send(fmt("This should go to /tmp/logchannel.test.  The time is now: %s",
                ::ctime(&rawtime)));
    sew::system("cat /tmp/logchannel.test; rm /tmp/logchannel.test");

    // Exercise the round-up of size to the next multiple of both
    // record_size() and getpagesize()
    lc.open("%csb^/tmp/logchannel.csb^nrecs=64^reclen=80", 0666);
    const size_t requested_nrecs=64;
    circular_shared_buffer reader("/tmp/logchannel.csb", 0, 80);
    CHECK(reader.record_size() == 80);
    CHECK(reader.size() * reader.record_size() % getpagesize() == 0);
    CHECK(reader.size() >= requested_nrecs);  // we asked for nrecs=64, we should get at least that many
    size_t filesz = reader.size() * reader.record_size();
    // and we couldn't have gotten  any less.
    CHECK(filesz - std::lcm(reader.record_size(), getpagesize()) < requested_nrecs);
    std::cout << fmt("getpagesize=%d, nrecs=64^reclen=80 produced a file with %zd records of size %zd\n",
                     getpagesize(), reader.size(), reader.record_size());

    lc.open("%csb^/tmp/logchannel.csb^nrecs=64", 0666);
    for(int i=0; i<100; ++i){
        timespec now;
        ::clock_gettime(CLOCK_REALTIME, &now);
        lc.send(fmt("\n%ld.%06ld This is the %d'th record in /tmp/logchannel.csb.",
                    long(now.tv_sec), now.tv_nsec/1000, i));
    }
    sew::system("cat /tmp/logchannel.csb; rm /tmp/logchannel.csb");
    return utstatus(1);
 }catch(std::exception& e){
    std::cerr << "Exception thrown:\n";
    for(const auto& v : core123::exnest(e))
        std::cerr << v.what() << "\n";
    return 1;
 }
            
