#pragma once
#include <utility>

// Machinery to allow perfect forwarding of arguments to a lambda.

// See https://vittorioromeo.info/index/blog/capturing_perfectly_forwarded_objects_in_lambdas.html

// FWD is boilerplate that makes using std::forward a lot less wordy.
#define FWD(x) std::forward<decltype(x)>(x)

// fwd_capture's arguments are perfectly forwarded to a tuple, which
// may, in turn be captured by a C++14-style initialized lambda
// capture.  See the example in periodic.hpp
template <typename ... Ts>
auto fwd_capture(Ts&&... xs){
    return std::tuple<Ts...>(FWD(xs)...);
}

