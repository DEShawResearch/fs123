#include "fs123request.hpp"
#include "fs123p7exportd_common.hpp"
#include <event2/keyvalq_struct.h>  // XXX only for diag
#include "selector_manager.hpp"
#include "fs123/httpheaders.hpp"
#include "fs123/stat_serializev3.hpp"
#include "fs123/content_codec.hpp"  // see FIXME near sizeof(fs123_secretbox_hdr)
#include "fs123/acfd.hpp"
#include <gflags/gflags.h>
#include <core123/complaints.hpp>
#include <core123/http_error_category.hpp>
#include <core123/threeroe.hpp>
#include <core123/sew.hpp>
#include <core123/pathutils.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/netstring.hpp>
#include <core123/strutils.hpp>
#include <core123/svto.hpp>
#include <memory>
#include <string>
#include <limits>
#include <algorithm>
#include <sys/ioctl.h>
#if __has_include(<linux/fs.h>)
#include <linux/fs.h>
#endif
#include <sys/xattr.h>
// #include <sodium.h> // only needed if we use a libsodium hash instead of threeroe.

using namespace core123;

DEFINE_bool(allow_unencrypted_replies, false, "allow unencrypted replies to clients that don't have Accept-encodings: fs123-secretbox.  WARNING - this defeats the use of a --sharedkeydir");

namespace{
auto _http = diag_name("http");
auto _secretbox = diag_name("secretbox");

constexpr size_t KiB = 1024;
 
DEFINE_bool(fake_ino_in_dirent, false, "if true, readir replies set each entry's estale-cookie to 0.");
DEFINE_uint64(mtim_granularity_ns, uint64_t(4000000), "the granularity (in nanoseconds) of values recorded in a struct stat's st_mtim.");
DEFINE_bool(allow_unencrypted_requests, true, "if false, then only accept requests encoded in the /e/ envelope");

// validate_path_info - throw an exception if the path_info "looks
//   bad" Note that this is an essential "security" component - we
//   reject any path_info that has a '/..' in it to prevent requests
//   escaping from the specified 'basepath'
void validate_path_info(const std::string& pi) try {
    if(pi.empty())
        return;   // An empty string is ok.

    if(pi.front() != '/')
        throw se(EINVAL, "path must start with a /");
    if(pi.back() == '/')
        throw se(EINVAL, "path may not end with a /");
    if(pi.find("//") != std::string::npos)
        throw se(EINVAL, "path may not contan //");

    if(pi.find('\0') != std::string::npos)
        throw se(EINVAL, "path may not contain NUL");

    if(pi.find("/../") != std::string::npos)
        throw se(EINVAL, "path may not contain /../");
    if(pi.size() >= 3 && pi.rfind("/..")  == (pi.size() - 3))
        throw se(EINVAL, "path may not end with /..");
 }catch(std::exception& e){
    std::throw_with_nested( std::runtime_error("validate_path_info(" + pi + ")"));
 }

uint64_t timespec2ns(const struct timespec& ts){
    return uint64_t(1000000000)*ts.tv_sec + ts.tv_nsec;
}

// ReplyPlus is a private collection of partially assembled pieces.
// It's not a particularly good or useful design to copy.  If you're
// re-implementing a 'F_READ' handler, all you have to do is implement
// do_read (at the bottom).

struct ReplyPlus {
    // max_ckib is the max chunk size to prevent a resource-exhaustion DoS.
    // Existing practice is 128KiB chunks, so 8MiB should be well outside normal parameters.
    // Remember that chunk size should not be varied willy-nilly, since
    // it is part of the URL and therefore part of the key used by http caches,
    // so varying chunk size results in duplication of data in any intermediate
    // cache (varnish, squid) etc as well as the disk cache of the client.
    // If an installation-wide change of chunk size is necessary, might be good to
    // flush all intermediate and client caches.
    static const size_t max_ckib_ = 8*KiB;  // 8*KiB means 8MiB max chunk
    static const time_t max_max_age = std::numeric_limits<time_t>::max();
    static_assert(max_ckib_  < std::numeric_limits<ssize_t>::max()/KiB, "max_ckib too large, must be less than 1/1024th of max value of ssize_t");

