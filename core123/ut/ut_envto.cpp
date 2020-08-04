#include <core123/envto.hpp>
#include <core123/ut.hpp>

int main(int, char **) {
    auto x = core123::envto<long>("WINDOWID", -1);
    if (x == -1) {
        std::cout << "no WINDOWID" << std::endl;
    } else {
        std::cout << "WINDOWID=" << x << std::endl;
    }
    const char *testname = "_UT_ENVTO_TEST_";
    const char *testval = "hello world";
    ::setenv(testname, testval, 1);
    auto xenv = core123::envto<std::string>(testname);
    EQSTR (xenv, testval);
    return utstatus();
}
