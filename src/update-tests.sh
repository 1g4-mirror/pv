#!/bin/sh
#
# Make sure that all tests are listed in the TESTS variable in Makefile.am.
#
# Tests are assumed to be under the "tests" subdirectory and have names of
# the form "name.test" where "name" is any valid filename portion that
# doesn't contain a newline.
#
# This script isn't called automatically - it will need to be run whenever
# new tests are added.
#
# Run it from the directory containing the configure.ac file.

topDir='.'

test -e "${topDir}/configure.ac" \
|| { printf '%s: %s: %s\n' "${0##*/}" "${topDir}/configure.ac" 'not found' 1>&2; exit 1; }

# Update TESTS in Makefile.am.

automakeFile="${topDir}/Makefile.am"

listStartLine="$(grep -hnE '^[[:space:]]*TESTS[[:space:]]*=' "${automakeFile}" 2>/dev/null | cut -d : -f 1 | sed -n 1p)"
test -n "${listStartLine}" \
|| { printf '%s: %s: %s\n' "${0##*/}" "${automakeFile}" 'no TESTS line found' >&2; continue; }

listEndLine="$(sed -n "${listStartLine}",'/[^\\]$/=' "${automakeFile}" | sed -n '$p')"
test -n "${listEndLine}" \
|| { printf '%s: %s: %s\n' "${0##*/}" "${automakeFile}" 'unable to find the end of the TESTS list' >&2; continue; }

sed "${listStartLine},\$d" < "${automakeFile}" > "${automakeFile}.new"
printf '%s\n' 'TESTS = \' >> "${automakeFile}.new"
find "${topDir}/tests" -type f -name "*.test" -printf 'tests/%f \\\n' \
| sort \
| sed '$s/ \\$//' \
>> "${automakeFile}.new"
sed "1,${listEndLine}d" < "${automakeFile}" >> "${automakeFile}.new"

if cmp -s "${automakeFile}" "${automakeFile}.new" 2>/dev/null; then
	rm -f "${automakeFile}.new"
else
	printf '%s: %s\n' 'updating' "${automakeFile}"
	mv -f "${automakeFile}.new" "${automakeFile}"
fi
