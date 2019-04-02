#pragma once
#include <string>
#include <core123/svto.hpp>
#include <core123/strutils.hpp>

namespace core123 {

// A tiny function to "format" a string as a netstring.
// The inverse is called svscan_netstring in svto.hpp
inline std::string netstring(core123::str_view sv){
    auto ret = std::to_string(sv.size());
    ret.append(1, ':');
    ret.append(sv.data(), sv.size());
    ret.append(1, ',');
    return ret;
}

inline void fput_netstring(FILE *fp, core123::str_view sv) {
    if (fprintf(fp, "%zu:", sv.size()) <= 0 || ferror(fp)) {
        throw std::system_error(errno, std::system_category(),
                     "failed to write netstring size "+std::to_string(sv.size()));
    }
    if (fwrite(sv.data(), 1, sv.size(), fp) != sv.size() || ferror(fp)) {
        throw std::system_error(errno, std::system_category(),
                      "failed to write "+std::to_string(sv.size())+" bytes");
    }
}

inline std::ostream& sput_netstring(std::ostream& out, core123::str_view sv) {
    // we convert the number ourselves because who knows what state
    // the stream numeric conversion is in.
    // 64 bit int needs 20 chars in decimal, so plenty of space
    char buf[32], *cp=buf+sizeof(buf)-1;
    *cp-- = '\0';
    size_t val = sv.size();
    do {
        if (cp == buf)
            throw std::runtime_error("size_t too big for buffer! "+std::to_string(sv.size()));
        unsigned b = val % 10u;
        val /= 10u;
        *cp-- = b + '0';
    } while (val);
    out << cp+1 << ':';
    out.write(sv.data(), sv.size());
    out << ',';
    return out;
}

// svscan a netstring into a str_view.  This one feels a bit odd...
// second argument is a str_view, so the first argument had better
// hang around as long as svp is needed/used!!
inline size_t svscan_netstring(core123::str_view sv, core123::str_view* svp, size_t start){
    size_t len;
    start = svscan<size_t>(sv, &len, start); // start now points to colon
    if(sv.size() < 2+len+start)
        throw std::invalid_argument("svscan_netstring: sv too short: " + std::string(sv.substr(start,30)));
    if(sv[start] != ':')
        throw std::invalid_argument("svscan_netstring: did not see colon in: " + std::string(sv.substr(start,30)));
    if(sv[start+len+1] != ',')
        throw std::invalid_argument("svscan_netstring: did not see trailing comma in " + std::string(sv.substr(start,30)));
    *svp = sv.substr(start+1, len);
    return start+len+2;
}

inline bool sget_netstring(std::istream& inp, std::string* sp) {
    inp >> std::ws;
    if (inp.eof())
        return false;
    unsigned char c;
    size_t sz = 0;
    while (1) {
        c = '\0';
        inp >> c;
        if (c == ':')
            break;
        else if (inp.eof())
            throw std::runtime_error("sget_netstring got eof when expecting digit, current sz "+std::to_string(sz));
        else if (!isdigit(c))
            throw std::runtime_error("sget_netstring got unexpected character "+std::to_string(c)+" when expecting digit, current sz "+std::to_string(sz));
        sz = sz * 10 + (c - '0');
    }
    if (sz) {
        sp->resize(sz);
        inp.read(&(*sp)[0], sz);
        if (inp.eof())
            throw std::runtime_error("sget_netstring got eof when reading string data, "+std::to_string(sz) + " bytes");
    } else {
        *sp = "";
    }
    inp >> c;
    if (c != ',') {
        throw std::runtime_error("sget_netstring got unexpected character "+std::to_string(c)+" when expecting comma, sz "+std::to_string(sz));
    }
    return true;
}

} // namespace core123
