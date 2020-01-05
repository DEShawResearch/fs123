#pragma once
#include "secret_manager.hpp"
#include "core123/expiring.hpp"
#include <string>
#include <mutex>
#include <chrono>

// sharedkeydir: a concrete secret_manager that finds share keys in
// directory and caches them for a limited time in memory.  The member
// functions are thread-safe.  I.e., they may be called freely by
// multiple threads.
//
// Methods:
//
// sharedkeydir(dirfd, encoding_sid_indirect, refresh_sec) - The
//   constructor.  Dirfd is an open file descriptor to a directory
//   containing the shared secrets.  The caller should not close the
//   file descriptor while the sharedkeydir is in use.  sharedkeydir's
//   destructor does not close the dirfd.
//
//   encoding_sid_indirect is used by the encode_sid() method.  See below.
//
//   refresh_sec is an integer that says how long secrets should be kept
//   in in-memory cache.  Requests, i.e., get_sharedkey and encoding_sid,
//   will be served from in-memory cache rather than by making filesystem
//   accesses if the data in cache is less than refresh_sec old.
//
// get_sharedkey(sid): Appends the string ".sharedkey" to the 'sid'
//   argument and treat the result as a relative path from dirfd
//   provided to the constructor.  The opened file is parsed in its
//   entirety by libsodium's 'hex2bin', and the resulting binary data
//   is returned as the secret.  Errors at any step result in a thrown
//   runtime_error.
//
//   Note that it is an error if the sid does not satisfy
//   secret_manager::legal_sid, which does not permit the '/'
//   character.  So the code will never look for a .sharedkey file
//   outside the shared key directory provided to the constructor.
//
// encoding_sid() - The encoding sid is stored in a file named in the
//   'encoding_sid_indirect' constructor argument.  I.e.,
//   get_encode_sid opens the file named by 'encode_sid_indirect' and
//   returns the contents.  The encode_sid_indirect filename must also
//   satisfy secret_manager::legal_sid.
//
// regular_maintenance() - May be called from time to time by maintenance
//   threads.  It prunes stale data from the cache.  It is not essential
//   for correct operation.
//
// report_stats(ostream) - Report some usage statistics on its argument.

// Legal sids are defined in secret_manager::legal_sid.  The rules are:
// A sid is "legal" if it's
//   non-empty
//   no more than 255 chars long
//   doesn't start with '.' 
//   is alphanumeric with underscore, hyphen and period,


struct sharedkeydir : public secret_manager{
public:
    sharedkeydir(int dirfd, const std::string& encoding_sid_indirect, unsigned refresh_sec);
    std::string get_encode_sid() override;
    secret_sp get_sharedkey(const std::string& sid) override;
    void regular_maintenance() override;
    std::ostream& report_stats(std::ostream&) override;
    ~sharedkeydir(){}
private:
    std::mutex mtx;
    core123::expiring<std::string> encode_sid;
    int dirfd;
    std::string encode_sid_indirect;
    core123::expiring_cache<std::string, secret_sp> secret_cache;
    std::chrono::system_clock::duration refresh_time;

    secret_sp refresh_secret(const std::string& sid);
    std::string refresh_encode_sid();
};
