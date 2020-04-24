#include "special_ino.hpp"
#include "app_mount.hpp"
#include "fuseful.hpp"
#include "backend123.hpp"
#include <core123/scoped_nanotimer.hpp>
#include <core123/diag.hpp>
#include <core123/sew.hpp>
#include <core123/throwutils.hpp>
#include <cstring>

// FIXME - these don't belong here.  Maybe beget.[ch]pp ?
#include "backend123.hpp"
reply123 begetserver_stats(fuse_ino_t ino); // in mount.fs123p7.cpp

using namespace core123;

auto _special = diag_name("special");

struct stat shared_getattr_special_ino(fuse_ino_t ino, struct fuse_file_info *fi){
    struct stat sb{};
    DIAGfkey(_special, "shared_getattr_special(ino=%lu, fi=%p, fi->fh=%jx)", ino, fi, (intmax_t)(fi?fi->fh:0L));
    switch(ino){
    case SPECIAL_INO_STATS:
    case SPECIAL_INO_CONFIG:
    case SPECIAL_INO_SERVER_STATS:
        sb.st_ino = ino;
        sb.st_nlink = 1;
        sb.st_mode = S_IFREG | 0444;
        if(fi && fi->fh){
            std::string* sp = reinterpret_cast<std::string*>(fi->fh);
            sb.st_size = sp->size();
        }
        return sb;
    case SPECIAL_INO_IOCTL:
        sb.st_ino = ino;
        sb.st_nlink = 1;
        sb.st_mode = S_IFREG | 0400;
        sb.st_uid = sew::geteuid();
        sb.st_gid = sew::getegid();
        if(fi && fi->fh){
            std::string* sp = reinterpret_cast<std::string*>(fi->fh);
            sb.st_size = sp->size();
        }
        return sb;
    default:
        throw se(EPROTO, fmt("shared_getattr_special:  unspecial ino: %lu", ino));
    }
}

// return true if we've fully handled the request.  false otherwise.
bool lookup_special_ino(fuse_req_t req, fuse_ino_t pino, const char *name){
    if(pino == 1){
        fuse_ino_t cino = 0;
        if(strcmp(name, special_ino_to_fullname(SPECIAL_INO_STATS)) == 0){
            DIAGfkey(_special, "lookup_special_ino(1, %s)\n", special_ino_to_fullname(SPECIAL_INO_STATS));
            cino = SPECIAL_INO_STATS;
        }else if(strcmp(name, special_ino_to_fullname(SPECIAL_INO_IOCTL)) == 0){
            DIAGfkey(_special, "lookup_special_ino(1, %s)\n", special_ino_to_fullname(SPECIAL_INO_IOCTL));
            cino = SPECIAL_INO_IOCTL;
        }else if(strcmp(name, special_ino_to_fullname(SPECIAL_INO_CONFIG)) == 0){
            DIAGfkey(_special, "lookup_special_ino(1, %s)\n", special_ino_to_fullname(SPECIAL_INO_CONFIG));
            cino = SPECIAL_INO_CONFIG;
        }else if(strcmp(name, special_ino_to_fullname(SPECIAL_INO_SERVER_STATS)) == 0){
            DIAGfkey(_special, "lookup_special_ino(1, %s)\n", special_ino_to_fullname(SPECIAL_INO_SERVER_STATS));
            cino = SPECIAL_INO_SERVER_STATS;
        }
        if(cino != 0){
            auto sb = shared_getattr_special_ino(cino, nullptr);
            struct fuse_entry_param e{};
            e.ino = cino;
            e.attr = sb;
            e.attr_timeout = 0.;
            e.entry_timeout = 1.e3;
            DIAGfkey(_special, "returning from lookup_special_ino\n");
            reply_entry(req, &e);
            return true;
        }
    }
    return false;
 }

void getattr_special_ino(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi){
    struct stat sb = shared_getattr_special_ino(ino, fi);
    reply_attr(req, &sb, 0.);
}

