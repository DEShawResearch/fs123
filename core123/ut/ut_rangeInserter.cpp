#include "core123/streamutils.hpp"
#include <sstream>
#include <iostream>
#include <vector>
#include <cassert>

using namespace std;
using core123::insbe;

int main(int, char **){
    int numbers[] = {11, 37, 19};

    ostringstream oss;
    oss << "Some numbers: " << insbe(numbers, numbers+3);
    cout << oss.str() << "\n";
    assert( oss.str() == "Some numbers: 11 37 19" );
    
    oss.str("");
    oss << insbe(" x ", numbers, numbers+3);
    cout << oss.str() << "\n";
    assert( oss.str() == "11 x 37 x 19");

    vector<int> v(numbers, numbers+3);
    oss.str("");
    oss << insbe("\n", v.begin(), v.end());
    cout << oss.str() << "\n";
    assert( oss.str() == "11\n37\n19");

    return 0;
}
