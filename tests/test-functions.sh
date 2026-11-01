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
#
# Sourcing this file sets an EXIT trap, so the calling script shouldn't trap
# EXIT.  Instead, it can define a function "onExit" which will be called by
# the exit handler.
#
# Copyright 2026 Andrew Wood
# License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.

error="not set - call this from 'make check'"
true "${testSubject:?$error}"
true "${sourcePath:?$error}"
true "${workDir:?$error}"

# Skip the test, writing a message in the process.
skipTest () {
	printf '%-77s\n' "$f: $*" 1>&9
	exit 77
}

# Skip all tests if so directed.
test "${SKIP_ALL_TESTS}" = "1" && skipTest 'SKIP_ALL_TESTS is set'

# If directed to run under valgrind, check everything is correct first, and
# skip the test if not.
if test "${useValgrind}" = '1'; then
	test "${SKIP_VALGRIND_TESTS}" = '1' && skipTest 'SKIP_VALGRIND_TESTS is set'

	if command -v arch >/dev/null 2>&1 && arch | grep -Fq -e arm -e aarch; then
		test "${ENABLE_VALGRIND_ARM}" = "1" \
		|| skipTest 'skipping valgrind on ARM without ENABLE_VALGRIND_ARM'
	fi

	command -v valgrind >/dev/null 2>&1 || skipTest "test requires \`valgrind'"

	valgrindHelp="$(valgrind --help 2>&1)"
	for valgrindOption in "verbose" "show-error-list" "error-exitcode" "track-fds" "leak-check"; do
		printf "%s\n" "${valgrindHelp}" | grep -Fq "${valgrindOption}" \
		|| skipTest "test requires \`valgrind --${valgrindOption}'"
	done
fi

# Set an exit trap to run appropriate cleanups.
trap 'testCleanup' EXIT

# Skip the current test unless tmux is available and can resize the terminal.
skipTestUnlessTmuxCanResize () {
	# Skip the test if `tmux' is not available.
	command -v tmux >/dev/null 2>&1 || skipTest "test requires \`tmux'"

	# Skip the test if `tmux' does not have "-C".
	if printf '%s\n' "kill-server" | tmux -C -S "${workDir}/tmuxCheckForDashC" 2>&1 | grep -Fq "tmux: unknown option"; then
		skipTest "test requires a newer \`tmux'"
	fi
	rm -f "${workDir}/tmuxCheckForDashC"

	# Confirm that tmux can resize its terminal.
	currentSize="$(stty -a 2>/dev/null | tr ';' '\n' | grep col | sed -n 1p | tr -dc 0-9)"
	checkSize="100"
	test "${currentSize}" = "${checkSize}" && checkSize="102"
	detectedWidth=$(runInTerminal "${checkSize}" 10 "stty -a 2>/dev/null | tr \\\";\\\" \\\"\\\\n\\\" | grep col | sed -n 1p | tr -dc 0-9")
	if ! test "${detectedWidth}" = "${checkSize}"; then
		printf '%s: %s\n' \
		  "test requires a \`tmux' that can resize a detached window" \
		  "requested width ${checkSize}, detected ${detectedWidth}"
		skipTest "test requires a \`tmux' that can resize a detached window"
	fi
}

