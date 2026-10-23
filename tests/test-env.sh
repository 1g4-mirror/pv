#!/bin/sh
#
# Set up the environment variables, temporary working area, and cleanup
# processes required to run each test.
#
# Tests must exit with 0 for success, 1 for failure, 77 to skip the test, or
# 99 for a fatal error with the test framework.

# Parameters.
testSubject="./pv"
sourcePath="${srcdir?:not set - call this from 'make check'}"
true "${f:?not set - expecting test script filename - call this from 'make check'}"

# Set everything to the "C" locale.
LANG=C
LC_ALL=C
export LANG LC_ALL

# Temporary working area, for the test scripts to use.
workDir=$(mktemp -d 2>/dev/null) || { workDir="./.test-tmp"; mkdir -p "${workDir}"; }

# Clean up the temporary files on exit, in case of interruption.
trap 'rm -rf "${workDir}"' EXIT

# Show the operator that the test has begun.
test -t 2 && printf '%s: %s\r' '----' "$f" 1>&2

# Temporary working files, for the test scripts to use.
workFile1="${workDir}/temp1"
workFile2="${workDir}/temp2"
workFile3="${workDir}/temp3"
workFile4="${workDir}/temp4"
workFile5="${workDir}/temp5"

# Variables used by the test scripts.
export f testSubject sourcePath workDir workFile1 workFile2 workFile3 workFile4 workFile5

# TODO: remove workFile1..5 from here and from docs/DEVELOPERS.md, once none
# of the tests use them any more.