    fs123Req* req_;
    const char *inhdr_inm_;
    std::shared_ptr<per_selector> per_selector_;
    std::string full_path_, cache_control_, content_type_;
    unsigned long long reqid_;
    std::string secretid_;
    int status_;
    int check_inm_(const struct stat& sb, uint64_t estale_cookie);
    uint64_t do_estale_cookie_common_(int fd, std::string source, const struct stat& sb, const std::string& fullpath);
    uint64_t monotonic_validator(const struct stat& sb){
        // We once naively thought that we could rely on the
        // the 'nanosecond resolution' of st_mtim on modern Linux.
        // Nope!  Even though st_mtim has a nanosecond field, it
        // appears (on ext4 in the Linux-3.10 kernel) to have a
        // granularity of 1ms.  I.e., modifications made up to 1ms
        // apart may leave a file's mtim unchanged.  Later
        // modifications will cause the mtim to increase in increments
        // of roughly 1 million nanoseconds!  Consequently st_mtim
        // alone isn't a "strong validator" if writers might be making
        // modifications less than 1ms apart (which isn't hard at
        // all).
        //
        // To work around this, we have a command line option:
        //   --mtim_granularity_ns 
        // with default value 4 million, i.e., 4msec.
        //
        // Instead of using mtim directly as a validator, we instead
        // use:
        //    std::min( mtim, now-mtim_granularity ).
        // It produces a "never match" validator for the first few
        // milliseconds after a file is modified, but after the mtim
        // is safely in the past, (when it is no longer possible for a
        // modification to leave the mtim unchanged) it produces a
        // consistent, repeatable validator.
        //
        // FWIW, it looks like there has been some work on the kernel
        // timekeeping code in the 4.x kernels as part of the y2038
        // effort that *may* mitigate/fix the underlying granularity
        // problem.  I.e., with new (4.11) kernels, it might be
        // reasonable to set --mtim_granularity_ns=0.
        uint64_t mtim64 = timespec2ns(sb.st_mtim);
        // We're looking at st_mtim, not some fancy C++ <chrono>
        // thingy.  So use clock_gettime rather than std::chrono::whatever.
        struct timespec now_ts;
        sew::clock_gettime(CLOCK_REALTIME, &now_ts);
        uint64_t now64 = timespec2ns(now_ts);
        return std::min(mtim64, now64 - 2*FLAGS_mtim_granularity_ns); 
    }
    uint64_t compute_etag(const struct stat& sb, uint64_t estale_cookie, const std::string& secretid){
        // Hash the estale_cookie, the nanosecond st_mtim, st_size and
        // secretid together.  The validator is used for ETag/INM
        // and as a strong validator on proto=7.1 /a replies.
        //
        // The estale cookie is necessary because it can change even
        // if the mtime doesn't, and in order to be a "strong
        // validator", the etag must reflect changes in semantically
        // important headers (such as fs123-estale-cookie).
        //
        // The secretid is incorporated because if we're encrypting
        // replies and we're rotating secrets, we don't want caches to
        // hand out resources encrypted with "old" secrets after we've
        // started using "new" secrets.  N.B.  The secretid is an argument
        // and not this->secretid_ because sometimes we want a validator
        // that doesn't depend on the secret.
        //
        // BEWARE: *nothing* is guaranteed if clocks go backwards or
        // if users maliciously (or accidentally) roll back mtimes.
        // To defend (however imperfectly) against backward-running
        // clocks, we mix in the st_size as well.  It's no more
        // effort, and it *might* avoid a nasty surprise one day.
        // Using ctim might defend against malicious/accidental
        // futimens(), but it would defeat the ability to sync an
        // export_root between servers.  The ctim/mtim choice could
        // plausibly be another command line option.
        //
        uint64_t mtim64 = monotonic_validator(sb);
#if 1
        // threeroe isn't cryptographic, so it theoretically "leaks"
        // info about mtim64 and st_size to eavesdroppers.
        // (estale_cookie and secretid are public anyway).  BUT
        // there's not a lot of entropy in mtim64 and st_size,
        // so even a much stronger hash would be invertible by
        // brute-force:  just try plausible values mtim and st_size
        // until one works.
        return threeroe(mtim64, estale_cookie).
            Update(&sb.st_size, sizeof(sb.st_size)).
            Update(secretid).
            Final().first;
#else
        unsigned char inputs[secretid.size() + 24];
        unsigned char *p = &inputs[0];
        ::memcpy(p, &mtim64, sizeof(mtim64)); p += sizeof(mtim64);
        ::memcpy(p, &estale_cookie, sizeof(estale_cookie)); p += sizeof(estale_cookie);
        ::memcpy(p, &sb.st_size, sizeof(sb.st_size)); p += sizeof(sb.st_size);
        ::memcpy(p, secretid.data(), secretid.size()); p += secretid.size();
        assert( p - &inputs[0] == sizeof(inputs)) ;
   #if 0
        // crypto_shorthash is Sip-Hash.  It's not designed to be used
        // with a compromised key.  I.e., it is not designed to
        // prevent an attacker with knowledge of the key
        // ("fs123-secretbox") from recovering the input.  So even
        // ignoring the brute-force attack (above), it's not clear
        // that siphash is a better choice than threeroe.  A deeper
        // understanding of SipHash would help :-).
        uint32_t h[2];
        static_assert(crypto_shorthash_BYTES==8, "Expected shorthash to produce 8 bytes");
        static_assert(sizeof(h) == 8, "Expected uint32_t h[2] to be 8 bytes long");
        unsigned char key[crypto_shorthash_KEYBYTES] = "fs123-secretbox";
        crypto_shorthash(reinterpret_cast<unsigned char*>(&h[0]), inputs, sizeof(inputs), key);
   #else
        // crypto_generichash is Blake2b.  It's designed for keyless use,
        // so it may be better than Siphash for our purposes...
        uint32_t h[ crypto_generichash_BYTES_MIN/4 ];
        static_assert(crypto_generichash_BYTES_MIN == sizeof(h), "Weird alignment?");
        static_assert(crypto_generichash_BYTES_MIN >= 8, "need 8 bytes");
        crypto_generichash(reinterpret_cast<unsigned char*>(&h[0]), sizeof(h),
                           inputs, sizeof(inputs), nullptr, 0);
   #endif        
        return (uint64_t(ntohl(h[0]))<<32) | ntohl(h[1]);
#endif
    }
    uint64_t do_estale_cookie_(const struct stat& sb, const std::string& fullpath);
    uint64_t do_estale_cookie_(int fd, const struct stat& sb, const std::string& fullpath);
    uint64_t do_estale_cookie_(const std::string& fullpath, int d_type);
    void do_common_headers_(const struct stat* sbp = nullptr);
    void do_cc_headers_(int eno, const struct stat *sbp = nullptr, time_t time_since_change = max_max_age, time_t now_sec = 0);
    time_t time_since_change_(time_t ftime, time_t now_sec = sew::time(nullptr));
    void common_report_errno_(int eno);
    ReplyPlus(fs123Req* req, const selector_manager *selmgr, unsigned long long reqid) :
	req_{req}, content_type_{"application/octet-stream"},
	reqid_{reqid}, status_{-1},
        content_up_{}, content_{}, content_arena_{}{
	// The selector is passed to 'selmgr_->selector_match', to obtain
	// per_selector_ data.
	//
	// For now, we just accept anything in /SEL/EC/TOR, and use
	// the one-and-only selector_data that was established by
	// initialization code in main.  In the future, we envision
	// multiple matching selectors, each referring to an export_root,
	// a set of timeout parameters, etc.
	per_selector_ = selmgr->selector_match(req_->selector_);
        if(!per_selector_)
            throw se(EINVAL, "Uh oh.  selector_match isn't supposed to return a nullptr");

        // make sure the secretid_ is the same in Etag and when we
        // encode by getting it once-and-for-all here.
        secretid_ = per_selector_->get_encode_secretid();
    
	// if we were interested in more than one or two headers we'd want
	// to do the looping differently...
	auto headers = evhttp_request_get_input_headers(req_->evreq_);
	if(headers){
            inhdr_inm_ = evhttp_find_header(headers, "If-None-Match"); // yes - it uses strcasecmp.
            DIAGfkey(_http, "inhdr_inm = %s\n", inhdr_inm_? inhdr_inm_ : "<nullptr>");
        }else{
            inhdr_inm_ = nullptr;
            DIAGfkey(_http, "no request headers? inhdr_inm = <nullptr>\n");
        }

        // Deal with encrypted /e requests.  
        auto e_request = (req->function_ == "e");
        if(!FLAGS_allow_unencrypted_requests && !e_request)
            httpthrow(400, "Requests must be encrypted");
        if(e_request)
            remove_envelope_(req);

	validate_path_info(req_->path_info_);
	full_path_ = per_selector_->basepath() + req_->path_info_; // e.g., /proj/desres/root/Linux/x86_64/foo/bar
    }
    ssize_t get_chunklen_(uint64_t ckib);
    void remove_envelope_(fs123Req*);
    void do_attr_();
    void do_file_();
    void do_dir_();
    void do_link_();
    void do_statfs_();
    void do_xattr_();
    void do_numbers_();
    void do_request();	// only external entry point, other than constructor
private:
    std::unique_ptr<char[]> content_up_;
    str_view content_;
    str_view content_arena_;
    // we use a char[] for content rather than string so we can hand it
    // to libevent as a buffer with a function to free it
    // FIXME -  the leader_len and  trailer_len are set by the 
    // encoding strategy, which should be the responsibility of the
    // per_encoding-> interface. 
    void reserve_content_(size_t len, size_t leader_len=sizeof(fs123_secretbox_header)/*=284*/, size_t trailer_len=32){
        DIAGfkey(_http, "%llu reserve_content(%zu)\n", reqid_, len);
        if(content_up_)
            throw std::logic_error("ReplyPlus::reserve_content may only be called once!");
        content_up_ = std::unique_ptr<char []>(new char[len+leader_len + trailer_len]);
        content_arena_ = str_view(content_up_.get(), leader_len + len + trailer_len);
        content_ = str_view(content_up_.get() + leader_len, 0);
    }
    template <typename T>
    void add_content_(const T& from){
        DIAGfkey(_http, "%llu add_content<T>(from, %zu*%zu)\n", reqid_, from.size(), sizeof(*from.data()));
        auto len = from.size()*sizeof(*from.data());
        auto newlen = content_.size() + len;
        auto dest = const_cast<char*>(content_.data() + content_.size());
        resize_content(newlen);  // checks size!
        ::memcpy(dest, (void *)from.data(), len);
    }
    template <typename T>
    void set_content_(const T& from, const char *ctype = nullptr){
        DIAGfkey(_http, "%llu set_content<T>(from, %zu*%zu)\n", reqid_, from.size(), sizeof(*from.data()));
        auto len = from.size()*sizeof(*from.data());
        reserve_content_(len);
        add_content_(from);
	if (ctype)
	    content_type_ = ctype;
    }
    void resize_content(size_t newsz){
        if(newsz == content_.size())
            return;
        if(content_.data() + newsz > content_arena_.data() +  content_arena_.size())
            httpthrow(500, "Tried to resize_content beyond edge of arena");
        content_ = str_view(content_.data(), newsz);
    }

