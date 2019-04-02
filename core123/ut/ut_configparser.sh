#!/bin/bash
# This script assumes that it is being run in a 'build' directory,
# where ./testconfigparser already exists.  I.e., you should run
# 'make' before running this script.
#
# It also assumes that an environment variable, TESTDATA points to a
# directory with .ini files for testing.  (This is set up
# automatically by 'make runtests).  If not, it assumes that
# the testdata is in the .../testdata sub-directory sibling
# of the script itself (as it is in the source tree).
#

: ${TESTDATA:=$(dirname $0)/testdata}
for i in $TESTDATA/*.ini; do
	printf "Parsing $i: "; TESTCFP_SUMMARY=true ./testconfigparser $i
	diff <(./testconfigparser $i) <($TESTDATA/refconfigparser.py $i)
	r=$?
	case "$r" in
	0)	;;
	*)	echo "Exit status $r parsing $i";;
	esac
done
# finally, read all test files as single config
i=$TESTDATA/*.ini
printf "Parsing all $i: "; TESTCFP_SUMMARY=true ./testconfigparser $i
diff <(./testconfigparser $i) <($TESTDATA/refconfigparser.py $i)
