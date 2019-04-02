#include "crfio.hpp"
#include <core123/sew.hpp>
#include <core123/throwutils.hpp>

namespace {
const std::string arrow{"->"};
const std::string newline{"\n"};
}

using namespace core123;

namespace crfio {

void _write(const std::string& x, FILE *fp){
    sew::fwrite(x.data(), x.size(), 1, fp);
}

void _read(std::string& x, size_t len, FILE *fp){
    x.resize(len);
    sew::fread(&x[0], len, 1, fp);
}

void _xpect(const std::string& x, FILE *fp){
    std::string buf;
    _read(buf, x.size(), fp);
    if( x != buf )
        throw se(EPROTO, "xpect");
}

void out(FILE *out, const std::string& k, const std::string& d){
    // djb just does putc( i%10 + '0' ) in a loop.
    // more or less reliable than fprintf?
    if( fprintf(out, "+%zd,%zd:", k.size(), d.size()) < 0 )
        throw se("fprintf(ks,ds) returned negative");
    _write(k, out);
    _write(arrow, out);
    _write(d, out);
    _write(newline, out);
}

void outeof(FILE *out){
    _write("\n", out);
    sew::fflush(out);
}

size_t _scan_sizet(char delim, FILE *fp){
    const size_t fff = ~size_t(0);
    size_t ret = 0;
    char next = sew::fgetc(fp);
    do{
        unsigned decimal = next-'0';
        if( decimal>9 )
            throw se(EPROTO, "crfio_ref:in:  Expected a digit");
        if( ret > fff/10 || (ret == fff/10 && decimal > fff%10) )
            throw se(EPROTO, "crfio_ref::in - overflow in numeric conversion of ret.  ret=" + std::to_string(ret) + " decimal=" + std::to_string(decimal));
        ret *= 10;
        ret += decimal;
        next = sew::fgetc(fp);
    }while( next != delim );
    return ret;
}

bool in(FILE *in, std::string& k, std::string& d){
    std::string first;
    _read(first, 1, in);
    switch( first[0] ){
    case '\n': return false; // newline indicates the last record.
    case '+': break;
    default:  throw se(EPROTO, "Expected '+' or newline, got something else");
    }
        
    // djb just does n = n*10 + (c-'0') in a loop.
    // More or less reliable than fscanf?  Note that
    // fscanf accepts leading spaces and signs on the decimal
    // lengths, which would be forbidden by a strict parser.
    //if( fscanf(in, "%zd,%zd:", &kl, &dl) != 2 )
    //     throw error("Failed to read lengths");
    size_t kl = _scan_sizet(',', in);
    size_t dl = _scan_sizet(':', in);

    _read(k, kl, in);
    _xpect(arrow, in);
    _read(d, dl, in);
    _xpect(newline, in);
    return true;
}
} // namespace crfio
