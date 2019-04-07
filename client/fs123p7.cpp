// Main client program
// Invokes various apps.

#include "diskcache.hpp"
#include "fs123/acfd.hpp"
#include "fs123/sharedkeydir.hpp"
#include "fs123/content_codec.hpp"
#include <iostream>
#include <core123/strutils.hpp>
#include <core123/pathutils.hpp>
#include <core123/diag.hpp>
#include <core123/sew.hpp>
#include <core123/exnest.hpp>
#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif

using namespace std;
using namespace core123;

namespace {
auto _init = diag_name("init");

const string fs123p7pfx = "fs123p7";
const string mountprog = "mount." + fs123p7pfx;
const string usage = "Usage: "+fs123p7pfx+" OP OPARGS\n       where OP is one of mount, cachedump, ctl, flushfile, secretbox or setxattr";

// Just enough of a capabilities "API" to allow us to acquire
// CAP_DAC_OVERRIDE in setxattr and drop all capabilities
// everywhere else.  Error checking is non-existent, but
// sufficient for the use case:  if we fail to manipulate our
// capabilities, we just carry on and hope for the best.
#ifdef HAVE_LIBCAP
void acquire_cap(cap_value_t capability){
    DIAG(_init,  "fs123p7:acquire_cap(" << capability << ")\n");
    auto caps = cap_get_proc();
    cap_set_flag(caps, CAP_EFFECTIVE, 1, &capability, CAP_SET);
    cap_set_proc(caps);
    cap_free(caps);
}

void drop_caps(){
    DIAG(_init, "drop_caps - all capabilities dropped\n");
    auto caps = cap_init(); // empty - no capabilities
    cap_set_proc(caps);
    cap_free(caps);
}
#endif

int app_cachedump(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        string url;
        auto r = diskcache::deserialize_no_unlink(-1, argv[i], &url);
        cout << argv[i] << " : errno     " << r.eno << "\n";
        cout << argv[i] << " : expires   " << str(r.expires) << "\n";
        cout << argv[i] << " : etag64    " << r.etag64 << "\n";
        cout << argv[i] << " : lastrefr  " << str(r.last_refresh) << "\n";
        cout << argv[i] << " : swr       " << str(r.stale_while_revalidate) << "\n";
        cout << argv[i] << " : escookie  " << r.estale_cookie << "\n";
        cout << argv[i] << " : cnextoff  " << r.chunk_next_offset << "\n";
        cout << argv[i] << " : cnextmet  " << r.chunk_next_meta << "\n";
        cout << argv[i] << " : encoding  " << r.content_encoding << "\n";
        cout << argv[i] << " : trsum     " << insbe("", r.content_threeroe, r.content_threeroe+sizeof(r.content_threeroe)) << "\n";
        cout << argv[i] << " : datalen   " << r.content.size() << "\n";
        cout << argv[i] << " : url       " << url << "\n";
        cout << endl;
    }
    return 0;
}

int app_flushfile(int argc, char **argv) {
    for(int i=1; i<argc; ++i){
        acfd fd = sew::open(argv[i], O_RDONLY);
#ifdef POSIX_FADV_DONTNEED
        sew::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    }
    return 0;
}

int app_secretbox(int argc , char **argv) {
    if(argc != 2){
        cerr << "Usage:  " << argv[0] << " secretfile < encoded > decoded\n";
        exit(1);
    }
    sharedkeydir sm(sew::open(argv[1], O_DIRECTORY), "encoding", 120);
    char buf[sizeof(fs123_secretbox_header)];
    auto nread = sew::read(0, buf, sizeof(buf));
    fs123_secretbox_header hdr(str_view(buf, nread));
    
    uint32_t recordsz = ntohl(hdr.recordsz_nbo);
    string in(hdr.wiresize()+recordsz, 0);
    ::memcpy(&in[0], buf, nread);
    if(recordsz > hdr.wiresize() + nread)
        sew::read(0, &in[0]+nread, recordsz - hdr.wiresize() - nread);
    auto ret = content_codec::decode(content_codec::CE_FS123_SECRETBOX, in, sm);
    sew::write(1, ret.data(), ret.size());
    return 0;
}

} // namespace anon

extern int app_mount(int argc, char **argv);
extern int app_ctl(int argc, char **argv);
extern int app_setxattr(int argc, char **argv);

int main(int argc, char *argv[]) try {
    char **args = argv;
    auto op = pathsplit(argv[0]).second;
    if (op == mountprog) {
        op = "mount";
    }else if (argc > 1) {
        op = argv[1];
        args++;
        argc--;
    } else {
        cerr << usage << endl;
        exit(1);
    }

    DIAGf(_init, "op \"%s\"\n", op.c_str());

#ifdef HAVE_LIBCAP
    // Capabilities.  If we're already running as root, do nothing -
    // assume the caller and or admins know what they're doing.
    // Otherwise, assume we're installed with one or more 'permitted'
    // but not (necessarily) 'effective' capabilities, which we either
    // acquire or drop depending on which command we're running.  Note
    // that there are no errors reported.  If the request fails (e.g.,
    // we weren't installed with 'permitted' capabilities as assumed),
    // we just carry on and hope for the best.
    if(sew::geteuid() != 0){
        if(op == "setxattr")
            acquire_cap(CAP_DAC_OVERRIDE);
        else
            drop_caps();
    }
#endif

    if (op == "mount")
        return app_mount(argc, args);
    else if (op == "ctl") 
        return app_ctl(argc, args);
    else if (op == "secretbox")
        return app_secretbox(argc, args);
    else if (op == "setxattr")
        return app_setxattr(argc, args);
    else if (op == "flushfile")
        return app_flushfile(argc, args);
    else if (op == "cachedump")
        return app_cachedump(argc, args);
    else {
        cerr << usage << endl;
        exit(1);
    }
 } catch (exception& e) {
    for(auto& a : exnest(e)){
        fprintf(stderr, "%s: nested exception caught in main: %s\n", argv[0], a.what());
    }
    exit(2);
 }
