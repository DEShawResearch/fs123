#pragma once

/* DOCUMENTATION_BEGIN

It's kinda' cool that you can do:
   printf("foo: %i bar: %i", foo, bar);
   // and then later read it back with:
   scanf("foo: %i bar: %i", &foo, &bar);

Isn't it a shame that you can't that with C++ I/O streams?

Well now you can.  Just say:

   #include <core123/const_ntbsExtractor.hpp>
   someostream << "foo: " << foo << " bar: " << bar;
   // and then later
   someistream >> "foo: " >> foo >> " bar: " >> bar;

Following the C99 standard specification of scanf: "White space in the
format should match any amount of white space, including none, in the
input."

C++ stream extractors are deficient and surprising in lots of ways.
This little header doesn't change that.  Use with care.

DOCUMENTATION_END */

#include <iostream>

inline 
std::istream& operator>> (std::istream& in, char const* str) { 
    int i = 0; 
    for (char c; str[i]; ++i) {
        if(isspace(str[i])){
            // skip any additional whitespace in str
            do{
                i++;
            }while(isspace(str[i]));
            // str[i] is the next non-space char.  But it *might*
            // be NUL.
            while(in.get(c) && isspace(c))
                ;
            // c is the next non-space.  But it *might* be EOF. 
            if(!str[i]){
                // we're looking at terminal whitespace in str.
                // unget the most recent non-white character.
                if(in)
                    in.unget();
                return in;
            }
        }else{
            in.get(c);
        }
        // str[i] is non-NUL and non-space.  Either in is false
        // (in which case we're at EOF), or c is the next bona
        // fide, non-white char.
        if(!in || str[i] != c)
            break;
    }
    if (str[i]) 
        in.setstate(std::ios_base::failbit); 
    return in; 
} 

