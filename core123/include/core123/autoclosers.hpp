#pragma once

// Quickstart guide:
//
// Instead of:
//     int fd = open(blah, blah blah);
//     ... WORRY about exceptions
//     close(fd);
//
// Say:
//     #include <core123/autoclosers.hpp>
//     #include <core123/sew.hpp>
//     using namespace core123;
//     ...
//     ac::fd_t<> fd = sew::open(blah, blah, blah);
//     ... DON'T WORRY about execptions
//
// fd will be closed when it goes out of scope.  In fact, you don't
// even have to call close unless you really care about close's return
// status.  Just let fd go out of scope and fd will be closed
// automatically!
//
//  Similar constructions for FILE*, DIR* and PIPEFILE*,  E.g.,
//
//     ac::FILE<> fp = sew::fopen(...);
//     ac::PIPEFILE<> pfp = sew::popen(...);
//     ac::DIR<> dp = sew::opendir(...);
//
// Detailed TL;DR description:
//
// All names are in namespace 'core123::ac'.
//
// autocloser_t<T, D, E> is basically a unique_ptr with two additional
// member functions:
//
//   - operator pointer() - equivalent to get().  Allows the
//                          autocloser_t to be used almost anywhere a
//                          T* can be used, but introduces the
//                          possibility of ambiguity.
//
//   - close(pointer) - calls the deleter.  The deleter may throw, in
//                      which case any exceptions thrown by the
//                      deleter are propagated to the caller.
//                  
// It's intended as an RAII wrapper around things like FILE*, DIR*,
// file descriptors and other things that need to be close'ed. 
//
// autocloser_t's T and D template params are the same as unique_ptr,
// but unlike a unique_ptr, its deleter *may* throw.  The
// autocloser_t's destructor catches exceptions thrown by the deleter
// and forwards them to an instance of the error handler, E.  The
// default error handler writes a message to stderr, but a user-provided
// error handler can behave differently.
// 
// There are four classes in the core123::ac namespace:
//   fd_t, FILE, PIPEFILE, and DIR
// all of which are specializations of autocloser_t.
//
// The following text describes an ac::fd_t, but exactly the same
// considerations apply to FILE, DIR or any other autocloser_t.
//
// The idea is that you assign the file descriptor returned by ::open
// to an fd_t and then you let the runtime manage closing the file
// descriptor when the fd_t is deleted.  It has an operator int()
// conversion method, so passing an fd_t to a function that expects an
// int (e.g., read or sew::read) works transparently and as expected.
//
// For example,
//
//     ac::fd_t<> acfd = ::open("/tmp/file", O_RDONLY);
//     // use acfd as if it were a file-descriptor, e.g.,
//     ::read(acfd, buf, cnt);
//
// One can, of course, use the wrapped system calls in the
// system_error_wrapper namespace, e.g., system_error_wrapper::open
// and system_error_wrapper::read instead of ::open and ::read.
//
// The managed file descriptor will be closed when acfd is destroyed,
// e.g., when it goes out-of-scope.  It is simplest to then completely
// ignore any question of calling ::close yourself.  Just let acfd go
// out of scope and let the runtime call ::close automatically.  In
// particular, if acfd's scope is exited because of an exception, the
// managed file descriptor will be closed as part of the normal
// stack-unwinding.  This makes autoclosers particularly useful (even
// essential) in combination with the system_error_wrapper functions.
//
// fd_t has a template parameter that specifies a
// "close_error_handler" class.  Any exception thrown by fd_t's
// deleter *when called by fd_t's destructor*, is caught and forwarded
// to an instance of the close_error_handler, E as if by:
//
//     catch(std::exception& e){
//         E()(e);
//     }
//
// The default close-error-handler (i.e., the one you get with
// ac::fd_t<>) inserts a line including e.what() into
// std::cerr.  It's hard to do much more.
//
// You can declare an empty fd_t, i.e., one without a managed file
// descriptor, and then assign to it.  Assigning to an fd_t causes its
// managed descriptor (if one exists) to be closed before it assumes
// management of the new one, so this code never has more than one
// file descriptor open:
//
//     ac::fd_t<> acfd;
//     for( int i=1; i<argc; ++i){
//         acfd = ::open(argv[i], O_RDONLY);
//         ...
//      }
//  
// autocloser_t is derived from std::unique_ptr, so all of
// unique_ptr's methods should work: release(), reset(), get(), etc.
// fd_t has "ownership transfer" semantics, analogous to
// std::unique_ptr.  Thus, it has a 'move constructor' and a 'move
// assignment operator', but it does not have a copy constructor or a
// copy assignment operator.  You can transfer ownership by returning
// from a function, or by using std::move:
//
//     using namespace sew=system_error_wrapper;
//     ac::fd_t<> function(){
//       ac::fd_t<> acfd1 = sew::open("/tmp/foo", O_RDONLY);
//       ac::fd_t<> acfd2 = acfd1; // ERROR!  Assignment is forbidden
//       ac::fd_t<> acfd3 = std::move(acfd1); // OK, acfd3 is the owner.
//       return acfd3; // OK
//     }
//     ac::fd_t acfd4 = function(); // OK, acfd4 is now the owner
//
// Note that 'release' sets the managed object to nullptr (or -1, in
// the case of an fd_t), after which it can't be dereferenced.  So if
// you want something to be conditionally managed, you can't use
// release.  Instead:
//
//     ac::fd_t<> acfd;
//     int fd;
//     if( name == "-"){
//        fd = 0;
//     }else{
//        acfd = fd = sew::open(name, O_RDONLY);
//     }
//     // use fd until acfd goes out of scope
//
// with the result that stdin (fd=0) will not be closed by the deletion of acfd.
//
// Finally, this header file provides special handling for ::fdopen
// and ::fdopendir, *in the global namespace* , because they must do a
// handoff between two "competing" managers, so they can't just rely
// on the automatic promotion of ac::fd_t to int.
//
// The semantics of the ac::fd_t overloads are as follows:

