// Test strutils.hpp

#include <core123/strutils.hpp>
#include <cinttypes>
#include <core123/ut.hpp>

using core123::str;
using core123::startswith;
using core123::endswith;
using core123::lstrip;
using core123::rstrip;
using core123::strip;
using core123::tohex;
using core123::quopri;
using core123::cstr_encode;
using core123::urlescape;
using core123::hexdump;
using core123::svsplit_exact;
using core123::svsplit_any;
using core123::str_view;
using core123::CILess;

using namespace std;

// should this be in uth.hpp?
bool eqv(vector<str_view> a, vector<str_view> b,
         const char *atxt, const char *btxt){
    if(a.size() != b.size()){
        cerr << "a.size()=" << a.size() << " != b.size() " << b.size() << "  a=" << atxt << " b=" << btxt << endl;
        return false;
    }
    for(size_t i=0; i<a.size(); ++i){
        if(a[i] != b[i]){
            cerr << "FAILED: a[i] != b[i]: i = " << i;
            cerr << " a[i] = '" << a[i] << "'";
            cerr << " b[i] = '" << b[i] << "'";
            cerr << endl;
            return false;
        }
    }
    return true;
}

typedef map<string,string,CILess> CIMap;
std::ostream& operator<<(std::ostream& os, const CIMap::iterator &p){
    return os << '\"' << p->first << "\",\"" << p->second << '\"';
}

#define EQUALV(a, b) eqv(a, b, #a, #b) ? (utpass++) : (utfail++)
#define COMMA ,

