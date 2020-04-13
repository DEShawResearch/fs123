#include "fs123/secret_manager.hpp"
#include <sodium.h>
#include <string>
#include <cstring>
#include <cctype>
#include <algorithm>

bool
secret_manager::legal_sid(const std::string& sv){
    // A sid is "legal" if it's '
    //   non-empty
    //   no more than 255 chars long
    //   doesn't start with '.' 
    //   is alphanumeric with underscore, hyphen and period,
    //
    if(sv.size() == 0)
        return false;
    if(sv.size() > 255)
        return false;
    if(sv[0] == '.')
        return false;
    if( sv.end() != std::find_if_not(sv.begin(), sv.end(),
                                     [](char ch) { return isalnum(ch) || ch == '_' || ch == '-' || ch == '.'; }) )
        return false;
     
    return true;
}

// sodium_hex2bin was added to libsodium in 0.5.0.  CentOS6 ships with
// 0.4.5-3, so if we want to link with libsodium, we have to provide
// our own hex2bin.  Between 0.4.5 and 0.5.0, the
// SODIUM_LIBRARY_VERSION_MAJOR changed from 4 to 5, so
#if SODIUM_LIBRARY_VERSION_MAJOR < 5
namespace{

// sodium_hex2bin is copied from the libsodium source tree, which also has this LICENSE file:    
/*
 * ISC License
 *
 * Copyright (c) 2013-2017
 * Frank Denis <j at pureftpd dot org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


int
sodium_hex2bin(unsigned char *const bin, const size_t bin_maxlen,
               const char *const hex, const size_t hex_len,
               const char *const ignore, size_t *const bin_len,
               const char **const hex_end)
{
    size_t        bin_pos = (size_t) 0U;
    size_t        hex_pos = (size_t) 0U;
    int           ret     = 0;
    unsigned char c;
    unsigned char c_acc = 0U;
    unsigned char c_alpha0, c_alpha;
    unsigned char c_num0, c_num;
    unsigned char c_val;
    unsigned char state = 0U;

    while (hex_pos < hex_len) {
        c        = (unsigned char) hex[hex_pos];
        c_num    = c ^ 48U;
        c_num0   = (c_num - 10U) >> 8;
        c_alpha  = (c & ~32U) - 55U;
        c_alpha0 = ((c_alpha - 10U) ^ (c_alpha - 16U)) >> 8;
        if ((c_num0 | c_alpha0) == 0U) {
            if (ignore != NULL && state == 0U && strchr(ignore, c) != NULL) {
                hex_pos++;
                continue;
            }
            break;
        }
        c_val = (c_num0 & c_num) | (c_alpha0 & c_alpha);
        if (bin_pos >= bin_maxlen) {
            ret   = -1;
            errno = ERANGE;
            break;
        }
        if (state == 0U) {
            c_acc = c_val * 16U;
        } else {
            bin[bin_pos++] = c_acc | c_val;
        }
        state = ~state;
        hex_pos++;
    }
    if (state != 0U) {
        hex_pos--;
        errno = EINVAL;
        ret = -1;
    }
    if (ret != 0) {
        bin_pos = (size_t) 0U;
    }
    if (hex_end != NULL) {
        *hex_end = &hex[hex_pos];
    } else if (hex_pos != hex_len) {
        errno = EINVAL;
        ret = -1;
    }
    if (bin_len != NULL) {
        *bin_len = bin_pos;
    }
    return ret;
}
} // namespace anon
#endif // LIBSODIUM_LIBRARY_VERSION_MAJOR < 5

/*static*/ int
secret_manager::hex2bin(unsigned char *const bin, const size_t bin_maxlen,
                        const char *const hex, const size_t hex_len,
                        const char *const ignore, size_t *const bin_len,
                        const char **const hex_end){
    return sodium_hex2bin(bin, bin_maxlen, hex, hex_len, ignore, bin_len, hex_end);
}