    void add_out_hdr(const char* n, const char* v) {
        // N.B.  evhttp_add_header strdup's both n and v, so
        // we don't have to worry about lifetimes.
        if( evhttp_add_header(evhttp_request_get_output_headers(req_->evreq_), n, v) != 0 )
            throw se(EINVAL, fmt("evhttp_add_header(%s, %s) returned non-zero", n, v));
    }
    void add_out_hdr(const char* n, const std::string& v){
        add_out_hdr(n, v.c_str());
    }
};

// remove_envelope:  replace req->{path_info_, query_ and function_} with the
// values obtained by opening the encrypted envelope.
void ReplyPlus::remove_envelope_(fs123Req* req) try{
    std::string decoded = per_selector_->decode_envelope(req->path_info_);
    DIAGkey(_secretbox, "decoded request: " <<  decoded << "\n");
    if(decoded[0] != '/')
        throw std::runtime_error("Expected /FUNCTION at beginning of decoded string");
    auto nextslash = decoded.find('/', 1);
    if(nextslash != std::string::npos && nextslash != 2)
        throw std::runtime_error("/FUNCTION must be followed immediately by a slash or there may be no more slashes");
    // The decoding looks clean.  Reconstruct function_, path_info_ and query_,
    // more-or-less the same way we did in the fs123rReq constructor:
    req->function_ = decoded[1];
    auto queryidx = decoded.find_last_of('?');
    if(queryidx != std::string::npos){
        req->path_info_ = decoded.substr(2, queryidx-2);
        req->query_ = decoded.substr(queryidx+1);
    }else{
        req->path_info_ = decoded.substr(2);
        req->query_ = "";
    }
    DIAGfkey(_secretbox, "decoded:  path_info: %s, query: %s\n", req->path_info_.c_str(), req->query_.c_str());
 }catch(std::exception& e){
  std::throw_with_nested(http_exception(400, "Failed to decode/decrypt /e request"));
 }

// check_inm_ - Handle If-None-Match.  (Don't be confused by the NM
// acronym, which is shorthand for If-None-Match, but is NOT shorthand
// for "304 Not Modified").

// Compute the etag for the current reply and add it to the outbound
// headers.  Then, if the request has an If-None-Match header, compare
// the computed etag with that in the INM, and if they match, add
// required headers (e.g., cache-control), set the status to 304 and
// return 304 (telling the caller that the request is handled).  If
// there is no match (either there is no INM header, or the INM header
// doesn't match), return 0, indicating that normal processing should
// continue.
//
// Note that check_inm_ is called *before* do_common_headers_, and if
// it returns 304, do_common_headers_ will not be called.
int ReplyPlus::check_inm_(const struct stat& sb, uint64_t esc) try {
    auto et64 = compute_etag(sb, esc, secretid_);
    add_out_hdr("ETag", '"' + std::to_string(et64) + '"');
    if(!inhdr_inm_)
        return 0;
    server_stats.INM_requests++;
    str_view inm_sv = inhdr_inm_;
    if( inm_sv.find(',') != str_view::npos )
        complain(LOG_WARNING, "If-None-Match contains a comma.  check_inm only checks the first tag in the header");
    // the If-None-Match header should be a double-quoted 64-bit integer,
    // exactly as provided by an earlier ETag header.
    uint64_t inm64;
    try{
        inm64 = parse_quoted_etag(inm_sv);
    }catch(std::exception& e){
        complain(LOG_WARNING, "If-None-Match not parseable as a uint64.  Is someone messing with us?");
        return 0;
    }
    DIAGkey(_http, "inmhdr: " << inhdr_inm_ << " inm64: " << inm64 << " et64: " << et64 << "\n");
    if(inm64 != et64)
        return 0;
    do_cc_headers_(0, &sb, time_since_change_(sb.st_mtime));
    status_ = 304;
    return 304;
 }catch(std::exception& e){
    std::throw_with_nested(http_exception(400, "in check_inm_"));
 }

// do_estale_cookie_ - only called for 'S_IFREG' files.
//   Does *not* set the HHCOOKIE header.  That's the caller's responsibility.
uint64_t ReplyPlus::do_estale_cookie_common_(int fd, std::string source, const struct stat& sb, const std::string& fullpath) try {
    // DANGER - the code in the d_e_c(fullpath, d_type) overload below
    // assumes that if src is neither "none" nor "st_ino", then the
    // only field that matters in the sb is the st_mode.  Don't
    // violate that assumption without changing code in both
    // locations!
    uint64_t value;
    if(source == "ioc_getversion"){
        // the third arg to ioctl(fd, FS_IOC_GETVERSION, &what) is a pointer
        // to what type?  The first clue is that FS_IOC_GETVERSION is defined
        // in headers as _IOR('v', 1, long).  That means the ioctl interface
        // is expecting a long.  BUT - the kernel code (in extN/ioctl.c, reiserfs/ioctl.c,
        // etc.) all looks like:
        //       return put_user(inode->i_generation, (int __user *)arg);
        // which means an int (not a long) will be transferred to
        // user-space.  Furthermore, inode::i_generation is declared
        // an __u32 in linux/fs.h.  So it looks like the kernel is
        // working with ints and u32s.  Finally, e2fsprogs uses an int
        // pointer.  Conclusion: it's an int - the definition as
        // _IOR(...,long) notwithstanding
        unsigned int generation = 0;
#ifdef FS_IOC_GETVERSION
        sew::ioctl(fd, FS_IOC_GETVERSION, &generation);
#else
	throw se(EINVAL, "FS_IOC_GETVERSION not defined");
#endif
        value = generation;
    }else if(source == "getxattr" || source == "setxattr") {
        // char buf[32] corresponds to the amount of space reserved
        // for the cookie in the client-side's struct reply123.
        char buf[32];
        static size_t bufsz = sizeof(buf)-1;  // -1 guarantees room for a terminal NUL
    tryagain:  // we may come back here if setxattr fails with EEXIST
#ifndef __APPLE__
        auto ret = fgetxattr(fd, "user.fs123.estalecookie", buf, bufsz);
#else
        auto ret = fgetxattr(fd, "user.fs123.estalecookie", buf, bufsz, 0, 0);
#endif
        if( source == "setxattr" && ret < 0 && errno == ENODATA){
            // getxattr failed, and we've been asked to do "setxattr",
            // so we (try to) set the attribute to the current time-of-day
            // now is decimal nanoseconds since the epoch
            using namespace std::chrono;
            value = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
            std::string now = std::to_string(value);
#ifndef __APPLE__
            auto setret = fsetxattr(fd, "user.fs123.estalecookie", now.c_str(), now.size(), XATTR_CREATE);
#else
            auto setret = fsetxattr(fd, "user.fs123.estalecookie", now.c_str(), now.size(), 0, XATTR_CREATE);
#endif	    
            if(setret<0 && errno == EEXIST){ // XATTR_CREATE noticed a race!
                source = "getxattr";
                goto tryagain;
            }
            if(setret < 0)
                throw se(errno, fmt("setxattr(%s) failed.  errno=%d\n", fullpath.c_str(), errno));
            return value;
        }
        if(ret<0)
            throw se(errno, "fgetxattr(" + fullpath + ", \"user.fs123.estalecookie\") failed");
        if(ret==0 || size_t(ret) > bufsz)
            throw se(EINVAL, "fgetxattr(" + fullpath + ", \"user.fs123.estalecookie\") returned unacceptable length: " +std::to_string(ret));
        value = svto<uint64_t>(str_view(buf, ret));
    }else if(source == "st_ino"){
        value = sb.st_ino;
    }else if(source == "none"){
        value = 0;
    }else{
        throw se(EINVAL, fmt("Unrecognized FS123_ESTALE_COOKIE_SRC: %s\n", source.c_str()));
    }
    return value;
 }catch(std::system_error& se){
    // What should we do when we can't get an estale cookie?
    // Unfortunately, we don't (May 2018) have a good way to
    // distinguish between "this URL is borked" and "this server is
    // borked".  So if we throw an exception, that typically results
    // in us sending a 50x, which can cause all sorts of fire drills
    // and immune responses in downstream caches, clients, proxies,
    // forwarders, redirectors, etc.  Until we have a better way to
    // communicate the limited scope of the problem (this URL) let's
    // just issue a 'complaint' and return 0.  This choice runs the
    // slight risk of 'incorrect' estale behavior, but has a much
    // lower risk of disrupting wider operations.
    complain(LOG_WARNING, se, std::string(__func__) + "(fullpath=" + fullpath+")");
    return 0;
 }

uint64_t ReplyPlus::do_estale_cookie_(const struct stat& sb, const std::string& fullpath){
    // We don't have an fd, but we might need one...
    // N.B.  any errors encountered here,  e.g., inability to open the file,
    // missing xattrs, etc. get reported with an HTTP 500 Internal Server Error.
    // A proxy cache might transform that into 502 Bad Gateway.
    // When handling them on the client-side, take note of the fact
    // that these are *request-specific* errors.  Yes - the server is
    // misconfigured.  But No - it's probably not a good idea to
    // defer all requests to this server.
    auto& src = per_selector_->estale_cookie_src();
    acfd acfd;
    if(src == "ioc_getversion" || src=="getxattr" || src == "setxattr") {
	// sanity check to avoid reading through symlink etc.
	if (!S_ISREG(sb.st_mode) && !S_ISDIR(sb.st_mode))
	    throw se(EINVAL, fmt("was asked for estale_cookie when !S_ISREG && !ISDIR(%o): %s",
					 sb.st_mode, fullpath.c_str()));
        acfd = sew::open(fullpath.c_str(), O_RDONLY | O_NOFOLLOW);
    }
    return do_estale_cookie_common_(acfd, src, sb, fullpath);
}

uint64_t ReplyPlus::do_estale_cookie_(const std::string& fullpath, int d_type){
    // When called from do_dir we don't even have a stat buffer.
    // Let's try to avoid unnecessary syscalls.
    auto& src = per_selector_->estale_cookie_src();
    if(src == "none")
        return 0;
    
    // Estale_cookie is only for DIRs and REGular files
    if(!(d_type == DT_DIR || d_type == DT_REG))
        return 0;
    struct stat sb;
    if(src == "st_ino"){
        sew::lstat(fullpath.c_str(), &sb);
        return sb.st_ino;
    }
    // DANGER - this assumes that the only thing in the sb
    // that matters to d_e_c_common is the st_mode.  Be
    // careful when changing the code!
    sb.st_mode = dtype_to_mode(d_type);
    return do_estale_cookie_(sb, fullpath);
}
 
uint64_t ReplyPlus::do_estale_cookie_(int fd, const struct stat& sb, const std::string& fullpath){
    auto& src = per_selector_->estale_cookie_src();
    return do_estale_cookie_common_(fd, src, sb, fullpath);
}

// time_since_change - ftime is the time when the last change (on a
// file) occurred, it is either st_mtime (for d or f) or st_ctime (for
// a).  Returns now-ftime, clipped on the low end at 1 second and on
// the high end at max_max_age.  The max_age in the cache-control
// header will be the min of time_since_change and max_age_X (X =
// short or long, per the config for that path).  The heuristic we are
// aiming for is to specify a relatively short max-age for files that
// changed recently, since they seem likely to be changed again
// (uninstalled, appended to), but we still get a little cache help to
// handle big parallel job launches with a new software build, for
// example.  As the file age (i.e. now-ftime) increases, the max-age
// we specify for it also increases till we hit the cap of the config
// file.
time_t ReplyPlus::time_since_change_(time_t ftime, time_t now_sec) {
    return clip(time_t(1), now_sec-ftime, max_max_age);
}

