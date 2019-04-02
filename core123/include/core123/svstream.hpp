#pragma once

#include "str_view.hpp"
#include <iostream>
#include <sstream>

// DOCUMENTATION_BEGIN
// svstream.hpp - input (isvstream) and output (osvstream) streams
//  that leverage the new str_view type
//  to reduce the number of data allocations and copies required
//  to use C++ streams for in-memory formatted I/O.

// isvstream, wisvstream, basic_isvstream - fully functional istreams,
// analogous to istringstream, et al, but the underlying streambuf is
// a non-owning basic_isvbuf (see below).  The constructor takes a
// string_view argument or a pointer and length instead of a string
// argument.  There are no 'str()' methods, but the 'sv()' methods
// take and return string_view (and ptr,length) instead of string.
// The input sequence (i.e., the string_view argument to the
// constructor or sv()) is neither owned nor copied, so it must remain
// valid through the lifetime of the isvstream object.  For example:
//
//    char *p; 
//    size_t len;
//    // something sets ptr and len
//    isvstream isvs1(p, len);
//    int i, j;
//    std::string word;
//    isvs1 >> i >> j >> word;
//
// The automatic conversion from string to string_view allows:
//    std::string s("1 2 hello");
//    isvstream isvs2(s);  // No copy!!
//    isvs2 >> i >> j >> word;
//
// And the automatic conversion from char* to string_view allows:
//    const char *p = "3 4 hello";
//    isvstream isvs3(p);
//    isvs3 >> i >> j >> word;
//
// The advantage over a plain istringstream is that formatted
// extraction, as in the examples above, can can be done without
// allocating an internal string or copying from the original buffer
// or string.
//
// 
// osvstream, wosvstream, basic_osvstream - derived from stringstream,
// with the addition of an 'sv' method that returns a non-owning
// string_view of the current 'output sequence'.  This might save an
// allocate/copy in some circumstances, but considerable care is
// required.  Beware that string_view::data() is not automatically
// NUL-terminated.  E.g.,
//
//   if( problem ){
//      osvstream osvs;
//      osvs << "Houston, we have a problem with " << foo << " and " << bar << '\0';
//      syslog("%s", osvs.sv().data());
//   }
// 
//
//
// isvbuf, wisvbuf, u16isvbuf, u32isvbuf, basic_isvbuf - analogous
// stringbuf et al, but the underlying control-sequence is a
// non-owning string_view.  These are used internally by isvstream et
// al, but can be used separately.  Instead of a str(string)) method
// there's a sv(string_view) method.
// DOCUMENTATION_END

//  ---------------------------------
//
// Initial inspiration from:
//   http://stackoverflow.com/questions/2079912/simpler-way-to-create-a-c-memorystream-from-char-size-t-without-copying-t
//
// You've got something that you can do no-copy operator>> on with <5
// lines of code.  Cool!  Then you want to do tellg() and you find you
// have to implement seekoff.  Then you feel a compulsive need to
// implement analogs for the rest of the methods in stringbuf and in
// istringstream, and you add the whole template <CharT,Traits>
// basic_xxx rigamarole and before you know it you've got 100 lines of
// code :-(.
namespace core123{
template <class CharT, class Traits = std::char_traits<CharT> >
struct basic_isvbuf : public std::basic_streambuf<CharT, Traits>{
    // N.B.  stringbuf constructors take a 'which' argument, but
    // since we're not prepared to do anything with it, don't bother.
    basic_isvbuf(){
        sv(basic_str_view<CharT, Traits>());
    }
    basic_isvbuf(basic_str_view<CharT, Traits> _sv){
        sv(_sv);
    }
    // Note that implicit conversions from string -> string_view and
    // from char* -> string_view mean that one can call the constructor
    // and the sv(arg) method with string or CharT* arguments.
    basic_isvbuf(const CharT *p, size_t len){
        sv(p, len);
    }
        
    // const  sv() no-argument - return the sequence
    basic_str_view<CharT, Traits> sv() const{
        return {this->eback(), size_t(this->egptr()-this->eback())};
    }

    // non-const sv(argument) - replace the control sequence
    void sv( basic_str_view<CharT, Traits> _sv ){
        sv(_sv.data(), _sv.size());
    }        
    void sv(const CharT *cp, size_t len){
        CharT *p = const_cast<CharT*>(cp);
        this->setg(p, p, p + len);
    }
    
