#ifndef __fs123_ioctl_hpp
#define __fs123_ioctl_hpp

#include <sys/ioctl.h>

struct fs123_ioctl_data{
    char buf[257];
};

#define DIAG_NAMES_IOC _IOW(0, 99, fs123_ioctl_data)
#define DIAG_OFF_IOC _IOW(0, 98, fs123_ioctl_data)
// #define NEWURL_IOC _IOW(0, 100, fs123_ioctl_data) // was thread-unsafe.  unsupported
#define CONNECT_TIMEOUT_IOC _IOW(0, 101,fs123_ioctl_data)
#define TRANSFER_TIMEOUT_IOC _IOW(0, 102,fs123_ioctl_data)
//#define NO_SYSLOG_SUPPRESSION_IOC _IOW(0, 103, fs123_ioctl_data)
#define PAST_STALE_WHILE_REVALIDATE_IOC _IOW(0, 104, fs123_ioctl_data)
#define STALE_IF_ERROR_IOC _IOW(0, 105, fs123_ioctl_data)
#define DISCONNECTED_IOC _IOW(0, 106, fs123_ioctl_data)
#define RETRY_TIMEOUT_IOC _IOW(0, 107, fs123_ioctl_data)
#define RETRY_INITIAL_MILLIS_IOC _IOW(0, 108, fs123_ioctl_data)
#define RETRY_SATURATE_IOC _IOW(0, 109, fs123_ioctl_data)
#define IGNORE_ESTALE_MISMATCH_IOC _IOW(0, 110, fs123_ioctl_data)
#define CACHE_TAG_IOC _IOW(0, 111, fs123_ioctl_data)
//#define FAKE_INO_IN_DIRENT_IOC _IOW(0, 112, fs123_ioctl_data)
#define DIAG_DESTINATION_IOC _IOW(0, 113, fs123_ioctl_data)
#define CURL_MAXREDIRS_IOC _IOW(0,114, fs123_ioctl_data)
#define LOG_MAX_HOURLY_RATE_IOC _IOW(0, 115, fs123_ioctl_data)
#define LOG_DESTINATION_IOC _IOW(0, 116, fs123_ioctl_data)
#define LOAD_TIMEOUT_FACTOR_IOC _IOW(0, 117, fs123_ioctl_data)
#define EVICT_LWM_IOC _IOW(0, 118, fs123_ioctl_data)
#define EVICT_TARGET_FRACTION_IOC _IOW(0, 119, fs123_ioctl_data)
#define EVICT_THROTTLE_LWM_IOC _IOW(0, 120, fs123_ioctl_data)
#define EVICT_PERIOD_MINUTES_IOC _IOW(0, 121, fs123_ioctl_data)
#define DC_MAXMBYTES_IOC _IOW(0, 122, fs123_ioctl_data)
#define DC_MAXFILES_IOC _IOW(0, 123, fs123_ioctl_data)
#define NAMECACHE_IOC _IOW(0, 124, fs123_ioctl_data)
#define ADD_PEER_IOC _IOW(0, 125, fs123_ioctl_data)
#define REMOVE_PEER_IOC _IOW(0, 126, fs123_ioctl_data)
#define INVALIDATE_INODE_IOC _IOW(0, 127, fs123_ioctl_data)

#endif