// if a non-NULL DIR* or FILE* is returned, then the returned DIR* or
//   FILE* "owns" the underlying fd.  It will not be closed when the
//   ac::fd_t argument goes out of scope.  It's the responsibility of
//   the caller to call fclose() or closedir() or close() which it
//   *might* choose to do by assigning the returned value to a scoped
//   ac::DIR or an ac::FILE.

// otherwise (if NULL is returned), the underlying fd will be closed
//   before the function returns.
//
// In all cases, after the function returns, the caller has no
// responsibility for the ac::fd_t argument and the caller assumes
// "normal" responsibility for the returned DIR* or FILE*.
//
// N.B.  similar overloads in the system_error_wrapper namespace (in
// sew.hpp) throw rather than return NULL on error,
// but have similar ownership semantics.
//
// Future directions:  mmap/munmap/mremap present a unique
//  set of challenges.  It would be nice to provide an auto-munmapper,
//  but munmap requires a length arg in addition to a pointer.
//  So it's not sufficient to say:
//     typedef xmmaped_ptr_t xautocloser_t<void, xmunmap>;
//  We need something more tightly integrated with mmap and mremap.
//  See xmmap.hpp in the gardenfs module for inspiration.
//
//  One thing you can do with a unique_ptr is "promote" it
//  to shared_ptr by transfer of ownership to a shared_ptr.
//  Can we do the same with our xFOO_t-ypes?

#include "core123/throwutils.hpp"
#include "core123/strutils.hpp"
#include <exception>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <cstdio>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

namespace core123 {

// This is the default value of the CEH template parameter for the
// autoclosing classes.  It is called when from the autocloser's
// destructor when the close operation fails.  Since it's called from
// a destructor, it may not throw.  Autoclosers can be instantiated
// with a non-default handler.  A non-default handler must be a
// nothrow functor taking a const std::exception& argument and
// returning void.  Alternatives might 'complain', and/or 'abort'.
struct default_close_err_handler{
    void operator()(const std::exception& e) noexcept{
        std::cerr << "core123::~autocloser_t threw an exception: " << e.what() << "\n";
    }
};

template<typename T, typename D, typename E>
class autocloser_t : public std::unique_ptr<T, D>{
    typedef typename std::unique_ptr<T, D> uP;
public :
    typedef typename uP::pointer pointer;
    autocloser_t() : uP() {}
    autocloser_t(pointer v) : uP(v) {}
    autocloser_t(pointer v, D d) : uP(v, d) {}
    operator pointer() const  { return this->get(); }
    ~autocloser_t(){
        try{
            close();
        }catch(std::exception& e){
            E()(e);
        }catch(...){
            E()(std::runtime_error("close threw something not matched by std::exception&"));
        }
    }
    // Since we've defined a destructor, we have to explicitly state
    // our intention to use the default move-constructor and
    // move-assignment operators.
    autocloser_t(autocloser_t&& rhs) = default;
    autocloser_t& operator=(autocloser_t&& rhs) = default;
                
