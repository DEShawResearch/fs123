#!/usr/bin/env python3
# Parse INI files and dump the parsed config to stdout in same
# format as testconfigparser for easy comparison.
# Note that since python3 isn't installed everywhere, we
# have run this once-and-for-all and stashed the results
# in ./testdata/*.ref
import sys
try:
    from configparser import ConfigParser, ParsingError
    from urllib.parse import quote
    kw = { "inline_comment_prefixes" : ";#", "strict" : False }
except ImportError:
    from ConfigParser import ConfigParser, ParsingError
    from urllib import quote
    kw = {}
c = ConfigParser(**kw)
try:
    c.read(sys.argv[1:])
except ParsingError as e:
    print("Error:", e)
    sys.exit(1)
for i in sorted(c.sections()):
    print(quote(i))
    for j in sorted(c.items(i)):
        print("    "+quote(j[0])+'='+quote(j[1]))