    void swap(basic_isvbuf & rhs){
        // forward to the protected member of the parent class
        std::basic_streambuf<CharT,  Traits>::swap(rhs);
    }
protected:
    std::streampos seekoff(std::streamoff off, std::ios_base::seekdir way,
                           std::ios_base::openmode which) override{
        if(which != std::ios_base::in)
            return -1;
        if(way == std::ios_base::beg){
            off -= this->gptr() - this->eback();
        }else if(way == std::ios_base::end){
            off += this->egptr() - this->gptr();
        }
        this->gbump(off);
        return this->gptr() - this->eback();
    }
    std::streampos seekpos(std::streampos sp, std::ios_base::openmode which) override{
        return seekoff(sp, std::ios_base::beg, which);
    }
    // do we need any of the other virtuals?  setbuf, sync, showmanyc,
    // xsgetn, underflow, uflow, pbackfail?  I think they all have sane
    // defaults.
};

using isvbuf = basic_isvbuf<char>;
using wisvbuf = basic_isvbuf<wchar_t>;
using u16isvbuf = basic_isvbuf<char16_t>;
using u32isvbuf = basic_isvbuf<char32_t>;

template <class CharT, class Traits = std::char_traits<CharT> >
class basic_isvstream : public std::basic_istream<CharT, Traits>{
    basic_isvbuf<CharT, Traits> _rdbuf;
public:
    // these types are defined for basic_stringstream, so we might
    // as well define them for isvstream.
    using char_type = CharT;
    using traits_type = Traits;
    using int_type = typename Traits::int_type;
    using pos_type = typename Traits::pos_type;
    using off_type = typename Traits::off_type;

    // N.B.  istringstream  constructors take an openmode argument.  It's
    // not clear what we'd do with it, so we don't bother.
    explicit basic_isvstream() :
        std::basic_istream<CharT, Traits>(&_rdbuf),
        _rdbuf(basic_isvbuf<CharT, Traits>())
    {}
    explicit basic_isvstream(basic_str_view<CharT, Traits> sv) :
        std::basic_istream<CharT, Traits>(&_rdbuf),
        _rdbuf(sv)
    {}
    explicit basic_isvstream( const CharT* p, size_t len) :
        std::basic_istream<CharT, Traits>(&_rdbuf),
        _rdbuf(basic_str_view<CharT, Traits>(p, len))
    {}
    explicit basic_isvstream (basic_isvstream&& other) :
        std::basic_istream<CharT,  Traits>(std::move(other)),
        _rdbuf(std::move(other._rdbuf))
    {}
    basic_isvstream& operator=( basic_isvstream&& other ){
        std::basic_istream<CharT, Traits>::operator=(std::move(other));
        _rdbuf = std::move(other._rdbuf);
        return *this;
    }
    void swap( basic_isvstream& other ){
        std::basic_istream<CharT, Traits>::swap(other);
        rdbuf()->swap(*other.rdbuf());
    }
    // N.B.  stringstream has stringbuf* rdbuf() const; I.e., the
    // const method returns a non-const pointer.  That requires a
    // const_cast, which seems wrong, so here we have separate const
    // and non-const methods.  In practice, there is very littly you
    // can do with a 'const istream', so I doubt this will ever
    // matter.
    basic_isvbuf<CharT, Traits> const * rdbuf() const{
        return &_rdbuf;
    }
    basic_isvbuf<CharT, Traits> * rdbuf(){
        return &_rdbuf;
    }
    basic_str_view<CharT, Traits> sv() const{
        return rdbuf()->sv();
    }
    void sv(basic_str_view<CharT, Traits> new_str){
        rdbuf()->sv(new_str);
    }
    void sv(CharT *p, size_t len){
        rdbuf()->sv(p, len);
    }
};

using isvstream = basic_isvstream<char>;
using wisvstream = basic_isvstream<wchar_t>;

} // namespace core123

namespace std{
template <class CharT, class Traits = std::char_traits<CharT> >
void swap(core123::basic_isvstream<CharT, Traits>& lhs, core123::basic_isvstream<CharT, Traits> &rhs){
    lhs.swap(rhs);
}
}


// To implement osvstream::sv(), all we need to do is call the pbase
// member functions of the osvstream's rdbuf.  Don't let the fact that
// it's protected stop us.  It's possible to call X's protected
// member functions by inheriting from X and calling them with
// pointer-to-member function syntax!  See:
//
//  http://stackoverflow.com/questions/23900499/how-to-call-protected-member-function-in-base-on-instance-of-base-from-within
//  http://stackoverflow.com/questions/9907328/may-pointer-to-members-circumvent-the-access-level-of-a-member
//
// The consensus opinion seems to be that this is abusive and
// discouraged, but perfectly legal and well-defined.  A Defect Report
// (DR) was discussed, but I can't find any resolution.
//   https://groups.google.com/forum/#!topic/comp.std.c++/5NZ7duc72sE
namespace core123{
template <class CharT, class Traits = std::char_traits<CharT> >
struct basic_osvstream : public std::basic_ostringstream<CharT, Traits>, private std::basic_stringbuf<CharT, Traits>  {
    basic_str_view<CharT, Traits> sv(){
        auto placeholder = this->tellp();
        size_t len = this->seekp(0, std::ios_base::end).tellp();
        this->seekp(placeholder);
        return {(this->rdbuf()->*&basic_osvstream::pbase)(), len};
    }
};

using osvstream = basic_osvstream<char>;
using wosvstream = basic_osvstream<wchar_t>;

} // namespace core123
