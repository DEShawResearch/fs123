#include <core123/streamutils.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/diag.hpp>
#include <core123/ut.hpp>
#include <vector>
#include <iostream>
#include <string>

using core123::str;
using core123::strbe;
using core123::strtuple;
using core123::str_sep;
using core123::ins;
using core123::insbe;
using core123::ins_sep;

struct nocopy{
    int i;
    nocopy(int _i) : i(_i){}
    nocopy(const nocopy&) = delete;
    nocopy& operator=(const nocopy&) = delete;
    nocopy(nocopy&&) = default;
    nocopy& operator=(nocopy&&) = default;
    friend std::ostream& operator<<(std::ostream& os, const  nocopy& nc){
        return os << nc.i;
    }
};

struct udt{
    int i;
    udt(int _i) : i(_i){}
    friend std::ostream& operator<<(std::ostream& os, const udt& u){
        return os << u.i;
    }
};

struct udt_i1{
    int i;
    udt_i1(int _i) : i(_i){}
};

// Illustrate how to write a 'core123::insertone' struct
// specialization for cases where it's impossible or undesirable to
// write a friend operator<<(std::ostream&, ...).
namespace core123{
template <>
struct insertone<udt_i1>{
    static std::ostream& ins(std::ostream& os, const udt_i1& u){
        return os << u.i;
    }
};
}
    
// This example intentionally mimics the structure of std::chrono::duration.
template <class Rep, class Period>
struct udt_tpl{
    Rep r;
    udt_tpl(Rep _r) : r(_r){}
};

namespace core123{
template <class Rep, class Period>
struct insertone<udt_tpl<Rep, Period>>{
    static std::ostream& ins(std::ostream& os, const udt_tpl<Rep, Period>& u){
        return os << u.r;
    }
};
}

int main(int, char **){
    EQSTR(str(1, 2, 3), "1 2 3");
    EQSTR(str_sep("", 1, 2, 3), "123");

    // Can we insert non-copyable objects:
    nocopy one(1), two(2), three(3);
    EQSTR(str(one, two, three, 4), "1 2 3 4");
    EQSTR(str_sep(", ", one, two, three, 4), "1, 2, 3, 4");

    // Can we insert user-defined types:
    udt u88(88);
    EQSTR(str(u88), "88");

    // How about user-defined types where we've defined 'insertone':
    udt_i1 u99(99);
    EQSTR(str(u99), "99");

    udt_tpl<int, std::string> u55(55);
    EQSTR(str(u55), "55");

    {
        std::ostringstream oss;
        oss << ins(std::chrono::microseconds(314));
        EQSTR(oss.str(), "0.000314000");
    }

    auto then = std::chrono::system_clock::now();
    auto now = std::chrono::system_clock::now();
    std::cout << ins(now, then, now-then) << "\n";

    std::vector<int> vi = {5, 6, 7};
    EQSTR(strbe(vi.begin(), vi.end()), "5 6 7");
    EQSTR(strbe(vi), "5 6 7");

    std::vector<nocopy> vnc;
    vnc.emplace_back(21);
    vnc.emplace_back(22);
    EQSTR(strbe(std::begin(vnc), std::end(vnc)), "21 22");
    EQSTR(strbe(vnc), "21 22");

    {
    std::ostringstream oss;
    oss << std::hex << insbe(vnc);
    EQSTR(oss.str(), "15 16");
    }

    // If you want control over formatting, then set the
    // stream into whatever state you want and use the
    // ins family:
    {
    std::ostringstream oss;
    oss << std::hex << std::showbase << insbe(", ", vnc);
    EQSTR(oss.str(), "0x15, 0x16");
    }

    {
    std::ostringstream oss;
    oss.precision(17);
    oss << ins_sep(", ", 2./3., 3./4., 4./5., 5./6.);
    EQSTR(oss.str(), "0.66666666666666663, 0.75, 0.80000000000000004, 0.83333333333333337");
    }        

    // Try a few pathological cases with nothing to insert
    EQSTR(str(),  "");
    EQSTR(str_sep("abc"), "");
    std::vector<int> empty;
    EQSTR(strbe(empty), "");
    EQSTR(strbe("sep", empty), "");

    // Test the code that defends  against (char*)0:
    EQSTR(str((const char*)0), "<(const char*)0>");
    EQSTR(str("foo", (char*)0, "bar"), "foo <(char*)0> bar");
    EQSTR(str("foo", (const char*)0, "bar"), "foo <(const char*)0> bar");
    // How (int*)0 is  rendered is library-specific.  It might
    // be "0" (libstdc++) or it might be "nil" (libc++)
    std::ostringstream oxx;
    oxx << (int *)0;
    EQSTR(str("foo", (int*)0, "bar"), "foo " + oxx.str() + " bar");

    return utstatus();
}