# Run the given command $3 inside a terminal of the given width $1, waiting
# up to $2 seconds, and output the results.
#
# The calling script must run skipTestUnlessTmuxCanResize before any calls
# to this function.
#
# The function "sendTmuxCommands", if it exists, is called after the command
# has been started inside the terminal; its stdout is sent to the tmux
# control socket, so it can control the tmux session.
#
runInTerminal () {
	terminalWidth="$1"
	secondsToWait="$2"
	commandToRun="$3"

	test -n "${tmuxCounter}" || tmuxCounter=0
	tmuxCounter=$((1+tmuxCounter))
	tmuxTestSocket="${workDir}/runInTerminal-${f##*/}.${tmuxCounter}"

	# The total number of 0.1-second waits allowed before the
	# seconds-to-wait time has elapsed.  The wait is cut short when the
	# command finishes (when tmuxEnd is created).
	maxWait=$((10*secondsToWait))

	rm -f "${workDir}/tmuxOutput.${tmuxCounter}" "${workDir}/tmuxEnd.${tmuxCounter}" "${workDir}/subprocessValgrind.${tmuxCounter}"

	# Export tmuxCounter so it's available to tmux, meaning that the
	# caller can use it in supplied commands, for instance when telling
	# pv to use a PID file - this would further isolate tmux runs from
	# each other, which is needed if for instance a tmux pane spawn
	# fails once and succeeds on a retry, potentially causing 2 of the
	# same test commands to bump into each other.
	export tmuxCounter

	{
	printf '%s\n' "set remain-on-exit on"
	printf '%s\n' "set default-shell /bin/sh"
	printf '%s\n' "new-session -d -x ${terminalWidth} -y 5"
	# starting the session doesn't always correctly set the size
	printf '%s\n' "resize-window -x ${terminalWidth} -y 5"
	# tmux 1.8 doesn't have "resize-window"
	printf '%s\n' "resize-pane -x ${terminalWidth} -y 5"
	printf '%s\n' "pipe-pane 'cat > ${workDir}/tmuxOutput.${tmuxCounter}'"
	waitCount=0
	# First, wait for the pipe-pane "cat" to be started.
	while test ${waitCount} -lt ${maxWait} && ! test -e "${workDir}/tmuxOutput.${tmuxCounter}"; do
		sleep 0.1
		waitCount=$((1+waitCount))
	done
	# Now start the command.
	# On some systems, tmuxOutput ends up with the shell prompt in it
	# before the command below is run, so output a marker to show where
	# the output really starts.
	printf '%s\n' "respawn-pane -k \"echo; exec 8>>${workDir}/subprocessValgrind.${tmuxCounter}; echo START; ${commandToRun}; true > ${workDir}/tmuxEnd.${tmuxCounter}\""
	# Wait for the process started by "respawn-pane" to start.
	while test ${waitCount} -lt ${maxWait} && ! test -e "${workDir}/subprocessValgrind.${tmuxCounter}"; do
		sleep 0.1
		waitCount=$((1+waitCount))
	done
	# Allow the caller to send arbitrary commands to the tmux socket.
	type sendTmuxCommands >/dev/null 2>&1 && sendTmuxCommands
	# Wait for the process started by "respawn-pane" to end.
	while test ${waitCount} -lt ${maxWait} && ! test -e "${workDir}/tmuxEnd.${tmuxCounter}"; do
		sleep 0.1
		waitCount=$((1+waitCount))
	done
	# Terminate the tmux server.
	printf '%s\n' "kill-server"
	# Wait a short time to allow for a clean exit.
	sleep 0.2
	} \
	| tmux -C -S "${tmuxTestSocket}" >/dev/null

	rm -f "${tmuxTestSocket}"
	tmuxTestSocket=''

	tr -d '\r' < "${workDir}/tmuxOutput.${tmuxCounter}" | sed '1,/^START$/d'

	# Send the valgrind output to file descriptor 8, if that fd is open.
	{ true >&8; } 2>/dev/null \
	&& test -e "${workDir}/subprocessValgrind.${tmuxCounter}" \
	&& cat "${workDir}/subprocessValgrind.${tmuxCounter}" >&8
}

