#!/bin/sh
#
# Function to launch the test subject under valgrind.
#
# If valgrind is unavailable, exits the script with status 77, after writing
# a note to file descriptor 4.
#
# If valgrind finds an error, writes the error to "valgrind.out" in the
# current directory, and exits the script with status 1 after writing a note
# to file descriptor 4.
#
# If valgrind does not find any errors, the function returns with the exit
# status of the test subject.
#
# Source this file from test scripts that use valgrind.
#
# Requires ${testSubject} and ${workFile4}.  This means that the caller must
# not use file ${workFile4}, as this function will overwrite it.
#

# Output file for failures.
valgrindOutputFile="valgrind.out"

true "${testSubject:?not set - call this from 'make check'}"
true "${workFile4:?not set - call this from 'make check'}"
true "${workFile5:?not set - call this from 'make check'}"

if ! command -v valgrind >/dev/null 2>&1; then
	echo "test requires \`valgrind'"
	exit 77
fi

if test "${SKIP_VALGRIND_TESTS}" = "1"; then
	echo "SKIP_VALGRIND_TESTS is set"
	exit 77
fi

valgrindHelp="$(valgrind --help 2>&1)"
for valgrindOption in "verbose" "show-error-list" "error-exitcode" "track-fds" "leak-check"; do
	echo "${valgrindHelp}" | grep -Fq "${valgrindOption}" || { echo "test requires \`valgrind --${valgrindOption}'"; exit 77; }
done

runWithValgrind () {

	cat > "${workFile5}" <<EOF
{
   ignore-initproctitle-leak
   Memcheck:Leak
   fun:malloc
   fun:initproctitle
   fun:main
}
EOF

	true > "${workFile4}"
	valgrind --tool=memcheck \
	  --verbose \
	  --show-error-list=yes \
	  --suppressions="${workFile5}" \
	  --log-file="${workFile4}" \
	  --error-exitcode=125 \
	  --track-fds=yes \
	  --leak-check=full \
	  "${testSubject}" "$@" \
	  4<&- 9<&-

	returnValue=$?

	if test "${returnValue}" -eq 125; then
		{
		echo "================================================"
		date
		echo "Command: ${testSubject} $*"
		echo
		cat "${workFile4}"
		echo "================================================"
		echo
		} >> "${valgrindOutputFile}"
		echo "memory check failed - see file \`valgrind.out'." 1>&4
		exit 1
	fi

	return "${returnValue}"
}
