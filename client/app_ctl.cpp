#include "fs123/fs123_ioctl.hpp"
#include "fs123/acfd.hpp"
#include <core123/sew.hpp>
#include <core123/exnest.hpp>
#include <core123/throwutils.hpp>
#include <core123/strutils.hpp>
#include <core123/complaints.hpp>
#include <iostream>
#include <cstring>

using namespace core123;

static const char *progname="<unknown>";

int complain(){
    std::cerr << progname << " /path/to/fs_mount_point cmd arg ...\n"
        "cmd is one of the following fs123 -o options without the 'Fs123' prefix:\n"
        " DiagNames\n"
        " DiagOff\n"
        " ConnectTimeout\n"
        " TransferTimeout\n"
        " LoadTimeoutFactor\n"
        " NameCache\n"
        " PastStaleWhileRevalidate\n"
        " StaleIfError\n"
        " Disconnected\n"
        " CacheTag\n"
        " RetryTimeout\n"
        " RetryInitialMillis\n"
        " RetrySaturate\n"
        " IgnoreEstaleMismatch\n"
        " DiagDestination\n"
        " LogDestination\n"
        " CurlMaxRedirs\n"
        " LogMaxHourlyRate\n"
        " EvictLwm\n"
        " EvictTargetFraction\n"
        " EvictThrottleLwm\n"
        " EvictPeriodMinutes\n"
        " CacheMaxMBytes\n"
        " CacheMaxFiles\n"
        ;
    return 1;
}

int app_ctl(int argc, char **argv) try {
    // FIXME! - if we add any more ioctls, we really
    // need getopt or something like it!
    progname = argv[0];
    if( argc > 1){
        argc--;
        argv++;
    }else{
        return complain();
    }
    if(strcmp(argv[0], "-h")==0 || strcmp(argv[0], "--help")==0 ){
        complain();
        return 0;
    }

    acfd fd;
    if( argc ){
        argc--;
        std::string name = *argv++;
        if( !endswith(name, "/.fs123_ioctl") ) {
            name += "/.fs123_ioctl";
        }
        fd = sew::open(name.c_str(), O_RDONLY);
    }else{
        return complain();
    }

    std::string cmd;
    if(argc){
        cmd = *argv++;
        argc--;
    }else{
        return complain();
    }
        
    int ioc;
    if(cmd == "diagnames" || cmd == "DiagNames"){
        ioc = DIAG_NAMES_IOC;
    }else if(cmd == "diagoff" || cmd == "DiagOff"){
        ioc = DIAG_OFF_IOC;
    }else if(cmd == "ConnectTimeout"){
        ioc = CONNECT_TIMEOUT_IOC;
    }else if(cmd == "TransferTimeout"){
        ioc = TRANSFER_TIMEOUT_IOC;
    }else if(cmd == "LoadTimeoutFactor"){
        ioc = LOAD_TIMEOUT_FACTOR_IOC;
    }else if(cmd == "NameCache"){
        ioc = NAMECACHE_IOC;
    }else if(cmd == "PastStaleWhileRevalidate"){
        ioc = PAST_STALE_WHILE_REVALIDATE_IOC;
    }else if(cmd == "StaleIfError"){
        ioc = STALE_IF_ERROR_IOC;
    }else if(cmd == "Disconnected"){
        ioc = DISCONNECTED_IOC;
    }else if(cmd == "CacheTag"){
        ioc = CACHE_TAG_IOC;
    }else if(cmd == "RetryTimeout"){
        ioc = RETRY_TIMEOUT_IOC;
    }else if(cmd == "RetryInitialMillis"){
        ioc = RETRY_INITIAL_MILLIS_IOC;
    }else if(cmd == "RetrySaturate"){
        ioc = RETRY_SATURATE_IOC;
    }else if(cmd == "IgnoreEstaleMismatch"){
        ioc = IGNORE_ESTALE_MISMATCH_IOC;
    }else if(cmd == "DiagDestination"){
        ioc = DIAG_DESTINATION_IOC;
    }else if(cmd == "LogDestination"){
        ioc = LOG_DESTINATION_IOC;
    }else if(cmd == "CurlMaxRedirs"){
        ioc = CURL_MAXREDIRS_IOC;
    }else if(cmd == "LogMaxHourlyRate"){
        ioc = LOG_MAX_HOURLY_RATE_IOC;
    }else if(cmd == "EvictLwm"){
        ioc = EVICT_LWM_IOC;
    }else if(cmd == "EvictTargetFraction"){
        ioc = EVICT_TARGET_FRACTION_IOC;
    }else if(cmd == "EvictThrottleLwm"){
        ioc = EVICT_THROTTLE_LWM_IOC;
    }else if(cmd == "EvictPeriodMinutes"){
        ioc = EVICT_PERIOD_MINUTES_IOC;
    }else if(cmd == "CacheMaxMBytes"){
        ioc = DC_MAXMBYTES_IOC;
    }else if(cmd == "CacheMaxFiles"){
        ioc = DC_MAXFILES_IOC;
    }else{
        fprintf(stderr, "Unrecognized command: %s\n", cmd.c_str());
        return complain();
    }

    while(argc--){
        const char *key = *argv++;
        fs123_ioctl_data rdo;
        if(strlen(key)>= sizeof(rdo.buf)){
            throw se(EINVAL, fmt("key (%s) too long.  Maximum %zd bytes\n", key, sizeof(rdo.buf)));
            return 1;
        }
        strcpy(rdo.buf, key);
        sew::ioctl(fd, ioc, &rdo);
    }
    return 0;
}catch(std::exception& e){
    complain();
    std::cerr << "Exception thrown:\n";
    for(auto& a : exnest(e)){
        std::cerr << a.what()  << "\n";
    }
    return 2;
 }
