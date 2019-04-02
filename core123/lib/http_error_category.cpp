#include "core123/http_error_category.hpp"

http_error_category_t& http_error_category(){
    static http_error_category_t the_http_category;
    return the_http_category;
}

