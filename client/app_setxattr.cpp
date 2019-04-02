// fs123setxattr:  A program that sets the extended attribute:
//
//    user.fs123.estalecookie
//
// to the time of day for every file named on the command line, which
// does not already have a value for that extended attribute.  Since
// existing attributes are unchanged, it is safe to call fs12setxattr
// without first checking whether the attribute already exists.
//
// If the --force option is given, then the attribute is reset to the
// current time of day, even if it was already set.
//
// The --delete option removes the attribute from all named files.
//
// With the --mtime option, whenever a file's inode is changed by a
// successful call to setxattr or removexattr, the file's mtime will
// be updated to the current time of day by utimes(name, NULL).  Note
// that without --force, setxattr is unsuccessful on files with a
// pre-existing attribute and all metadata for such files (mtime,
// ctime, xattrs) will be completely undisturbed.
//
// Note that fs123setxattr fails on files that are not writable to it.
// However, it is recommended that it be installed with the capability:
//    cap_dac_override=pe
//
// giving it effective and permitted CAP_DAC_OVERRIDE capability
// which allows modify the attributes of unwritable files
// without resorting to chmod (which would change the ctime, even if
// the attributes weren't changed).
//
// All errors are reported to stderr, but errors on earlier files do
// not stop processing of later files.  The exit status is zero if and
// only if no errors were encountered.
//


#include <core123/sew.hpp>
#include <core123/exnest.hpp>
#include <core123/throwutils.hpp>
#include <cstring>
#include <string>
#include <chrono>
    
using namespace core123;
    
namespace {
const char *progname = "fs123setxattr";
bool opt_force = false;
bool opt_verbose = false;
bool opt_delete = false;
bool opt_mtime = false;
int nbad = 0;

void do_one(const char *fname) try{
    static const char *attrname = "user.fs123.estalecookie";
    bool inode_changed = false;
    // There doesn't seem to be any point in doing open followed by fsetxattr.
    // It appears that you need write-permission on the file, regardless.
    if(opt_delete){
        if(opt_verbose)
            fprintf(stderr, "removexattr(%s, %s)\n", fname, attrname);
#ifndef __APPLE__
        sew::removexattr(fname, attrname);
#else
        sew::removexattr(fname, attrname, 0);
#endif
        inode_changed = true;
    }else{
        // now is decimal nanoseconds since the epoch
        using namespace std::chrono;
        std::string now = std::to_string(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
        if(now.size() > 31) // see bufsz in exportd/do_request.cpp
            throw std::runtime_error("time-of-day too long (must be <=31 characters): " + now);
        // flags=0 means set unconditionally.  flags=XATTR_CREATE means
        // fail with EEXIST if the attribute exists.
        int flags = opt_force ? 0 : XATTR_CREATE;
        if(opt_verbose)
            fprintf(stderr, "setxattr(%s, %s, %s, flags=%d)\n",
                    fname, attrname, now.c_str(), flags);
#ifndef __APPLE__
        auto ret = lsetxattr(fname, attrname, now.c_str(), now.size(), flags);
#else
        auto ret = setxattr(fname, attrname, now.c_str(), now.size(), 0, flags|XATTR_NOFOLLOW);
#endif
        if(ret < 0){
            if( opt_force || errno != EEXIST )
                throw se(errno, "lsetxattr(" + str_sep(", ", fname, attrname, now) + ")");
        }
        inode_changed = (ret==0);
    }
    if(opt_mtime && inode_changed){
        // If we forced a change to the xattrs,  then also update
        // the atime and mtime, which might have helped us when
        // we added xattrs the first time...
        sew::utimes(fname, nullptr);
    }        
 }catch(std::exception& e){
    // report and count exceptions as we see them, but don't let them escape.
    nbad++;
    for(auto& a : exnest(e))
        fprintf(stderr, "%s: %s\n", progname, a.what());
 }
} // namespace <anonymous>

int app_setxattr(int , char**  argv){
    // would getopt be easier??
    const char *arg;
    while((arg=*++argv) && *arg=='-'){
        if(strcmp(arg, "-h")==0 || strcmp(arg, "--help")==0 ){
            fprintf(stderr, "Usage: %s: [-h|--help][-f|--force][-v|--verbose][-d|--delete][-m|--mtime][--] [filename ...]\n", progname);
            return 0;
        }
        if(strcmp(arg, "-f")==0 || strcmp(arg, "--force")==0 ){
            opt_force = true;
        }
        if(strcmp(arg, "-v")==0 || strcmp(arg, "--verbose")==0 ){
            opt_verbose = true;
        }
        if(strcmp(arg, "-d")==0 || strcmp(arg, "--delete")==0 ){
            opt_delete = true;
        }
        if(strcmp(arg, "-m")==0 || strcmp(arg, "--mtime")==0 ){
            opt_mtime = true;
        }
        if(strcmp(arg, "--")==0) // end of options.
            break;
    }

    while(arg){
        do_one(arg);
        arg = *++argv;
    }
    return nbad > 0;
}
