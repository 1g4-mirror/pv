#!/bin/sh
#
# Common functions used by the test scripts.
#
# This file should be sourced by test scripts, and checks that it that has
# been called via "test-env.sh", which sets the variables checked by each
# "true" line in this first section below.
#
# If $useValgrind is 1, changes runTestSubject to run the test subject under
# valgrind; and if valgrind is unavailable, or $SKIP_VALGRIND_TESTS is 1,
# exits with status 77 (skip test) as soon as this file is sourced.
#
# If $SKIP_ALL_TESTS is 1, exits with status 77 immediately.

error="not set - call this from 'make check'"
true "${testSubject:?$error}"
true "${sourcePath:?$error}"
true "${workDir:?$error}"

# Output file for valgrind failures.
valgrindOutputFile="valgrind.out"

# Skip the test, writing a message in the process.
skipTest () {
	printf "%-70s\n" "$*" 1>&9
	exit 77
}

# Skip all tests if so directed.
test "${SKIP_ALL_TESTS}" = "1" && skipTest 'SKIP_ALL_TESTS is set'

# If directed to run under valgrind, check everything is correct first, and
# skip the test if not.
if test "${useValgrind}" = '1'; then
	test "${SKIP_VALGRIND_TESTS}" = '1' && skipTest 'SKIP_VALGRIND_TESTS is set'
	command -v valgrind >/dev/null 2>&1 || skipTest "test requires \`valgrind'"
	valgrindHelp="$(valgrind --help 2>&1)"
	for valgrindOption in "verbose" "show-error-list" "error-exitcode" "track-fds" "leak-check"; do
		printf "%s\n" "${valgrindHelp}" | grep -Fq "${valgrindOption}" \
		|| skipTest "test requires \`valgrind --${valgrindOption}'"
	done
fi

# Skip the current test unless tmux is available and can resize the terminal.
skipTestUnlessTmuxCanResize () {
	# Skip the test if `tmux' is not available.
	command -v tmux >/dev/null 2>&1 || skipTest "test requires \`tmux'"

	# Skip the test if `tmux' does not have "-C".
	if printf '%s\n' "kill-server" | tmux -C -L pvtest 2>&1 | grep -Fq "tmux: unknown option"; then
		skipTest "test requires a newer \`tmux'"
	fi

	# Confirm that tmux can resize its terminal.
	currentSize="$(stty -a | tr ';' '\n' | grep col | sed -n 1p | tr -dc 0-9)"
	checkSize="100"
	test "${currentSize}" = "${checkSize}" && checkSize="102"
	detectedWidth=$(runInTerminal "${checkSize}" 10 "stty -a | tr \\\";\\\" \\\"\\\\n\\\" | grep col | sed -n 1p | tr -dc 0-9")
	if ! test "${detectedWidth}" = "${checkSize}"; then
		printf '%s: %s\n' \
		  "test requires a \`tmux' that can resize a detached window" \
		  "requested width ${checkSize}, detected ${detectedWidth}"
		skipTest "test requires a \`tmux' that can resize a detached window"
	fi
}

# Start a sub-test - increment the sub-test counter, and if running on a
# terminal, display the percentage progress through the sub-tests.  Assumes
# that "$f" is the full path to the running test script.
#
# For the percentage to be calculated correctly, each call to startSubTest
# must be the first thing on the line.
#
# The automatic sub-test count calculation can be adjusted by placing a
# comment of the form "subTest:+123" on a line by itself - this will add 123
# to the count.  Use this if running startSubTest in a loop that always has
# the same number of iterations.
#
# Sets an exit trap to run endSubTests on script exit.
#
# When running under valgrind (useValgrind=1), uses $orig_f instead of $f
# for the displayed test name.
startSubTest () {
	# Do nothing if not running on a terminal.
	test -t 9 2>/dev/null || return 0
	# If the running test's sub-test count isn't yet known, calculate it.
	if ! test -n "${subTestCount}"; then
		subTestCount=$(grep -Ec '^[[:space:]]*startSubTest' "${sourcePath}/$f")
		for addToSubTestCount in $(sed -En '/^[[:space:]]*#?[[:space:]]*subTestCount:\+([0-9]+).*$/s//\1/p' < "${sourcePath}/$f"); do
			test -n "${addToSubTestCount}" && subTestCount=$((subTestCount+addToSubTestCount))
		done
	fi
	# If the sub-test counter hasn't been set yet, initialise it, and
	# set the exit handler.
	if ! test -n "${thisSubTest}"; then
		thisSubTest=0
		trap 'endSubTests' EXIT
	fi
	# Increment the counter.
	thisSubTest=$((1+thisSubTest))
	# Display progress.
	display_f="$f"
	test "${useValgrind}" = '1' && test -n "${orig_f}" && display_f="${orig_f}"
	printf '%3d%%: %s (%s: %d/%d)\r' $((100*(thisSubTest-1)/subTestCount)) "${display_f}" 'sub-test' "${thisSubTest}" "${subTestCount}" >&9
}

# Show a final sub-test count on exit, for a tidier display.
endSubTests () {
	display_f="$f"
	test "${useValgrind}" = '1' && test -n "${orig_f}" && display_f="${orig_f}"
	printf '%3d%%: %s (%s: %d)        \r' 100 "${display_f}" 'sub-tests' "${subTestCount}" >&9
}