void open_special_ino(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){
    DIAGfkey(_special, "open_special_ino(ino=%lu, fi=%p, fi->fh=%ju)\n", ino, fi, (intmax_t)(fi?fi->fh:0L));
    struct timeval now;
    gettimeofday(&now, nullptr);
    char nowtm_buf[128];
    struct tm nowtm;
    if(nullptr == ::gmtime_r(&now.tv_sec, &nowtm) ||
       0 == ::strftime(nowtm_buf, sizeof(nowtm_buf), "utc: %Y-%m-%dT%H:%M:%SZ\n", &nowtm))
        nowtm_buf[0] = '\0';
    std::ostringstream oss;
    oss << std::boolalpha;
    oss << nowtm_buf << fmt("epoch: %lu.%06lu\n", now.tv_sec, (long)now.tv_usec);
    switch(ino){
    case SPECIAL_INO_STATS:
        // /.statistics
        // Dynamically generate statistics information in human-readable form.
        report_stats(oss);
        DIAGkey(_special, "open_special_ino: " << oss.str() << "\n");
        fi->fh = reinterpret_cast<decltype(fi->fh)>(new std::string(oss.str()));
        fi->keep_cache = 0;
        return reply_open(req, fi);
    case SPECIAL_INO_CONFIG:
        oss << "git_description: " << GIT_DESCRIPTION << "\n";
        report_config(oss);
        fi->fh = reinterpret_cast<decltype(fi->fh)>(new std::string(oss.str()));
        fi->keep_cache = 0;
        return reply_open(req, fi);
    case SPECIAL_INO_IOCTL:
        // .config_ioctl - nothing to do for open.
        return reply_open(req, fi);
    case SPECIAL_INO_SERVER_STATS:
        DIAGfkey(_special, "open_special_ino calling begetserver_stats(%llu)\n", (long long unsigned)ino);
        reply123 sstats = begetserver_stats(ino);
        DIAGfkey(_special, "sstats.content: %s\n", sstats.content.c_str());
        if(sstats.eno)
            return reply_err(req, sstats.eno);
        fi->fh = reinterpret_cast<decltype(fi->fh)>(new std::string(std::move(sstats.content)));
        // sstats.content is in a valid but unspecified state!
        return reply_open(req, fi);
    }
    throw se(EINVAL, fmt("open_special_ino: can't handle ino: %lu", ino));
}

void read_special_ino(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi){
    switch(ino){
    case SPECIAL_INO_STATS:
    case SPECIAL_INO_CONFIG:
    case SPECIAL_INO_SERVER_STATS:
        {
        if(!(fi && fi->fh))
            throw se(EIO, "read_special_ino:  expected non-NUL fi && fi->fh");
        // recover the contents from fi and send it out.
        const std::string* sp = reinterpret_cast<const std::string*>(fi->fh);
        DIAGfkey(_special, "read_special_ino(ino=%lu, size=%zd, off_t=%jd, sp=%p\n",
                 ino, size, (intmax_t)off, sp);
        DIAGfkey(_special, "%zd: %s\n", sp->size(), sp->data());
        if(off<0)
            throw se(EINVAL, "read_special_ino with negative offset?");
        // Return 0 bytes (but not an error) if the offset is past the
        // end.
        size_t soff = std::min(size_t(off), sp->size());
        return reply_buf(req, soff+sp->data(), sp->size() - soff);
        }
    case SPECIAL_INO_IOCTL:
        return reply_buf(req, nullptr, 0);
    }
    throw se(EINVAL, "read_special_ino - expected low-numbererd 'special' ino");
}

void release_special_ino(fuse_req_t req, fuse_ino_t /*ino*/, struct fuse_file_info* fi){
    if(fi->fh)
        delete reinterpret_cast<std::string*>(fi->fh);
    return reply_release(req);
}

void forget_special_ino(fuse_ino_t /*ino*/, uint64_t /*nlookup*/){
}
