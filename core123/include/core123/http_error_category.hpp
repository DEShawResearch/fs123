#pragma once 
// Some boilerplate that allows us to use our system_error machinery
// with http errors.
//
// Usage:
//
//  httpthrow(404, "not here now.  Did you spell the name correctly");
//
//   ...
//  }catch(std::system_error& se){
//     if(se.code().category() == http_error_category()){
//        ... it's an http error ...
//     }
//  }

#include <system_error>
#include <string>

enum class http_errc { success=0, client_error,  server_error, other };
namespace std{
template<> struct is_error_condition_enum<http_errc> : public true_type{};
}

struct http_error_category_t : public std::error_category{
    virtual const char *name() const noexcept { return "http"; }
    virtual std::error_condition default_error_condition(int ev) const noexcept {
        if((ev>=200)&&(ev<300)) return std::error_condition(http_errc::success);
        else if((ev>=400)&&(ev<500)) return std::error_condition(http_errc::client_error);
        else if((ev>=500)&&(ev<600)) return std::error_condition(http_errc::server_error);
        else return std::error_condition(http_errc::other);
    }

    virtual bool equivalent(const std::error_code& code, int condition) const noexcept{
        return *this==code.category() &&
            static_cast<int>(default_error_condition(code.value()).value()) == condition;
    }

    virtual bool equivalent(int code, const std::error_condition& condition) const noexcept{
        return default_error_condition(code) == condition;
    }
    
    virtual std::string message(int ev) const{
        switch(ev){
        case 200: return "OK";
        case 400: return "400 Bad Request";
        case 402: return "402 Payment Required";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 406: return "406 Not Acceptable";
        case 407: return "407 Proxy Authentication Required";
        case 408: return "408 Request Timeout";
        case 409: return "409 Conflict";
        case 410: return "410 Gone";
        case 411: return "411 Length Required";
        case 412: return "412 Precondition Failed";
        case 413: return "413 Request Entity Too Large";
        case 414: return "414 Request-URI Too Long";
        case 415: return "415 Unsupported Media Type";
        case 416: return "416 Requested Range Not Satisfiable";
        case 417: return "417 Expectation Failed";
        case 500: return "500 Internal Service Error";
        case 501: return "501 Not Implemented";
        case 502: return "502 Bad Gateway";
        case 503: return "503 Service Unavailable";
        case 504: return "504 Gateway Timeout";
        case 505: return "505 HTTP Version Not Supported";
        default:  return std::to_string(ev) + " Unknown HTTP error";
        }
    }
};

extern http_error_category_t& http_error_category();

inline std::error_condition make_error_condition(http_errc e){
    return std::error_condition(static_cast<int>(e), http_error_category());
}

inline std::system_error http_exception(int status, const std::string& msg){
    return std::system_error(status, http_error_category(), msg);
}

[[noreturn]] inline void httpthrow(int status, const std::string& msg){
    throw http_exception(status, msg);
}
