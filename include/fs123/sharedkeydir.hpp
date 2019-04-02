#pragma once
#include "secret_manager.hpp"
#include "core123/expiring.hpp"
#include <string>
#include <mutex>
#include <chrono>

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