 void ReplyPlus::do_common_headers_(const struct stat* sbp){
    // we hopefully just did the fstat, so we try to do the
    // gettime as quickly after that as possible.
    auto now_sec = sew::time(nullptr);
    auto dt_change = max_max_age;
    if(sbp){
	dt_change = time_since_change_(sbp->st_ctime, now_sec);
	DIAGkey(_http, reqid_ << " now=" <<
	    now_sec << " mtime=" << sbp->st_mtime << " ctime=" <<
	    sbp->st_ctime << " dt_change=" << dt_change << "\n");

	// sanity check #1.  Might happen if clock jumps back after
	// some files were created, or files were restored with times
	// in future from some other machine
	if( now_sec < sbp->st_mtime ) {
            complain("mtime in the future for %s.  st_mtime=%lu, time=%lu\n",
                     req_->path_info_.c_str(), (unsigned long)sbp->st_mtime, (unsigned long)now_sec);
	}
        // On Unix ctime is strictly >= mtime except under extraordinary circumstances
        // (e.g., somebody's been fiddling with the clock).  We're relying pretty
        // heavily on that, so it's worth checking and complaining if it's not true...
	// Seems commonplace on files in /proc, however!
        if( sbp->st_ctim < sbp->st_mtim )
            complain("sbp->st_ctim (%.9f) < sbp->st_mtim (%.9f) for %s.\n",
		     sbp->st_ctim.tv_sec + 1.e-9* sbp->st_ctim.tv_nsec,
		     sbp->st_mtim.tv_sec + 1.e-9* sbp->st_mtim.tv_nsec,
		     req_->path_info_.c_str());
    }
    do_cc_headers_(0, sbp, dt_change, now_sec);
    add_out_hdr(HHERRNO, "0");
    status_ = 200;
}

// do_cc_headers_ :  add Cache-control and Date headers
void ReplyPlus::do_cc_headers_(int  eno, const struct stat *sbp, time_t dt_change, time_t now_sec){
    auto cc = per_selector_->get_cache_control(req_->function_, req_->path_info_, sbp, eno, dt_change);
    add_out_hdr("Cache-Control", cc);
    DIAGfkey(_http, "%llu common_report_errno added cache-control %s\n", reqid_, cc.c_str());
    add_out_hdr("Date", timet_to_httpdate(now_sec ? now_sec : sew::time(nullptr)));
 }

// common_report_errno_ 
// Use this to report an explicit, recognizable errno that the client
// or intermediates can safely cache, since it appears as an HTTP
// success with status 200.  Do NOT use this to report internal
// problems or protocol inconsistencies.
 void ReplyPlus::common_report_errno_(int eno){
    add_out_hdr(HHERRNO, std::to_string(eno));
    do_cc_headers_(eno);
    status_ = 200;
}

ssize_t ReplyPlus::get_chunklen_(uint64_t ckib) {
    if(ckib > max_ckib_)
	httpthrow(400, fmt("chunksize: %" PRIu64 " too large.  May not exceed %zu",
				    ckib, max_ckib_));
    return ckib*KiB;
}

void ReplyPlus::do_attr_() {
    server_stats.a_requests++;
    DIAGfkey(_http, "%llu do_attr %s\n", reqid_, full_path_.c_str());
    struct stat sb;
    if( ::lstat(full_path_.c_str(), &sb) < 0 ){
        return common_report_errno_(errno);
    }
    uint64_t esc = 0;
    if(S_ISREG(sb.st_mode) || S_ISDIR(sb.st_mode)){
        // getattr of regular file or directory requires an ESTALE-Cookie.
        esc = do_estale_cookie_(sb, full_path_);
        add_out_hdr(HHCOOKIE, std::to_string(esc));
    } else if (!S_ISLNK(sb.st_mode)) {
	return common_report_errno_(EINVAL);
    }
    do_common_headers_(&sb);
    std::ostringstream oss;
    oss << sb << '\n';
    if(req_->url_proto_minor_ >= 1){
        // The 7.1 protocol requires a 'strong validator' following the newline.
        // The 64-bit etag has exactly the properties that fs123
        // requires for a validator:
        oss << monotonic_validator(sb);
        DIAGfkey(_http, "7.1 style /a/ reply.  url_proto_minor_ = %d\n", req_->url_proto_minor_);
    }else{
        DIAGfkey(_http, "7.0 style /a/ reply.  url_proto_minor_ = %d\n", req_->url_proto_minor_);
    }
    set_content_(oss.str());
}

void ReplyPlus::do_dir_() try {
    int chunkfrombegin;
    int64_t chunkfrom;
    uint64_t ckib;
    server_stats.d_requests++;
    // TODO: use scanx to parse
    if (req_->query_.empty())
	httpthrow(400, fmt("no query string for %s", req_->path_info_.c_str()));
    
    svscan(req_->query_, std::tie(ckib, chunkfrombegin, chunkfrom));
    auto chunklen = get_chunklen_(ckib);
    auto fname = full_path_.c_str();
    DIAGfkey(_http, "%llu do_dir %jd %d %ju %zd %s\n",
	     reqid_, (uintmax_t)ckib, chunkfrombegin, chunklen, (intmax_t)chunkfrom, fname);
    // use open+fdopendir so we can use O_NOFOLLOW for safety
    acfd xfd = open(fname, O_DIRECTORY | O_NOFOLLOW | O_RDONLY);
    acDIR dir = ::fdopendir(std::move(xfd));
    if(!dir)
	return common_report_errno_(errno);
    // call fstat again, just to be sure...
    struct stat sb;
    sew::fstat(sew::dirfd(dir), &sb);
    uint64_t esc = do_estale_cookie_(sew::dirfd(dir), sb, full_path_);
    add_out_hdr(HHCOOKIE, std::to_string(esc));
    if( check_inm_(sb, esc) == 304 )
        return;
    struct dirent* de;
    long last_diroff;
    if(chunkfrombegin){
	last_diroff = sew::telldir(dir);  // could telldir(opendir()) really be non-zero??
    }else{
	sew::seekdir(dir, chunkfrom);
	last_diroff = chunkfrom;
    }
    reserve_content_(chunklen);
    while( (de = sew::readdir(dir)) ){
	std::ostringstream oss;
	// N.B.  The third field is *not* d_ino.  It's the file's estale_cookie.
        uint64_t estale_cookie;
        try{
            estale_cookie = FLAGS_fake_ino_in_dirent ? 0 : do_estale_cookie_(full_path_ + "/" + de->d_name, de->d_type);
        }catch(std::exception& e){
            // This might happen if the file was removed or replaced
            // between the readdir and whatever syscall we use to
            // get the estale_cookie.
            complain(e, "do_dir_: error obtaining estale_cookie for: "+full_path_ + "/" + de->d_name  + ".  Setting entry esc to 0");
            estale_cookie = 0;
        }
	oss << netstring(de->d_name) << " " << int(de->d_type) << " " << estale_cookie << "\n";
	const auto entry = oss.str();
	if(content_.size() + entry.size() > size_t(chunklen)){
	    break;
	}
	add_content_(entry);
	last_diroff = sew::telldir(dir);
    }
    add_out_hdr(HHNO, fmt(de? "%ld" : "%ld EOF", last_diroff));
    do_common_headers_(&sb);
 }catch( std::exception& e){
    std::throw_with_nested(http_exception(400, fmt("do_dir path_info=\"%s\" expected chunksizeKiB;offset;... query: \"%s\"",
                                                            req_->path_info_.c_str(), req_->query_.c_str())));
 }

void ReplyPlus::do_file_() try {
    int64_t chunkfrom;
    uint64_t ckib;
    server_stats.f_requests++;
    if (req_->query_.empty())
	httpthrow(400, fmt("no query string for %s", req_->path_info_.c_str()));
    svscan(req_->query_, std::tie(ckib, chunkfrom));
    auto chunklen = get_chunklen_(ckib);
    chunkfrom *= KiB; // KiB in the URL, bytes here.
    auto fname = full_path_.c_str();
    DIAGfkey(_http, "%llu do_file %jd %zd %jd %s\n",
	     reqid_, (uintmax_t)ckib, chunklen, (uintmax_t)chunkfrom, fname);
    acfd acfd = ::open(fname, O_RDONLY | O_NOFOLLOW);
    if( !acfd ){
	// we were presumably able to lstat it, but couldn't open it.
	// This happens "normally" when an unreadable file
	// is in a readable directory.
	return common_report_errno_(errno);
    }
    // call fstat again, just to be sure...
    struct stat sb;
    sew::fstat(acfd, &sb);
    if(!S_ISREG(sb.st_mode))
	httpthrow(400, fmt("expected file, but mode is %o for %s", sb.st_mode, req_->path_info_.c_str()));
    // we always do the read exactly as requested, then
    // decide if we hit the eof after the read
    uint64_t esc = do_estale_cookie_(acfd, sb, full_path_);
    add_out_hdr(HHCOOKIE, std::to_string(esc));
    if( check_inm_(sb, esc) == 304 )
	return;
    // Ugh... There really should be a better way to put two things into the body...
    std::string vstr;
    if(req_->url_proto_minor_ >= 2)
        vstr = netstring(std::to_string(monotonic_validator(sb)));
    reserve_content_(vstr.size() + chunklen);
    char *data = const_cast<char*>(content_.data());
    memcpy(data, vstr.data(), vstr.size());
    auto nread = sew::pread(acfd, data + vstr.size(), chunklen, chunkfrom);
    resize_content(nread + vstr.size());
    do_common_headers_(&sb);
 }catch( std::exception& e){
    std::throw_with_nested(http_exception(400, fmt("do_file path_info=\"%s\" expected chunksizeKiB;offset;... and got: \"%s\"",
                                                            req_->path_info_.c_str(), req_->query_.c_str())));
 }


void ReplyPlus::do_link_() {
    // not worth the effort to do check_inm_ for symlinks?
    server_stats.l_requests++;
    std::string content;
    try{
        content = sew::str_readlink(full_path_.c_str());
    }catch(std::system_error& se){
        return common_report_errno_(se.code().value());
    }
    set_content_(content);
    do_common_headers_();
}

void ReplyPlus::do_statfs_(){
    struct statvfs svb;
    server_stats.v_requests++;
    sew::statvfs(per_selector_->basepath().c_str(), &svb);
    std::ostringstream oss;
    oss << svb << '\n';
    set_content_(oss.str());
    do_common_headers_();
}

void ReplyPlus::do_xattr_() try {
    uint64_t ckib;
    server_stats.x_requests++;
    if (req_->query_.empty())
	httpthrow(400, fmt("no query string for %s", req_->path_info_.c_str()));

    str_view svq(req_->query_);
    // Expect ckib;name;...
    // If name is non-empty, it's a getxattr request.
    // If name is empty, it's a listxattr request.
    auto first_semiidx = svscan(svq, &ckib);
    // The name extends to the next semicolon.  The name may not contain semicolon.
    if( first_semiidx >= svq.size() || svq[first_semiidx] != ';' )
        httpthrow(400, "no semicolon after ckib");
    auto second_semiidx = svq.find_first_of(";", first_semiidx+1);
    if( second_semiidx >= svq.size() || svq[second_semiidx] != ';' )
        httpthrow(400, "no semicolon after name");
    auto encnamelen = second_semiidx - (1 + first_semiidx);
    std::string encname(req_->query_.substr(first_semiidx+1, encnamelen));
    size_t namelen;
    auto name = std::unique_ptr<char, decltype(std::free)*>
		{evhttp_uridecode(encname.c_str(), 0, &namelen), std::free};
    if (!name)
	throw se(EINVAL, "failed to uridecode xattr name");
    auto chunklen = get_chunklen_(ckib);
    auto fname = full_path_.c_str();
    DIAGfkey(_http, "%llu do_xattr %jd %zd name \"%s\" %s\n",
	     reqid_, (intmax_t)ckib, chunklen, name.get(), fname);
    if (chunklen)
	reserve_content_(chunklen);
    ssize_t sz;
#ifndef __APPLE__
    if (name.get()[0] == '\0') {
	sz = llistxattr(fname, const_cast<char*>(content_.data()), chunklen);
    } else {
        sz = lgetxattr(fname, name.get(), const_cast<char*>(content_.data()), chunklen);
    }
#else
    if (name.get()[0] == '\0') {
	sz = listxattr(fname, const_cast<char*>(content_.data()), chunklen, XATTR_NOFOLLOW);
    } else {
        sz = getxattr(fname, name.get(),const_cast<char*>(content_.data()), chunklen, 0, XATTR_NOFOLLOW);
    }
#endif
    if (sz < 0)
	return common_report_errno_(errno);
    if (chunklen == 0)
	set_content_(std::to_string(sz)+'\n');
    else
	resize_content(sz);
    do_common_headers_();
}catch(std::exception& e){
    std::throw_with_nested(http_exception(400, fmt("do_xattr path_info=\"%s\" query=\"%s\"",
                                                            req_->path_info_.c_str(), req_->query_.c_str())));
 }
 
void ReplyPlus::do_numbers_() {
    std::ostringstream oss;
    server_stats.n_requests++;
    oss << server_stats;
    oss << "syslogs_per_hour: " << get_complaint_hourly_rate() << "\n";
    per_selector_->report_stats(oss);
    set_content_(oss.str());
    do_common_headers_();
}

void ReplyPlus::do_request() {
    auto c = req_->function_[0];
    if (c == '\0' || req_->function_[1] !='\0')
	httpthrow(400, fmt("function too long: %s", req_->function_.c_str()));
    server_stats.requests++;
    switch (c) {
	case 'a':
	    do_attr_();
	    break;
	case 'd':
	    do_dir_();
	    break;
	case 'f':
	    do_file_();
	    break;
	case 'l':
	    do_link_();
	    break;
	case 's':
	    do_statfs_();
	    break;
	case 'x':
	    do_xattr_();
	    break;
        case 'n': // statistics ... 's' was taken
            do_numbers_();
            break;
	default:
	    httpthrow(400, fmt("Unsupported function: /%s", req_->function_.c_str()));
    }
    // Add a few headers:
    if (status_ == 0) {
	throw std::runtime_error("do_request internal logic error: status 0 after request");
    } else if (status_ == 200) {
	// while content-type seems pointless for 0 size, libevent will otherwise default to
	// Content-Type: text/html; charset=ISO-8859-1
	add_out_hdr("Content-Type", content_type_);
        std::string content_encoding;
        // If there's no content, the arena might be empty.  Don't try to encode an empty
        // arena:
        if(content_arena_.empty())
            reserve_content_(0);
        DIAGfkey(_secretbox, "encode_content: content=%s\n", std::string(content_).c_str());
        std::tie(content_, content_encoding) = per_selector_->encode_content(*req_, secretid_, content_, content_arena_);
        if(!content_encoding.empty()){
            add_out_hdr("Content-encoding", content_encoding);
        }
        if(FLAGS_allow_unencrypted_replies){
            // As long as we allow unencrypted replies, we might
            // return different content to different clients,
            // according to the Accept-encoding header.  So it makes
            // sense to add a Vary header.  Note that
            // --allow-unencrypted-replies actually *defeats* any
            // security provided by encryption, so it should only be
            // used to transition from no-secretbox to with-secretbox
            // operation.  Thus, the Vary header will only appear
            // during such transitions.
            add_out_hdr("Vary", "Accept-encoding");
        }
        if(content_encoding != "fs123-secretbox" && content_.size() > 0){
            // If we're authenticating with secretbox threeroe(ciphertext) adds no value
            // and we definitely don't want to send threeroe(plaintext) because threeroe
            // is definitely not cryptographic.
            add_out_hdr(HHTRSUM, threeroe(content_).Final().hexdigest());
        }
	add_out_hdr("Content-Length", std::to_string(content_.size()));
    }
    //add_out_hdr("Status", std::to_string(status_));

    // And finally, if it is a GET, attach the content_.
    if(req_->method_ == EVHTTP_REQ_GET && content_.size() > 0){
	DIAGf(_http, "%llu content size %zu\n", reqid_, content_.size());
	DIAGf(_http >= 2, "%llu content \"\"\"%s\"\"\"\n", reqid_,
              quopri({content_.data(), content_.size()}).c_str());
	if(0 > evbuffer_add_reference(evhttp_request_get_output_buffer(req_->evreq_),
                                      content_.data(), content_.size(),
				      [](const void *, size_t, void *extra){
					  delete []((char *)extra);
				      }, content_up_.release())){
	    httpthrow(500, "evbuffer_add_reference failed");
	}
    }
}

} // namespace <anon>

