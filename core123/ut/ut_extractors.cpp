#include "core123/const_ntbsExtractor.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <cassert>

using namespace std;

int main(int argc, char **argv){
    double x;
    int y;
    string s1, s2;

    istringstream iss("x: 3.14 y:19 s:          t: preceded_by_ws s2:nowhitespace");

    iss >> "x: " >> x >> " y:" >> y >> " s: t:" >> s1 >> " s2: " >> s2;
    assert(x == 3.14);
    assert(y==19);
    assert(s1 == "preceded_by_ws");
    assert(s2 == "nowhitespace");

    iss.clear();
    iss.str("x: 3.14 y:19 s:          t: preceded_by_ws s2:nowhitespace");
    iss >> "x: " >> x >> " y:" >> y >> " s: t:" >> s1 >> " s2: " >> s2;
    assert(x == 3.14);
    assert(y==19);
    assert(s1 == "preceded_by_ws");
    assert(s2 == "nowhitespace");


    cout << "OK\n";
    return 0;
}