# Call runInTerminal(), except that the command $3 runs in a () subshell
# after sourcing this file, so that the command can use these functions.
#
# If $workDir/testInput exists, the command's stdin is redirected from it.
#
# Output is written to the file testOutput under $workDir.  If the command
# succeeded, output from valgrind is removed from testOutput.
#
# Returns 0 (true) if the command succeeded.  If the command failed, return
# 1 (false), and also sets exitStatus to 1.
#
# Running a process under tmux involves a lot more resource consumption than
# running it normally, particularly in terms of number of running processes.
# Local resource limits can cause transient failures where the process never
# gets run and so there's no output and no failure.  For that reason, if
# there is no failure but no output was written, this function will wait a
# short time and try again, up to $attemptTestsInTerminal times.
testInTerminal () {
	terminalWidth="$1"
	secondsToWait="$2"
	commandToRun="$3"

	test -e "${workDir}/testInput" && commandToRun="exec <\"${workDir}/testInput\"; ${commandToRun}"

	# By default, retry up to 3 times if the output is empty.
	test -n "${attemptTestsInTerminal}" || attemptTestsInTerminal='3'

	# Ensure there's a valid number in the variable.
	test "${attemptTestsInTerminal}" -gt 0 || attemptTestsInTerminal=1

	testAttempt=0
	while test "${testAttempt}" -lt "${attemptTestsInTerminal}"; do
		testAttempt=$((1+testAttempt))
		test "${testAttempt}" -gt 1 && sleep "${testAttempt}"

		rm -f "${workDir}/operatorMessage.${testAttempt}" "${workDir}/testFailed.${testAttempt}" "${workDir}/testOutput"

		runInTerminal \
		  "${terminalWidth}" \
		  "${secondsToWait}" \
		  ". \"${sourcePath}/tests/test-functions.sh\" && ( ${commandToRun} ) 9>\"${workDir}/operatorMessage.${testAttempt}\" || true > \"${workDir}/testFailed.${testAttempt}\"" \
		> "${workDir}/testOutput"

		if test -e "${workDir}/testFailed.${testAttempt}"; then
			if test -s "${workDir}/operatorMessage.${testAttempt}"; then
				cat "${workDir}/operatorMessage.${testAttempt}" 1>&9
			else
				reportFailure "non-zero exit status for: ${commandToRun}"
			fi
			exitStatus=1
			return 1
		fi

		# If under valgrind, strip the valgrind output.
		if test "${useValgrind}" = '1'; then
			sed '/^==/,$d' < "${workDir}/testOutput" > "${workDir}/testOutput.new"
			mv -f "${workDir}/testOutput.new" "${workDir}/testOutput"
		fi

		# No more retries are needed, if there is any output.
		test -s "${workDir}/testOutput" && break
	done

	return 0
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
# When running under valgrind (useValgrind=1), uses $orig_f instead of $f
# for the displayed test name.
#
# The endSubTests function will be called on exit if any sub-tests ran.
startSubTest () {
	# Do nothing other than increment the counter if not running on a
	# terminal.
	if ! test -t 9 2>/dev/null; then
		test -n "${thisSubTest}" || thisSubTest=0
		thisSubTest=$((1+thisSubTest))
		return 0
	fi
	# If the running test's sub-test count isn't yet known, calculate it.
	if ! test -n "${subTestCount}"; then
		subTestCount=$(grep -Ec '^[[:space:]]*startSubTest' "${sourcePath}/$f")
		for addToSubTestCount in $(sed -En '/^[[:space:]]*#?[[:space:]]*subTestCount:\+([0-9]+).*$/s//\1/p' < "${sourcePath}/$f"); do
			test -n "${addToSubTestCount}" && subTestCount=$((subTestCount+addToSubTestCount))
		done
	fi
	# If the sub-test counter hasn't been set yet, initialise it.
	# The exit handler (testCleanup) will call endSubTests if
	# thisSubTest isn't blank.
	test -n "${thisSubTest}" || thisSubTest=0
	# Increment the counter.
	thisSubTest=$((1+thisSubTest))
	# Display progress.
	display_f="$f"
	test "${useValgrind}" = '1' && test -n "${orig_f}" && display_f="${orig_f}"
	printf '%3d%%: %s (%s: %d/%d)\r' $((100*(thisSubTest-1)/subTestCount)) "${display_f}" 'sub-test' "${thisSubTest}" "${subTestCount}" >&9
}

# Show a final sub-test count on exit, for a tidier display, if on a terminal.
endSubTests () {
	test -t 9 2>/dev/null || return 0
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
# value.  The valgrind output is sent to file descriptor 8.
runWithValgrind () {
	cat > "${workDir}/valgrind.suppressions" <<EOF
{
   ignore-initproctitle-leak
   Memcheck:Leak
   fun:malloc
   src:proctitle.c
   fun:main
}
EOF
	valgrind --tool=memcheck \
	  --verbose \
	  --show-error-list=yes \
	  --suppressions="${workDir}/valgrind.suppressions" \
	  --log-file="${workDir}/valgrind.out" \
	  --error-exitcode=125 \
	  --track-fds=yes \
	  --leak-check=full \
	  "${testSubject}" "$@" \
	  8<&- 9<&-
	returnValue=$?

	{
	printf "%s\n" "================================================"
	printf "%s\n\n" "Command: ${testSubject} $*"
	cat "${workDir}/valgrind.out"
	printf "%s\n\n" "================================================"
	} >&8

	if test "${returnValue}" -eq 125; then
		printf "%s\n" "memory check failed - see details above." >&8
		exit 1
	fi

	return "${returnValue}"
}

# Run the test with the given basename (such as "001_Basic.test") in place
# of this one, under valgrind, with file descriptor 8 opened as a copy of
# stdout so valgrind's output can be sent to it independently of any
# redirection being performed on the test subject.
runOtherTestUnderValgrind () {
	useValgrind='1'
	orig_f="$f"
	export useValgrind orig_f
	f="${f%/*}/$1"
	exec sh "${sourcePath}/$f" 8>&1
	exit 1
}

# Output message $* and set exitStatus to 1, preceding the message with the
# contents of "expectedOutput" under workDir if the file exists, and of
# "testOutput" if that exists and isn't empty, and of "stderr" if it exists
# and isn't empty.
#
# If the function "failureAdditionalInfo" exists, it is called just before
# the final message line.
#
# The message line is prefixed with the current sub-test number, if any
# startSubTest has been called at any point.
reportFailure () {
	if test -e "${workDir}/expectedOutput" \
	  && test -e "${workDir}/testOutput" \
	  && command -v diff >/dev/null 2>&1 \
	  && ! cmp -s "${workDir}/expectedOutput" "${workDir}/testOutput" \
	; then
		diff -u "${workDir}/expectedOutput" "${workDir}/testOutput"
	else
		test -e "${workDir}/expectedOutput" \
		&& { printf "%s\n" 'expected:'; sed 's,^,> ,' "${workDir}/expectedOutput"; }
		test -s "${workDir}/testOutput" \
		&& { printf "%s\n" 'test output:'; sed 's,^,> ,' "${workDir}/testOutput"; }
	fi
	test -s "${workDir}/stderr" && { printf "%s\n" 'stderr:'; sed 's,^,> ,' "${workDir}/stderr"; }
	type failureAdditionalInfo >/dev/null 2>&1 && failureAdditionalInfo
	test -n "${thisSubTest}" && printf '(%s %d): ' 'sub-test' "${thisSubTest}"
	printf "%s\n" "$*"
	exitStatus=1
}

# Like reportFailure but exit the script immediately afterwards.
fatalError () {
	reportFailure "$*"
	exit 1
}

# Perform cleanups on exit.
testCleanup () {
	# Tidy up the display if any sub-tests ran.
	test -n "${thisSubTest}" && endSubTests
	# If a tmux server was started, terminate it.
	test -n "${tmuxTestSocket}" && test -e "${tmuxTestSocket}" && printf '%s\n' "kill-server" >> "${tmuxTestSocket}"
	# Run any optional cleanup the test script has defined.
	type onExit >/dev/null 2>&1 && onExit
}