int do_request(fs123Req* reqp, void *arg, unsigned long long reqid) try {
    if (reqp->url_proto_major_ != fs123_protocol_major)
	httpthrow(400, fmt("unsupported major protocol version %d.  This binary only supports major protocol %d",
				     reqp->url_proto_major_, fs123_protocol_major));
    if( reqp->url_proto_minor_ < fs123_protocol_minor_min || reqp->url_proto_minor_ > fs123_protocol_minor_max)
        httpthrow(400, fmt("unsupported minor protocol version %d.  This binary only supports minor protocol versions from %d through %d",
                                    reqp->url_proto_minor_, fs123_protocol_minor_min, fs123_protocol_minor_max));
    DIAGfkey(_http, "do_request(path_info=%s) %llu\n", reqp->path_info_.c_str(), reqid);
    ReplyPlus rs(reqp, static_cast<selector_manager*>(arg), reqid);
    if (_http >= 2) {
	// XXX ugly, requires knowledge of keyvalq, but no other iterator primitive in libevent 2.0.21
	auto headers = evhttp_request_get_input_headers(reqp->evreq_);
	for (auto header = headers->tqh_first; header; header = header->next.tqe_next) {
	    DIAG(true, reqid << " ihdr \"" << header->key << "\" : \"" <<  header->value << "\"\n");
	}
    }
    rs.do_request();
    if (_http >= 2) {
	// XXX ugly, requires knowledge of keyvalq, but no other iterator primitive in libevent 2.0.21
	auto headers = evhttp_request_get_output_headers(reqp->evreq_);
	for (auto header = headers->tqh_first; header; header = header->next.tqe_next) {
	    DIAG(true, reqid << " ohdr \"" << header->key << "\" : \"" <<  header->value << "\"\n");
	}
    }
    switch(rs.status_){
    case 200:
        server_stats.reply_200s++;
        break;
    case 304:
        server_stats.reply_304s++;
        break;
    default:
        server_stats.reply_others++;
    }
        
    return rs.status_;
 }catch(std::exception& e){
    server_stats.reply_others++;
    std::throw_with_nested(std::runtime_error(fmt("do_request(path=%s)", reqp->path_)));
 }