    void close() { 
        pointer ptr = this->release();
        if(ptr != nullptr)
            this->get_deleter()(ptr);
        // N.B.  if the deleter throws, we've still released the uP.
        // That seems bad, but it's hard to imagine any less bad
        // options...
    }
};

namespace detail{
// N.B.  Don't call sew::xxclose because we want autoclosers.hpp
// to be independent of sew.hpp.
struct fclose_may_throw{
    void operator()(::FILE* p){ auto fd{fileno(p)}; if(::fclose(p)) throw se(strfunargs("ac::fclose", p) + " fileno(p)=" + str(fd)); }
};

struct pclose_may_throw{
    void operator()(::FILE* p){ auto fd{fileno(p)}; if(::pclose(p)) throw se(strfunargs("ac::pclose", p) + " fileno(p)=" + str(fd)); }
};

struct closedir_may_throw{
    void operator()(::DIR* p){ auto fd{dirfd(p)}; if(::closedir(p)) throw se(strfunargs("ac::closedir", p) + " dirfd(p)=" + str(fd)); }
};

template <typename T, typename Closer>
struct deleter{
    Closer f;
    deleter(Closer f_) : f(f_) {}
    void operator()(T* p){
        f(p);
    }
};

struct fdcloser{
    void operator()(const int* fdp) { 
        int fd = *fdp;
        delete fdp;
        if(::close(fd))
            throw se(fmt("autocloser::detail::fdcloser(%d)",  fd));
    }
};
}  // namespace detail
    
// autoclosers for FILE* and DIR* are easy
namespace ac{
template <typename CEH = default_close_err_handler>
using FILE = autocloser_t<::FILE, detail::fclose_may_throw, CEH>;
template <typename CEH = default_close_err_handler>
using DIR = autocloser_t<::DIR, detail::closedir_may_throw, CEH>;
template <typename CEH = default_close_err_handler>
using PIPEFILE = autocloser_t<::FILE, detail::pclose_may_throw, CEH>;

// The autocloser for an fd is slightly trickier.  Unique_ptr *really*
// wants a pointer.  For example, it had better be the case that:
//
//     lvalue-reference pointer::operator*(rvalue)
//
// really returns a bona fide lvalue-reference that outlives the
// rvalue argument.  Rather than try to cobble something together that
// satisfies this (and perhaps other) subtle requirements, we just
// call 'new' to get a pointer before handing off the heavy lifting to
// autocloser_t.  Our deleter deletes the pointer and then calls the
// close(fd).  We provide void reset(int fd=-1) and int release()
// member functions analogous to unique_ptr.

template <typename CEH = default_close_err_handler>
class fd_t{
    // N.B.  all methods are noexcept.  Is there any value in saying so?
    static int deref(const int* p) { return p ? *p : -1; }
    static const int* newptr(int fd) { return fd>=0 ? new const int(fd) : nullptr; }
    autocloser_t<const int, detail::fdcloser, CEH> aci;
public:
    fd_t(int fd=-1) : aci(newptr(fd)){}
    void reset(int fd=-1){
        aci.reset(newptr(fd));
    }
    int get() const { return deref(aci.get()); }
    operator int() const{ return get(); }
    operator bool() const{ return bool(aci); }
    int release(){
        // release the base aci and delete the pointer it managed.
        // Return (but do not close) the file descriptor associated
        // with it.
        const int *p = aci.release(); // release is noexcept!
        int ret = deref(p);
        delete p;
        return ret;
    }
    void close() { aci.close(); }
};
} // namespace ac

// make_autocloser and associated machinery:  succinctly creating an
// autocloser for a C-style "handle":
//
// The templated function make_autocloser returns an autoclosing pointer
// to a C-style "handle".  E.g.,
//
//    using namespace ac=core123;
//
//    auto raii = make_autocloser(fuse_lowelevel_new(...), ::fuse_session_destroy);
//
// Notice that the type inferences for the make_autocloser functions
// are all automatic.  Also, note that autocloser_t avoids calling
// Closer(nullptr), so there's no need to worry about whether that's
// safe.

// write:  make_autocloser(new_foo(), foo_delete
template <typename T, typename Closer>
auto make_autocloser(T* ptr, Closer closer)
    -> autocloser_t<T, detail::deleter<T, Closer>, default_close_err_handler>
{
    return {ptr, closer};
}
} // namespace core123

// Now for some overloads in the global namespace!
template <typename CEH>
DIR *fdopendir(core123::ac::fd_t<CEH> fd){
    auto ret = ::fdopendir(int(fd));
    if(ret)
        fd.release();
    return ret;
}

template <typename CEH>
inline FILE *fdopen(core123::ac::fd_t<CEH> fd, const char *mode){
    auto ret = ::fdopen(int(fd), mode);
    if(ret)
        fd.release();
    return ret;
}

template <typename CEH>
inline void close(core123::ac::fd_t<CEH> fd){
    fd.close();
}

template <typename CEH>
inline void pclose(core123::ac::PIPEFILE<CEH> fp){
    fp.close();
}

template <typename CEH>
inline void fclose(core123::ac::FILE<CEH> fp){
    fp.close();
}
