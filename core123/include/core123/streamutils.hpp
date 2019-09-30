#pragma once

/* DOCUMENTATION_BEGIN

  Easy-to-use string formatting tools.  The emphasis is on ease of use
  and a succinct API.  It should be *really* easy to add some
  variables to an ostream or to join them into a string.  These tools
  DO NOT provide detailed control over formatting (precisions, widths,
  traits, facets, etc.).

  inserters:  ins, insbe, instuple,
     Meant to be used as the right-hand side of an ostream operator<<
     insertion operator.  E.g., 

     os << ins(a, b, 3, 3.1415, some_udt);
     os << "v: " << insbe(begin(v), end(v));
     os << "[" << insbe(", ", v) << "]";
     os << "(" << instuple(", ", t) << ")"; 

  See also the analogous 'str' functions in strutils.hpp

  They can work with:

    variadic argument lists and argpacks: ins, ins_sep

    tuples: instuple

    ranges and collections: insbe

  Note that the variadic functions can work with variadic argpacks
  forwarded from other variadic functions.

  The 'be' and 'tuple' families are overloaded, so if they are
  called with a const char* first argument,  then that  argument
  is treated as a separator and is inserted between the other
  arguments in the output.

  The variadic functions can't disambiguate a const char* argument
  from an argument intended for insertion, so they have  _sep variants
  that always take a separator as their first argument.

  Also in this file:

  stream_{flags,precision,width}_saver : RAII objects inspired by
    boost that restore a stream's characteristics when they go out of
    scope.  Note that there are other stream characteristics that are
    a little trickier to save, because they're templated on the
    stream's CharT: iostate, exception, tie, rdbuf, fill, locale, and
    user-defined state.  We'll get to them one day.

  ostream_size : A function that returns the size of an ostream (e.g.,
    a stringstream) without having to allocate space for and copy 
    bytes into its 'str()' method.

DOCUMENTATION_END*/      

#include <tuple>
#include <ostream>
#include <memory>
#include <sstream>
#include <iterator>
#include <type_traits>
#include <limits>
#include <cstdio>
#include <cstdarg>

// N.B. we make no effort to do 'perfect forwarding' here.  Is that
// a problem?

namespace core123{

// ostream_size - returns the 'pubseekoff' of the 'end' of the
// stream's underlying rdbuf.  For stringstreams, this is the length
// of the string you'd get by calling os.str() - but *without having
// to construct or copy the string*.  For file streams, it's the
// length of the underlying file (or -1 if the underlying file is not
// seekable).  For other streams (are there other streams?), it's
// whatever is returned by pubseekoff(0, os.end, os.out).
//
// It works by temporarily modifying the underlying rdbuf.  It would
// be permissible by const-rules to do this even on a const ostream,
// but it would be thread-unsafe, so the stream argument is a
// non-const reference to emphasize to the caller that (temporary)
// modifications are happening.
inline auto ostream_size(std::ostream& os){
    std::streambuf* buf = os.rdbuf();
    if(!buf)
        throw std::ios_base::failure("ostream_size:  stream argument has a null rdbuf()");
    auto orig = buf->pubseekoff(0, os.cur, os.out);
    auto end = buf->pubseekoff(0, os.end, os.out);
    buf->pubseekpos(orig, os.out);
    return end;
}

class stream_precision_saver{
    ::std::ios_base& stream;
    ::std::streamsize const saved;
public:
    stream_precision_saver(::std::ios_base& stream_):
        stream(stream_), saved(stream_.precision())
    {}
    stream_precision_saver(::std::ios_base& stream_, ::std::streamsize newprec_):
        stream(stream_), saved(stream_.precision(newprec_))
    {}
    // Non-copyable
    stream_precision_saver(const stream_precision_saver&) = delete;
    stream_precision_saver& operator=(const stream_precision_saver&) = delete;
    ~stream_precision_saver(){
        stream.precision(saved);
    }
};

class stream_width_saver{
    ::std::ios_base& stream;
    ::std::streamsize const saved;
public:
    stream_width_saver(::std::ios_base& stream_):
        stream(stream_), saved(stream_.width())
    {}
    stream_width_saver(::std::ios_base& stream_, ::std::streamsize newwidth_):
        stream(stream_), saved(stream_.width(newwidth_))
    {}
    // Non-copyable
    stream_width_saver(const stream_width_saver&) = delete;
    stream_width_saver& operator=(const stream_width_saver&) = delete;
    ~stream_width_saver(){
        stream.width(saved);
    }
};

class stream_flags_saver{
    ::std::ios_base& stream;
    ::std::ios_base::fmtflags const saved;
public:
    stream_flags_saver(::std::ios_base& stream_):
        stream(stream_), saved(stream_.flags())
    {}
    stream_flags_saver(::std::ios_base& stream_, ::std::ios_base::fmtflags newflags_):
        stream(stream_), saved(stream_.flags(newflags_))
    {}
    // Non-copyable
    stream_flags_saver(const stream_flags_saver&) = delete;
    stream_flags_saver& operator=(const stream_flags_saver&) = delete;
    ~stream_flags_saver(){
        stream.flags(saved);
    }
};
    
// The insertone struct is in the public core123:: namespace so that
// callers can define their own specializations for types for which it
// is  impossible or undesirable to add a stream inserter.
template <typename T>
struct insertone{
    static std::ostream& ins(std::ostream& os, T const& t){
        return os << t;
    }
};

// If you have a user-defined type (udt), and you want it to
// work with 'ins' and 'str', but it's impractical to give it
// an ostream inserter, this should work:
//
// struct udt;
// 
// namespace core123{
// template <>
// struct insertone<udt>{
//     static std::ostream& ins(std::ostream& os, const udt& u){
//         return os << ...; 
//     }
// };
// }
//
// Or, if udt_tpl is a template:
//
// template <class Foo, class Bar>
// struct udt_tpl;
//
// namespace core123{
// template <class Foo, class Bar>
// struct insertone<udt_tpl<Foo, Bar>>{
//     static std::ostream& ins(std::ostream& os, const udt_tpl<Foo, Bar>& u){
//         return os << u.r;
//     }
// };
// }

// It's not uncommon to get a (char*)0 passed into insertN through an
// argpack.  Inserting it with operator<<, would be undefined
// behavior. Insertone inserts "<(char*)0>" or "<(const char*)0>"
// instead.  For anything other than char*, there's no problem because
// operator<< only dereferences the pointer when it's a char*.  Any
// other pointer gets converted to void* and is formatted as if by %p.
template <>
struct insertone<char*>{
    static std::ostream& ins(std::ostream& os, char * const& t){
        return os << ((t==0)?"<(char*)0>": t);
    }
};


template <>
struct insertone<char const *>{
    static std::ostream& ins(std::ostream& os, char const * const& t){
        return os << ((t==0)?"<(const char*)0>": t);
    }
};

// N.B.  One could do write out some fancy arithmetic that
// inevitably boils down to 9, 17 and 21 for the number of
// roundtrip digits required for floating point types...
template<>
struct insertone<float>{
    static std::ostream& ins(std::ostream& os, float t){
        stream_precision_saver raii(os, std::numeric_limits<float>::max_digits10); // 9
        return os << t;
    }
};

template<>
struct insertone<double>{
    static std::ostream& ins(std::ostream& os, double t){
        stream_precision_saver raii(os, std::numeric_limits<double>::max_digits10); // 17
        return os << t;
    }
};

template<>
struct insertone<long double>{
    static std::ostream& ins(std::ostream& os, long double t){
        stream_precision_saver raii(os, std::numeric_limits<long double>::max_digits10); // 21
        return os << t;
    }
};


namespace detail{

// insertN - insert the first N members of a tuple into an output stream
template <unsigned N>
struct insertN{
    template <typename ... T>
    static std::ostream& out(std::ostream& o, const std::tuple<T...>& t, const char*sep){
        return insertone<typename std::remove_cv_t<std::remove_reference_t<decltype(std::get<N-1>(t))>>>::ins(insertN<N-1>().out(o, t, sep) << sep, std::get<N-1>(t));
    }
};

// insertN - specialize for N=0
template <>
struct insertN<0u>{
    template <typename ... T>
    static std::ostream& out(std::ostream& o, const std::tuple<T...>&, const char*){
        return o;
    }
};

// insertN - specialize for N=1
template <>
struct insertN<1u>{
    template <typename ... T>
    static std::ostream& out(std::ostream& o, const std::tuple<T...>& t, const char*){
        return insertone<typename  std::remove_cv_t<std::remove_reference_t<decltype(std::get<0>(t))>>>::ins(o, std::get<0>(t));
    }
};

template <typename ...T>
struct inserter{
    std::tuple<T const&...> args;
    const char *sep;

