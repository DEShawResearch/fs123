#pragma once

// Core123 wrappers Francois-Xavier Bourlet's backward-cpp library,
// backward.hpp (MIT License)

// 1 - A convenience function that creates a backward::StackTrace from the
// current location:

//    stacktrace_from_here(size_t depth=100);

// 2 - A specialization of core123::insertone<backward::StackTrace>.
// This automatically enables the core123 'ins' and 'str' functions
// for backward::StackTrace.  So if you want a nicely formatted
// backtrace, you just say:

//    std::cerr << ins(stacktrace_here()) << "\n";
// or
//    std::string tb = str(stacktrace_here());

// 3 - stacktrace_error is a runtime_error whose 'what' message contains a
// stack trace at the point at which the object was created.
//
//   if(problem)
//       throw stacktrace_error("Uh oh!")
//
// The what() message looks reasonable when printed with 'complain'.
//
// It also works nicely with nested exceptions:
//
//   try{...}catch(std::exception& e){
//       ...
//       if(things_look_especially_confusing)
//           std::throw_with_nested(stacktrace_error("That's very weird"));
//       ...
//   }
//
// To associate a stack trace with a specific kind of error (which you
// might intend to match a catch expression higher up the stack), use
// throw_nest from exnest.hpp:
//
//   if(problem)
//        throw_nest(specific_error("foo"), stacktrace_error("Backtrace:"));
//
// N.B.  pre-defined macros provide a lot of flexibility to backward.
// The defaults produce a pretty lean backtrace.  To get a more
// informative backtrace, try one of:
//
// CPPFLAGS+=-DBACKWARD_HAS_DW=1    LDLIBS+=-ldw
// CPPFLAGS+=-DBACKWARD_HAS_BFD=1   LDLIBS+=-lbfd
// CPPFLAGS+=-DBACKWARD_HAS_DWARF=1 LDLIBS+=-ldwarf
// On some platforms (e.g., CentOS6) you may also need -ldl.

#include <core123/backward.hpp>
#include <core123/strutils.hpp>
#include <core123/streamutils.hpp>
#include <sstream>
#include <memory>
#include <stdexcept>

namespace core123{

backward::StackTrace stacktrace_from_here(size_t depth = 100){
    backward::StackTrace st;
    st.load_here(depth);
    return st;
}

// See the comments in streamutils.hpp.  This is how to enable
// 'core123::str' and 'core123::ins' for backward::StackTrace.
template <>
struct insertone<backward::StackTrace>{
    static std::ostream& ins(std::ostream& os, const backward::StackTrace& st){
        backward::TraceResolver tr;
        tr.load_stacktrace(st);
        os << "Backtrace:\n";
        for (size_t i = 0; i < st.size(); ++i) {
            backward::ResolvedTrace trace = tr.resolve(st[i]);
            trace_oneline(i, trace.source, trace, os);
            for (const auto& v : trace.inliners)
                trace_oneline(i, v, trace, os);
        }
        return os;
    }
    // trace() and trace_oneline could conceivably be used outside a
    // stacktrace_error if we somehow find ourselves with a
    // backward::StackTrace and/or a
    // backward::ResolvedTrace::SourceLoc on our hands.  They could
    // even be free functions...
    static std::ostream&
    trace_oneline(size_t i, const backward::ResolvedTrace::SourceLoc& src,
                  const backward::ResolvedTrace& trace, std::ostream& os) {
        backward::SnippetFactory sf;
        auto fname = src.filename;
        std::string srcline;
        if (fname.empty()) {
            fname = "?("+trace.object_filename+")";
        } else {
            auto p = sf.get_snippet(fname, src.line, 1);
            if (p.size()) srcline = p[0].second;
        }
        auto f = src.function;
        if (f.find("backward::StackTrace") != std::string::npos ||
            core123::endswith(fname, "stacktrace.hpp")) // i.e., basename(__FILE__)
            return os;
        os << "  #" << i << " " << fname << ':' << src.line << ':'
           << src.col << ':';
        if (!src.function.empty()) {
            os << src.function;
        } else {
            os << trace.object_function << "[" << trace.addr << "]";
        }
        os << ':' << srcline << '\n';
        return os;
    }
};

struct stacktrace_error : public std::runtime_error{
    // std::runtime_error has several "interesting" properties:
    //   - it is no-throw copy-able
    //   - the value returned by what() is guaranteed to persist until the object is destroyed
    //      or a non-const method is called.
    //   - its what() method is const
    //   - its what() method is noexcept
    //   - its constructors are explicit, but they are *not* noexcept.
    //   - there is no default-constructor.
    //
    // We do our best to give stacktrace_error the same properties.
    //
    // All this would be "easy" if we were willing to construct the
    // what string in the constructor.  But for a backtrace, constructing
    // the what() string may be very laborious, and we might not even
    // want it.  So we defer its construction until what() is called.
    // That means jumping through a few hoops...

    // We modify these members in what(), but since what() is const,
    // we have to make them mutable.  To guarantee that copying is
    // nothrow, we wrap them with shared_ptr.
    mutable std::shared_ptr<std::string> msg_;
    mutable std::shared_ptr<backward::StackTrace> st_ = std::make_shared<backward::StackTrace>();

    // constructors are explicit and requre a 'whatarg', just like runtime_error.
    // There is no default constructor, just like runtime_error.
    // We've added an optional 'depth' arg which tells the underlying 'backward'
    // library how deep a stack trace to construct.
    explicit stacktrace_error(const std::string& whatarg, size_t depth=100) : stacktrace_error{whatarg.c_str(), depth}{}
    explicit stacktrace_error(const char *whatarg, size_t depth=100) : std::runtime_error(whatarg) {
        st_->load_here(depth);
    }

    virtual const char* what() const noexcept try {
        if(!msg_){
            const char *what = std::runtime_error::what();
            if(*what)
                msg_ = std::make_shared<std::string>(what + std::string(": ") + str(*st_));
            else
                msg_ = std::make_shared<std::string>(str(*st_));
        }
        return msg_->c_str();
    }catch(...){
        // Seriously?  Constructing the backtrace threw?  Maybe we'd
        // be better off ignoring this and getting a core file here?
        // The std::runtime_error's what() shouldn't throw...
        return std::runtime_error::what();
    }

};

} // namespace core123