int main(int argc, char **argv) {
    EQUALV(svsplit_exact("x", "x"), {"" COMMA ""});
    EQUALV(svsplit_exact("", "x"), {""});
    EQUALV(svsplit_exact("", "xyzzy"), {""});
    EQUALV(svsplit_exact("a b c", " "), {"a" COMMA "b" COMMA "c"});
    EQUALV(svsplit_exact(" a b", " "), {"" COMMA "a" COMMA "b"}); // initial empty
    EQUALV(svsplit_exact("a b ", " "), {"a" COMMA "b" COMMA ""}); // no final empty
    EQUALV(svsplit_exact("a b c", " b"), {"a" COMMA " c"});
    EQUALV(svsplit_exact("a b c" , "c"), {"a b " COMMA ""});
    EQUALV(svsplit_exact("a b c", "  "), {"a b c"});
    // repeated delimiters leave empty strings in the output.
    EQUALV(svsplit_exact("abcbcd", "bc"), {"a" COMMA "" COMMA "d"});
    EQUALV(svsplit_exact("abcbcbcbcbcbcd", "bc"), {"a" COMMA "" COMMA "" COMMA "" COMMA "" COMMA "" COMMA "d"});
    EQUALV(svsplit_exact("", "abc"), {""});
    EQUALV(svsplit_exact("ab c d", "b c"), {"a" COMMA " d"});
    EQUALV(svsplit_exact("abc", "c", 2), {"" COMMA ""});
    EQUALV(svsplit_exact("abc", "c", 3), {""});
    EQUALV(svsplit_exact("abc", "c", 4), {});
    EQUALV(svsplit_exact("abc", "c", 1000), {});
    
    // Let's try sv_split_any
    EQUALV(svsplit_any("", " "), {""});
    EQUALV(svsplit_any("", ",\t "), {""});
    EQUALV(svsplit_any("a, b, c", ", "), {"a" COMMA "b" COMMA "c"});
    EQUALV(svsplit_any("a b c", ", "), {"a" COMMA "b" COMMA "c"});
    EQUALV(svsplit_any("a   b,,, c d,e", ", "), {"a" COMMA "b" COMMA "c" COMMA "d" COMMA "e"});
    EQUALV(svsplit_any("a,b c ", ", "), {"a" COMMA "b" COMMA "c"});
    EQUALV(svsplit_any(" ,a ,b ,c", ", "), {"" COMMA "a" COMMA "b" COMMA "c"});
    EQUALV(svsplit_any("a, b,c d-e%f", "/xyz"), {"a, b,c d-e%f"});
    EQUALV(svsplit_any("a, b, c  d", ""), {"a, b, c  d"});
    EQUALV(svsplit_any("", ""), {""});
    EQUALV(svsplit_any("  ", ""), {"  "});

    EQSTR (str(12345), "12345");
    EQUAL (atof(str(3.14159).c_str()), 3.14159);

    EQUAL (startswith("foo", "foobar"), false);
    EQUAL (startswith("foobar", "f"), true);
    EQUAL (startswith("foobar", "foo"), true);
    EQUAL (startswith("", ""), true);
    EQUAL (startswith("x", ""), true);
    EQUAL (startswith("x", "x"), true);

    EQUAL (endswith("foo", "foobar"), false);
    EQUAL (endswith("foobar", "bar"), true);
    EQUAL (endswith("foobar", ""), true);
    EQUAL (endswith("foobar", "r"), true);
    EQUAL (endswith("", ""), true);
    EQUAL (endswith("x", ""), true);
    EQUAL (endswith("x", "x"), true);

    EQSTR (lstrip(""), "");
    EQSTR (lstrip(" "), "");
    EQSTR (lstrip(" \t\v\f\n\r"), "");
    EQSTR (lstrip("a"), "a");
    EQSTR (lstrip(" a"), "a");
    EQSTR (lstrip(" \t\v\f\n\ra"), "a");
    EQSTR (lstrip("foo "), "foo ");
    EQSTR (lstrip(" foo "), "foo ");
    EQSTR (lstrip(" \t\v\f\n\rfoo "), "foo ");

    EQSTR (rstrip(""), "");
    EQSTR (rstrip(" "), "");
    EQSTR (rstrip(" \t\v\f\n\r"), "");
    EQSTR (rstrip("a"), "a");
    EQSTR (rstrip("a "), "a");
    EQSTR (rstrip("a \t\v\f\n\r"), "a");
    EQSTR (rstrip(" foo"), " foo");
    EQSTR (rstrip(" foo "), " foo");
    EQSTR (rstrip(" foo \t\v\f\n\r"), " foo");

    EQSTR (strip(""), "");
    EQSTR (strip(" "), "");
    EQSTR (strip(" \t\v\f\n\r"), "");
    EQSTR (strip("a"), "a");
    EQSTR (strip(" a"), "a");
    EQSTR (strip("a "), "a");
    EQSTR (strip(" a "), "a");
    EQSTR (strip(" \t\v\f\n\ra"), "a");
    EQSTR (strip("a \t\v\f\n\r"), "a");
    EQSTR (strip(" \t\v\f\n\ra \t\v\f\n\r"), "a");
    EQSTR (strip("foo "), "foo");
    EQSTR (strip(" foo"), "foo");
    EQSTR (strip(" foo "), "foo");
    EQSTR (strip(" \t\v\f\n\rfoo  \t\v\f\n\r"), "foo");

    uint32_t v32 = 0xd3ad;
    EQSTR (tohex(v32, false), "d3ad");
    EQSTR (tohex(v32), "0000d3ad");

    uint64_t v64 = 0xd35dbeef;
    EQSTR (tohex(v64, false), "d35dbeef");
    EQSTR (tohex(v64), "00000000d35dbeef");

    EQSTR (quopri(""), "");
    EQSTR (quopri(" "), "=20");
    EQSTR (quopri("a"), "a");
    EQSTR (quopri("hello"), "hello");
    EQSTR (quopri("he"), "he");
    EQSTR (quopri("\001\002Hell0\177"), "=01=02Hell0=7F");

    EQSTR (urlescape(""), "");
    EQSTR (urlescape(" "), "%20");
    EQSTR (urlescape("a"), "a");
    EQSTR(urlescape("abc"), "abc" );
    EQSTR(urlescape("abc?def"), "abc%3Fdef");
    EQSTR(urlescape("%%"), "%25%25");
    EQSTR(urlescape("\xff"), "%FF");
    EQSTR(urlescape("abc\xea""def"), "abc%EAdef");  // N.B.  "\xeadef" looks like a 5-hex-digit escape
    EQSTR(urlescape("\x3f\x2f\x19\xc3\xf0"), "%3F/%19%C3%F0");  // N.B.  0x2f == '/'
    EQSTR (urlescape("hello"), "hello");
    EQSTR (urlescape("\001\002H~e.l/l-0_\177"), "%01%02H~e.l/l-0_%7F");


    EQSTR (hexdump(" "), "   ");
    EQSTR (hexdump(" ", true), " 20");
    EQSTR (hexdump(" ", true, "."), ".20");
    EQSTR (hexdump(""), "");
    EQSTR (hexdump("", true, "."), "");
    EQSTR (hexdump("hello"), "  h  e  l  l  o");
    EQSTR (hexdump("\001\002Hell0\177"), " 01 02  H  e  l  l  0 7f");
    EQSTR (hexdump("\001\002Hell0\177", true), " 01 02 48 65 6c 6c 30 7f");
    EQSTR (hexdump("\001\002Hell0\177", true, "."), ".01.02.48.65.6c.6c.30.7f");

    CIMap m;
    m["hello"] = "world";
    m["Mark"] = "Spencer";
    m["Force"] = "Ten";
    m["mark"] = "six";
    vector<pair<string,string> > tests = {
        {"MARK", "six"},
        {"mark", "six"},
        {"Mark", "six"},
        {"HELLO", "world"},
        {"hello", "world"},
        {"Hello", "world"},
        {"force", "Ten"},
        {"FORCE", "Ten"},
    };
    DIAG(_ut, "Testing ciless map iterator via range-for:");
    for (const auto& x: m) {
        EQSTR(x.second, m[x.first]);
    }
    DIAG(_ut,"Testing case-insensitive lookups:");
    for (const auto& k: tests) {
        EQSTR(k.second, m[k.first]);
        auto p = m.find(k.first);
        NOTEQUAL(p, m.end());
        EQUAL(strcasecmp(p->first.c_str(), k.first.c_str()), 0);
        EQSTR(p->second, k.second);
    }
    vector<string> failtests = {"", "abc", "zzz"};
    for (const auto& k : failtests) {
        auto q = m.find(k);
        EQUAL(q, m.end());
    }
    EQUAL(cstr_encode(""), "");
    EQUAL(cstr_encode("x"), "x");
    EQUAL(cstr_encode("x\ny\tz\r\n\xff"), "x\\x0ay\\x09z\\x0d\\x0a\\xff");
    return utstatus();
}
