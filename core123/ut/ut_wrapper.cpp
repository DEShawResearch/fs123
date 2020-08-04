#include "core123/sew.hpp"
#include "core123/autoclosers.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace sew = core123::sew;
namespace ac = core123::ac;
using std::system_error;

// Minimal exercising of xopen, xunlink and (automatic) close (via xfd_t).
// 

ac::fd_t<> openfile(const char *name, int flags, mode_t mode=0){
    return ::open(name, flags, mode);
}

void dosomething(){
    const char *name = "ut_wrapper.tmp";
    try{
        sew::unlink(name);
    }catch(system_error& xe){
        printf("caught %s.  This is not surprising\n", xe.what());
    }

    int fdr;
    int fdw;
    char bufin[1024];
    const char *buf = "The quick brown fox jumped over the lazy dog";
    {
        ac::fd_t<> xfdw = openfile(name, O_WRONLY|O_CREAT, 0555);
        ac::fd_t<> xfdr = openfile(name, O_RDONLY);
        // Save the file descriptors for later.  This is *NOT*
        // recommended usage.  We're doing this so that we can
        // check that they were really closed!
        fdr = xfdr;
        fdw = xfdw;
        auto nwrote = ::write(fdw, buf, sizeof(buf));
        auto nread = ::read(xfdr, bufin, sizeof(bufin));
        assert( nwrote == nread );
        assert( memcmp(bufin, buf, nread) == 0 );
        sew::unlink(name);
        // Let xfdw close itself.  We'll close xfdr ourselves.
        // N.B.  This failed through version 0.4!
        fdr = xfdr.release();
        sew::close(fdr);
    }
    try{
        sew::read(fdr, bufin, sizeof(bufin));
    }catch(system_error& xe){
        if(xe.code().value() != (int)std::errc::bad_file_descriptor)
            throw;
        printf("fdr was closed by xfd_t.  Reading from it correctly gives us a EBADF: %s\n", xe.what());
    }

    try{
        sew::write(fdw, buf, sizeof(bufin));
    }catch(system_error& xe){
        if(xe.code().value() != (int)std::errc::bad_file_descriptor)
            throw;
        printf("fdw was closed by xfd_t.  Writing to it correctly gives us a EBADF\n");
    }

    // check that getcwd doesn't print uninitialized data:
    try{
        char shortbuf[1];
        errno = 0;
        sew::getcwd(shortbuf, sizeof(shortbuf));
    }catch(system_error& xe){
        if(xe.code().value() != (int)std::errc::result_out_of_range)
            throw;
        printf("xgetcwd correctly complained about a too-small buffer: %s\n", xe.what());
    }

    // check that the NULLs and pointers in execl don't cause grief
    try{
        const char *env[3] = {"FOO=bar", "BAZ=bletch", 0};
        //xexecle("/usr/bin/printenv", "printenv", (const char*)0, env);
        sew::execle("/doesnotexist/bin/echo", "echo", "Hello", "world", (const char *)0, env);
    }catch(system_error& xe){
        if(xe.code().value() != (int)std::errc::no_such_file_or_directory)
            throw;
        printf("execle correctly complained about a non-existent executable: %s\n", xe.what());
    }

    try{
        char const * const av[] = {"echo", "hello", "world", 0};
        //xexecle("/usr/bin/printenv", "printenv", (const char*)0, env);
        sew::execv("/doesnotexist/bin/echo", const_cast<char * const *>(av));
    }catch(system_error& xe){
        if(xe.code().value() != (int)std::errc::no_such_file_or_directory)
            throw;
        printf("execve correctly complained about a non-existent executable: %s\n", xe.what());
    }
        

    // check that getenv doesn't say 'Success'
    try{
        ::unsetenv("FOO");
        sew::getenv("FOO");
    }catch(system_error& xe){
        if(xe.code().value() != ENOENT)
            throw;
        printf("getenv correctly complained with ENOENT\n");
    }

    FILE *fp = sew::tmpfile();
    fprintf(fp, "hello world\n");
    sew::fclose(fp);
}

int main(int, char**){
    dosomething();
    
    printf("OK\n");
    return 0;
}