# Run the test subject with the given parameters.
runTestSubject () {
	if test "${useValgrind}" = '1'; then
		runWithValgrind "$@"
		return $?
	else
		"${testSubject}" "$@"
		return $?
	fi
}

# Run the test subject under valgrind, with the given parameters passed to
# the test subject; if valgrind reports an error, exit the script
# immediately with status 1, otherwise return the test subject's return
# value.
runWithValgrind () {
	valgrind --tool=memcheck \
	  --verbose --show-error-list=yes --log-file="${workDir}/valgrind.out" \
	  --error-exitcode=125 \
	  --track-fds=yes \
	  --leak-check=full \
	  "${testSubject}" "$@" \
	  9<&-
	returnValue=$?

	cat "${workDir}/valgrind.out"

	if test "${returnValue}" -eq 125; then
		{
		printf "%s\n" "================================================"
		date
		printf "%s\n\n" "Command: ${testSubject} $*"
		cat "${workDir}/valgrind.out"
		printf "%s\n\n" "================================================"
		} >> "${valgrindOutputFile}"
		printf "%s %s\n" "memory check failed - see output file:" "${valgrindOutputFile}" 1>&9
		exit 1
	fi

	return "${returnValue}"
}

# Run the test with the given basename (such as "001_Basic.test") in place
# of this one, under valgrind.
runOtherTestUnderValgrind () {
	useValgrind='1'
	orig_f="$f"
	export useValgrind orig_f
	f="${f%/*}/$1"
	exec sh "${sourcePath}/$f"
	exit 1
}

# Output message $* and set exitStatus to 1, preceding the message with the
# contents of "expected" under workDir if the file exists, and of "stdout"
# if that exists, and of "stderr" if it exists and isn't empty.
#
# If the function "failureAdditionalInfo" exists, it is called just before
# the final message line.
reportFailure () {
	if test -e "${workDir}/expected" && test -e "${workDir}/output" && command -v diff >/dev/null 2>&1 && ! cmp -s "${workDir}/expected" "${workDir}/output"; then
		diff -u "${workDir}/expected" "${workDir}/output"
	else
		test -e "${workDir}/expected" && { printf "%s\n" 'expected:'; sed 's,^,> ,' "${workDir}/expected"; }
		test -e "${workDir}/stdout" && { printf "%s\n" 'program output:'; sed 's,^,> ,' "${workDir}/stdout"; }
	fi
	test -s "${workDir}/stderr" && { printf "%s\n" 'stderr:'; sed 's,^,> ,' "${workDir}/stderr"; }
	type failureAdditionalInfo >/dev/null 2>&1 && failureAdditionalInfo
	printf "%s\n" "$*"
	exitStatus=1
}

# Like reportFailure but exit the script immediately afterwards.
fatalError () {
	reportFailure "$*"
	exit 1
}

# Run the test subject, expecting exit status $1 and, if the exit status is
# 0, stdout contents $2.  A discrepancy is reported with message "$3" and
# exitStatus set to 1.  The remaining parameters are passed to the test
# subject.
#
# If the variable $noCheckOptions is 1, the test subject is run with only
# the provided arguments - "-c ${workDir}/etc" is not prepended.
#
# If the processExpected function exists, it is run after writing $2 to the
# file "expected" under workDir.
#
# If the processStdout function exists, it is run after running the test
# subject, which will have written to file "stdout" under workDir.
#
# If the additionalCheck function exists, it is run (with argument $3) if no
# discrepancies were found; that function should report anything else it
# finds, via reportFailure.
checkOutput () {
	expectedStatus="$1"; shift
	expectedStdout="$1"; shift
	discrepancyMessage="$1"; shift

	true > "${workDir}/expected"
	test -n "${expectedStdout}" && printf '%s\n' "${expectedStdout}" > "${workDir}/expected"

	type processExpected >/dev/null 2>&1 && processExpected

	if test "${noCheckOptions}" = "1"; then
		runTestSubject "$@" >"${workDir}/stdout" 2>"${workDir}/stderr"
		actualStatus=$?
	else
		runTestSubject -c "${workDir}/etc" "$@" >"${workDir}/stdout" 2>"${workDir}/stderr"
		actualStatus=$?
	fi

	if test "${actualStatus}" -ne "${expectedStatus}"; then
		reportFailure "${discrepancyMessage}: exit status was ${actualStatus}, expected ${expectedStatus}"
		exitStatus=1
		return 1
	fi

	test "${actualStatus}" -eq 1 && return 0

	# Strip valgrind output.
	if test "${useValgrind}" = "1"; then
		sed -i '/^-- *[0-9]/d' "${workDir}/stdout"
		sed -i '/^== *[0-9]/d' "${workDir}/stdout"
	fi

	type processStdout >/dev/null 2>&1 && processStdout

	if ! cmp -s "${workDir}/expected" "${workDir}/stdout"; then
		reportFailure "${discrepancyMessage}: output is not as expected"
		return 1
	fi

	if type additionalCheck >/dev/null 2>&1; then
		additionalCheck "${discrepancyMessage}" || return 1
	fi

	return 0
}
