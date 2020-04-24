#include <core123/http_error_category.hpp>
#include <iostream>
#include <stdexcept>

int main(int, char **){
    try{
        httpthrow(404, "these are not the droids you're looking for");
    }catch(std::exception& e){
        std::cout << e.what() << '\n';
    }
    return 0;
}
