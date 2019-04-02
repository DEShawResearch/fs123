#include "core123/streamutils.hpp"
#include <cassert>
#include <sstream>
#include <iostream>

using core123::ins;
using core123::ins_sep;
using core123::instuple;

// g - try vaInserter_sep
template <typename ... Args>
void g(std::ostream& os, Args ... args){
    os << "g(" << ins_sep(" : ", args...) << ")";
}

// h - try vaInserter, with rvalue reference args
template <typename ... Args>
void h(std::ostream& os, Args&& ... args){
    os << "h(" << ins(args...) << ")";
}

struct noncopyable{
    noncopyable(const noncopyable&) = delete;
    noncopyable() = default;
    friend std::ostream& operator<<(std::ostream& os, const noncopyable& nc){
        return os << "<noncopyable>";
    }
};

union u{
    int i;
    long int li;
};
inline std::ostream& operator<<(std::ostream& os, const u& v){
    return os << v.i;
}

void reset(std::ostringstream& oss){
    oss.str(std::string()); oss.clear();
}

int main(int argc, char **argv){
    int x;
    std::ostringstream oss;
    oss << &x;
    auto xaddr = oss.str();
    noncopyable nc;

    reset(oss);
    auto t = std::make_tuple(1, "two", 3.125);
    oss << instuple(t);
    std::cout << oss.str() << "\n";
    assert( oss.str() == "1 two 3.125");

    reset(oss);
    std::tuple<int, noncopyable> tnc;
    oss << instuple(tnc);
    std::cout << oss.str() << "\n";

    reset(oss);
    g(oss, 4, "goodbye world", &x);
    std::cout << oss.str() << "\n";
    assert( oss.str() == "g(4 : goodbye world : " + xaddr + ")" );
    
    reset(oss);
    h(oss, 4, "with spaces", &x);
    std::cout << oss.str() << "\n";
    assert( oss.str() == "h(4 with spaces " + xaddr + ")" );
    
    reset(oss);
    oss << ins(4, nc);
    std::cout << oss.str() << "\n";
    assert( oss.str() == "4 <noncopyable>");

    reset(oss);
    u uv;
    uv.i = 11;
    x = 19;
    auto txuv = std::make_tuple(x, uv);
    oss << instuple(txuv);
    std::cout << oss.str() << "\n";
    assert(oss.str() =="19 11");

    std::cout << "OK\n";
    return 0;
}
