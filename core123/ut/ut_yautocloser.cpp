#include "core123/autoclosers.hpp"
#include "core123/sew.hpp"
#include "core123/throwutils.hpp"
#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace ac = core123::ac;
namespace sew = core123::sew;
using core123::se;
using core123::autocloser_t;
using core123::make_autocloser;
using core123::default_close_err_handler;

// Minimal exercising of xopen, xunlink and (automatic) xclose (via xfd_t).
// 

ac::fd_t<> openfile(const char *name, int flags, mode_t mode=0){
    return open(name, flags, mode);
}

void dosomething(){
    const char *name = "ut_wrapper.tmp";
    try{
        sew::unlink(name);
    }catch(std::system_error& xe){
        printf("caught:  %s.  This is not surprising\n", xe.what());
    }

    int fdr, fdw;
    char bufin[1024];
    const char *buf = "The quick brown fox jumped over the lazy dog";
    {
        auto xfdw = openfile(name, O_WRONLY|O_CREAT, 0777);
        auto xfdr = openfile(name, O_RDONLY);
        // Save the file descriptors for later.  This is *NOT*
        // recommended usage.  We're doing this so that we can
        // check that they were really closed!
        fdr = xfdr;
        fdw = xfdw;
        auto nwrote = sew::write(xfdw, buf, sizeof(buf));
        auto nread = sew::read(xfdr, bufin, sizeof(bufin));
        assert( nwrote == nread );
        assert( memcmp(bufin, buf, nread) == 0 );
        sew::unlink(name);
        // Let xfdw and xfdr them close themselves!
    }
    try{
        sew::read(fdr, bufin, sizeof(bufin));
    }catch(std::system_error& xe){
        if(xe.code().value() != (int)std::errc::bad_file_descriptor)
            throw;
        printf("fdr was closed by xfd_t.  Reading from it correctly gives us a EBADF\n");
    }

    try{
        sew::write(fdw, buf, sizeof(bufin));
    }catch(std::system_error& xe){
        if(xe.code().value() != (int)std::errc::bad_file_descriptor)
            throw;
        printf("fdw was closed by xfd_t.  Writing to it correctly gives us a EBADF\n");
    }
}

struct mydeleter{
    void operator()(int *p){
        std::cout << "mydeleter(" << p << ")\n";
        delete p;
    }
};

struct cranky_deleter{
    void operator()(int *p){
        std::cout << "cranky_deleter(*" << p  << "=" << *p << ")\n";
        if((*p) % 2)
            throw se(EINVAL, "cranky_deleter:  *p must be even");
        std::cout << "delete p = " << p << "\n";
        delete p;
    }
};

struct cranky_err_handler{
    void operator()(const std::exception& e){
        std::cout << "cranky_err_handler caught e.what() = " << e.what() << "\n";
    }
};

void deleters_and_error_handlers(){
    auto p = autocloser_t<int, cranky_deleter, cranky_err_handler>(new int(50));
    auto q= autocloser_t<int, cranky_deleter, cranky_err_handler>(new int(51));
}

int main(int, char**){
    dosomething();
    
    {
        ac::fd_t<> xfd;
        assert(sizeof(xfd) == sizeof(std::unique_ptr<int>));
        printf("int(unintialized xfd): %d\n", int(xfd)); // segfaulted in v0.3
        ac::fd_t<> yfd(std::move(xfd));
    }

    {
        ac::fd_t<> xfd = open(".", O_RDONLY);
        printf("int(non-empty xfd): %d\n", int(xfd));
        ac::DIR<> dir = ::fdopendir(std::move(xfd));
        printf("after being moved into ac::DIR:  %d\n", int(xfd));
    }

    {
        ac::fd_t<> xfd = open(".", O_RDONLY);
        printf("int(non-empty xfd): %d\n", int(xfd));
        ac::DIR<> dir = sew::fdopendir(std::move(xfd));
        printf("after being moved into ac::DIR:  %d\n", int(xfd));
    }
    
    ac::FILE<> xfp = sew::fopen("/dev/null", "r");
    std::cout <<  sizeof(xfp) << " " << sizeof(std::unique_ptr<FILE>) << "\n";
    assert(sizeof(xfp) == sizeof(FILE*)); // No space overhead!!
    // ac::FILE yfp = xfp; // compile-time error - copy-constructor is deleted
    ac::FILE<> yfp = std::move(xfp); // transfer of ownership, xfp will be empty
    assert(!xfp);
    ac::FILE<> zfp;
    assert( xfp == nullptr );
    assert( xfp == zfp );
    assert( xfp == 0);
    assert( yfp != nullptr );
    //sew::fclose(yfp); // compile-time error
    sew::fclose(std::move(yfp));
    assert( !yfp );

    deleters_and_error_handlers();
    
    // Check that we can construct our own deleter:
    auto xfoo = autocloser_t<int, mydeleter, default_close_err_handler>(new int(99));
    // And that make_autocloser works With a function:
    std::cout << sizeof(xfoo) <<  " " << sizeof(std::unique_ptr<int>) << "\n";
    auto yfoo = make_autocloser(new int(88), [](int*p){ 
            std::cout << "yfoo lambda deleter(" << p << ")\n";
            delete p;
        });

    return 0;
}
