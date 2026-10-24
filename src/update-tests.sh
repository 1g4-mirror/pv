#!/bin/sh
#
# Make sure that all tests are listed in the TESTS variable in Makefile.am.
#
# Tests are assumed to be under the "tests" subdirectory and have names of
# the form "name.test" where "name" is any valid filename portion that
# doesn't contain a newline.
#
# Memory safety counterparts to each test will be prefixed with a number
# above 500, if the original test's name started with a number, and will run
# the original, but with each instance of the test subject being run under
# valgrind.  Counterparts are not generated for tests that never call the
# runTestSubject or checkOutput functions.
#
# This script isn't called automatically - it will need to be run whenever
# new tests are added.
#
# Run it from the directory containing the configure.ac file.

topDir='.'

test -e "${topDir}/configure.ac" \
|| { printf '%s: %s: %s\n' "${0##*/}" "${topDir}/configure.ac" 'not found' 1>&2; exit 1; }


# Regenerate the memory safety counterparts to each test.

find "${topDir}/tests" -type f -name "*.test" -printf '%f\n' \
| sort \
| while read -r testFile; do
	# Skip tests that never call runTestSubject or checkOutput.
	grep -Fq -e 'runTestSubject' -e 'checkOutput' "${topDir}/tests/${testFile}" 2>/dev/null || continue
	# Skip tests that call runOtherTestUnderValgrind.
	grep -Fq 'runOtherTestUnderValgrind' "${topDir}/tests/${testFile}" 2>/dev/null && continue

	# Generate the filename of the memory safety counterpart test.
	counterpartFile="Memory_safety_-_${testFile}"

	# Replace the number, if the test name started with a 3-digit number
	# less than 500.
	if printf '%s\n' "${testFile}" | grep -Eq '^[0-4][0-9][0-9]_'; then
		testNumber="${testFile%%_*}"
		# Strip leading zeroes so "008" doesn't get treated as octal.
		testNumberAsInteger="${testNumber#0}"
		testNumberAsInteger="${testNumberAsInteger#0}"
		counterpartNumber=$((500+testNumberAsInteger)) || continue
		counterpartFile="${counterpartNumber}_Memory_safety_-_${testFile#???_}"
	fi

	# Generate a new memory safety counterpart test.
	cat > "${topDir}/tests/${counterpartFile}.new" <<EOF
#!/bin/sh
#
# Memory safety check counterpart for test ${testNumber}.
error="not set - call this from 'make check'"
true "\${sourcePath:?\$error}"
test -n "\${sourcePath}" && . "\${sourcePath}/tests/test-functions.sh"
runOtherTestUnderValgrind '${testFile}'
EOF
	# Move the test into place if it's not already there.
	if cmp -s "${topDir}/tests/${counterpartFile}" "${topDir}/tests/${counterpartFile}.new" 2>/dev/null; then
		rm -f "${topDir}/tests/${counterpartFile}.new"
	else
		printf '%s: %s\n' 'updating' "${topDir}/tests/${counterpartFile}"
		mv -f "${topDir}/tests/${counterpartFile}.new" "${topDir}/tests/${counterpartFile}"
	fi
done

# Update TESTS in Makefile.am.

automakeFile="${topDir}/Makefile.am"

listStartLine="$(grep -hnE '^[[:space:]]*TESTS[[:space:]]*=' "${automakeFile}" 2>/dev/null | cut -d : -f 1 | sed -n 1p)"
test -n "${listStartLine}" \
|| { printf '%s: %s: %s\n' "${0##*/}" "${automakeFile}" 'no TESTS line found' >&2; exit 1; }

listEndLine="$(sed -n "${listStartLine}",'/[^\\]$/=' "${automakeFile}" | sed -n '$p')"
test -n "${listEndLine}" \
|| { printf '%s: %s: %s\n' "${0##*/}" "${automakeFile}" 'unable to find the end of the TESTS list' >&2; exit 1; }

{
sed "${listStartLine},\$d" < "${automakeFile}"
printf '%s\n' "TESTS = \\"
find "${topDir}/tests" -type f -name "*.test" -printf 'tests/%f \\\n' \
| sort \
| sed '$s/ \\$//'
} \
> "${automakeFile}.new"
sed "1,${listEndLine}d" < "${automakeFile}" >> "${automakeFile}.new"

if cmp -s "${automakeFile}" "${automakeFile}.new" 2>/dev/null; then
	rm -f "${automakeFile}.new"
else
	printf '%s: %s\n' 'updating' "${automakeFile}"
	mv -f "${automakeFile}.new" "${automakeFile}"
fi
