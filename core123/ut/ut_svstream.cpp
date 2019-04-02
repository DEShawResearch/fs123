#include "core123/svstream.hpp"

using core123::isvstream;
using core123::osvstream;

int FAIL = 0;
#define Assert(P) do{ if(!(P)){ std::cerr << "Assertion failed on line " << __LINE__ << ": "  #P << std::endl; FAIL++; } } while(0)

using namespace std;
int main(int, char **){
    isvstream isvs("10 hello world");
    auto rdb = isvs.rdbuf();
    cout << rdb->in_avail() << " " << char(rdb->sgetc()) << "\n";
    std::string w;
    int ten;
    isvs >> ten;
    isvs >> w;
    std::cout << ten << " " << w << "\n";
    Assert( ten == 10 );
    Assert( w == "hello" );
    Assert( isvs );

    core123::str_view s = "11 goodbye world";
    isvs.sv(s);
    isvs >> ten;
    isvs >> w;
    std::cout << ten << " " << w << "\n";
    Assert( ten == 11 );
    Assert( w == "goodbye" );
    Assert( isvs );

    isvstream isvs2("99 xyzzy", 8);
    int ninetynine;
    std::string xyzzy;
    isvs2 >> ninetynine >> xyzzy;
    Assert(ninetynine == 99);
    Assert(xyzzy == "xyzzy");

    osvstream osvs;
    osvs << ten << " " << w;
    osvs.seekp(0);
    osvs << 12;
    auto twelve_gb = osvs.sv();
    std::cout << twelve_gb << "\n";
    Assert( twelve_gb == "12 goodbye" );
    Assert( osvs );

    if(FAIL){
        std::cout << FAIL << " tests failed\n";
    }else{
        std::cout << "OK\n";
    }
    return !!FAIL;
}
