#!/bin/sh
#
# Set up the environment variables, temporary working files, and cleanup
# processes required to run each test.
#
# Tests must exit with 0 for success, 1 for failure, 77 to skip the test, or
# 99 for a fatal error with the test framework.

# Parameters.
testSubject="./pv"
sourcePath="${srcdir}"

# Set everything to the "C" locale.
LANG=C
LC_ALL=C
export LANG LC_ALL

# Temporary working files, for the test scripts to use.
workFile1=$(mktemp 2>/dev/null) || workFile1="./.tmp1"
workFile2=$(mktemp 2>/dev/null) || workFile2="./.tmp2"
workFile3=$(mktemp 2>/dev/null) || workFile3="./.tmp3"
workFile4=$(mktemp 2>/dev/null) || workFile4="./.tmp4"

# Clean up the temporary files on exit, in case we are interrupted.
trap 'rm -f "${workFile1}" "${workFile2}" "${workFile3}" "${workFile4}"' EXIT

# Variables used by the test scripts.
export testSubject sourcePath workFile1 workFile2 workFile3 workFile4