    inserter(std::tuple<T const&...> args_, const char* sep_) :
        args(args_), sep(sep_){}

    friend std::ostream& operator<<(std::ostream& os, const inserter& t){
        std::ostream::sentry sentry(os);
        if(sentry)
            insertN<sizeof...(T)>::out(os, t.args, t.sep);
        return os;
    }
};

template  <typename ITER>
class rangeInserter {
    ITER b;
    ITER e;
    const char *sep;
public:
    rangeInserter(ITER _b, ITER _e, const char* _sep) : b(_b), e(_e), sep(_sep){}
    friend std::ostream& operator<<(std::ostream& s, const rangeInserter&r){
        std::ostream::sentry sentry(s);
        if(sentry){
            // copy(r.b, r.e, std::ostream_joiner(s, r.sep)); C++20 ?
            const char *maybe_sep = "";
            for(ITER p=r.b; p!=r.e; ++p){
                s << maybe_sep << *p;
                maybe_sep = r.sep;
            }
        }
        return s;
    }
};


} // namespace detail


template <typename ... Types>
detail::inserter<Types...>
ins_sep(const char *sep, Types const& ... values) 
{
    return {std::tie(values...), sep};
}

template <typename ... Types>
auto
ins(Types const& ... values) 
{
    return ins_sep(" ", values ...);
}

template <typename ... Types>
detail::inserter<Types...>
instuple(const char  *sep, const std::tuple<Types...>& t){
    // Why did we think we needed this?  Portability between
    // libstdc++ (gnu) and libc++ (llvm) is *much* easier if
    // we can do without std::experimental::apply from <experimental/tuple>.
    //return {std::experimental::apply(std::tie<typename std::add_const<Types>::type ...>, t), sep};
    // "Seems to work" with clang5 and gcc6 (and later):
    return {t, sep};
}

template <typename ... Types>
auto
instuple(const std::tuple<Types...>& t){
    return instuple(" ", t);
}

template <typename ITER>
detail::rangeInserter<ITER> 
insbe(const char *sep, ITER b, ITER e){
    return detail::rangeInserter<ITER>(b, e, sep);
}

template <typename ITER>
auto
insbe(ITER b, ITER e){
    return insbe(" ", b, e);
}

template <typename COLL>
auto
insbe(const char *sep, const COLL& coll){
    return insbe(sep, std::begin(coll), std::end(coll));
}

template <typename COLL>
auto
insbe(const COLL& coll){
    return insbe(" ", coll);
}

} // namespace core123
