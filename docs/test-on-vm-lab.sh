#!/bin/sh
#
# Run "make check" from a source tarball, on a whole lab of VMs.
#
# The source tarball should contain a directory; this directory must contain
# a "configure" script.  After running "configure", this script calls "make"
# and then "make check", from a build subdirectory.
#
# Hostnames need to be listed on the command line after the tarball, or in
# ~/.config/lab-hosts.  They need to be reachable over SSH with no password.
#
# Cross-compilation is performed if the hostname is of the form "x:y", where
# "x" is a target like "arm-linux-gnueabi", and "y" is the name of a host of
# that architecture.  The package is built locally with "--host=x" and the
# result is copied to "y" and "make check" is run.
#
# Displays progress to the terminal and produces a tarball of results.  In
# the results, the file "latest-status.txt" contains the last status
# display, and there is a directory for each host.  The directories contain
# SCW metrics files plus "output.log" (timestamped), "raw-output.log"
# (without timestamps), "framework-output.log" (messages from this script),
# and the "config.log", "test-suite.log", and "tests/" from the build area.
#
# Requires "scw" (https://www.ivarch.com/programs/scw.shtml).
#
# Copyright 2024-2025 Andrew Wood
# License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
#
# Version: 0.0.1 / 16 Dec 2024

# Number of tests to run in parallel on each host.
concurrency="1"

# Run a build and check, according to these environment variables:
#
#  sourceArchive       local source tarball path
#  buildHost           where to build, or "" for local
#  checkHost           where to test, or "" for local
#  remoteBuildDir      temp directory for remote build or check
#  localBuildDir       temp directory for local build
#  configureArguments  extra "configure" script arguments
#
# Does not return - always exits.
#
# Expects to be run under SCW, so writes status to fd 3.
#
testRunner () {
	printf '%s %s\n' 'notice' 'creating build area' >&3
	if test -n "${buildHost}"; then
		ssh "${buildHost}" mkdir "${remoteBuildDir}" || exit 1
	else
		mkdir "${localBuildDir}" || exit 1
	fi
	printf '%s %s\n' 'ok' 'created build area' >&3

	if test -n "${checkHost}"; then
		printf '%s %s\n' 'notice' 'creating check area' >&3
		ssh "${checkHost}" mkdir "${remoteBuildDir}" || exit 1
		printf '%s %s\n' 'ok' 'created check area' >&3
	fi

	printf '%s %s\n' 'notice' 'copying source archive' >&3
	if test -n "${buildHost}"; then
		scp "${sourceArchive}" "${buildHost}:${remoteBuildDir}/" || exit 1
	elif test -n "${checkHost}"; then
		scp "${sourceArchive}" "${checkHost}:${remoteBuildDir}/" || exit 1
	fi
	printf '%s %s\n' 'ok' 'source archive copied' >&3

	printf '%s %s\n' 'notice' 'extracting source archive' >&3
	if test -n "${buildHost}"; then
		ssh "${buildHost}" tar xzf "${remoteBuildDir}/${sourceArchive##*/}" -C "${remoteBuildDir}" || exit $?
	else
		tar xzf "${sourceArchive}" -C "${localBuildDir}" || exit $?
		ssh "${checkHost}" tar xzf "${remoteBuildDir}/${sourceArchive##*/}" -C "${remoteBuildDir}" || exit $?
	fi
	printf '%s %s\n' 'ok' 'source archive extracted' >&3

	stepResult="ok"
	if test -n "${buildHost}"; then
		printf '%s %s\n' 'notice' 'configuring' >&3
		ssh "${buildHost}" "cd \"${remoteBuildDir}\" && mkdir BUILD && cd BUILD && ../*/configure ${configureArguments}" || exit $?
	else
		printf '%s %s\n' 'notice' 'configuring locally' >&3
		( cd "${localBuildDir}" && mkdir BUILD && cd BUILD && ../*/configure ${configureArguments} ) || exit $?
		printf '%s %s\n' 'notice' "configuring on ${checkHost}" >&3
		ssh "${checkHost}" "cd \"${remoteBuildDir}\" && mkdir BUILD && cd BUILD && ../*/configure" \
		|| { stepResult="warning"; printf '%s %s\n' 'warning' "configuration failed on ${checkHost}" >&3; }
	fi
	printf '%s %s\n' "${stepResult}" 'configuration completed' >&3

	printf '%s %s\n' 'notice' 'building' >&3
	if test -n "${buildHost}"; then
		ssh "${buildHost}" "cd \"${remoteBuildDir}/BUILD\" && make -j${concurrency}" || exit $?
	else
		make -j${concurrency} -C "${localBuildDir}/BUILD" || exit $?
	fi
	printf '%s %s\n' 'ok' 'build completed' >&3

	if test -n "${buildHost}"; then
		printf '%s %s\n' 'notice' 'testing' >&3
		ssh "${buildHost}" "cd \"${remoteBuildDir}/BUILD\" && make -j${concurrency} check" || exit $?
		printf '%s %s\n' 'ok' 'testing completed' >&3
	elif test -n "${checkHost}"; then
		printf '%s %s\n' 'notice' "transferring build to ${checkHost}" >&3
		tar cvf "${localBuildDir}/build.tar" -C "${localBuildDir}" BUILD || exit $?
		scp "${localBuildDir}/build.tar" "${checkHost}:${remoteBuildDir}/build.tar" || exit $?
		ssh "${checkHost}" "tar xf \"${remoteBuildDir}/build.tar\" -C \"${remoteBuildDir}\"" || exit $?
		printf '%s %s\n' 'ok' "transferred build to ${checkHost}" >&3
		printf '%s %s\n' 'notice' "testing on ${checkHost}" >&3
		ssh "${checkHost}" "cd \"${remoteBuildDir}/BUILD\" && make -j${concurrency} check-TESTS XCTEST=1" || exit $?
		printf '%s %s\n' 'ok' 'testing completed' >&3
	fi

	exit 0
}


# Using directory $1 for metrics and output, run the remaining arguments as
# a command using "scw" to capture metrics and record logs.
#
recordCommand () {
	targetDir="$1"
	shift
	mkdir -p "${targetDir}"
	scw -c /dev/null \
	  -s UserConfigFile=/dev/null \
	  -s ItemsDir=/dev/null \
	  -s MetricsDir="${targetDir}" \
	  -s CheckLockFile="${targetDir}/.widelock" \
	  -s OutputMap= \
	  -s OutputMap="OES stamped ${targetDir}/output.log" \
	  -s OutputMap="OES raw ${targetDir}/raw-output.log" \
	  -s Command="$*" \
	  run item
}

if test "$1" = "--testRunner"; then
	testRunner
	exit 1
fi

labHosts="$(cat ~/.config/lab-hosts 2>/dev/null)"

workDir=$(mktemp -d) || exit 1
trap 'rm -rf "${workDir}"' EXIT

sourceArchive="$1"
shift
hostList="$*"
test -n "${hostList}" || hostList="${labHosts}"

test -n "${sourceArchive}" || { echo "Usage: ${0##*/} TARBALL [HOST...]"; exit 1; }
test -s "${sourceArchive}" || { echo "${sourceArchive}: not found"; exit 1; }

attrBold="$(tput bold 2>/dev/null)"
attrUnderline="$(tput smul 2>/dev/null)"
attrRed="$(tput setaf 1 2>/dev/null)"
attrGreen="$(tput setaf 2 2>/dev/null)"
attrYellow="$(tput setaf 3 2>/dev/null)"
attrBlue="$(tput setaf 4 2>/dev/null)"
attrMagenta="$(tput setaf 5 2>/dev/null)"
attrCyan="$(tput setaf 6 2>/dev/null)"
attrWhite="$(tput setaf 7 2>/dev/null)"
attrNone="$(tput sgr0 2>/dev/null)"

dateStamp="$(date +%Y%m%d-%H%M)"
resultsArchive="result-${dateStamp}.tar.gz"

startEpoch="$(date +%s)"


for hostSpec in ${hostList}; do
	localTestDir="${workDir}/${hostSpec}"
	remoteBuildDir="test-${dateStamp}.$(date +%s).$$"
	buildHost="${hostSpec}"
	checkHost=""
	configureArguments=""

	xcFor="${hostSpec%:*}"
	xcRunOn="${hostSpec#*:}"

	if ! test "${xcFor}" = "${xcRunOn}"; then
		localTestDir="${workDir}/${xcFor}-${xcRunOn}"
		buildHost=""
		checkHost="${xcRunOn}"
		configureArguments="--host ${xcFor}"
	fi

	mkdir "${localTestDir}"
	printf "%s\n" "${hostSpec}" > "${localTestDir}/hostSpec"

	localBuildDir="${localTestDir}/XC"

	(
	flock -x 3

	export sourceArchive buildHost checkHost remoteBuildDir localBuildDir configureArguments
	recordCommand "${localTestDir}" "sh \"$0\" --testRunner" 3<&-

	exec >>"${localTestDir}/output.log" 2>&1

	printf '*** %s\n' 'retrieving build and test artefacts'
	mkdir "${localTestDir}/tests"
	if test -n "${buildHost}"; then
		scp "${buildHost}:${remoteBuildDir}/BUILD/config.log" "${localTestDir}/"
		scp "${buildHost}:${remoteBuildDir}/BUILD/test-suite.log" "${localTestDir}/"
		scp "${buildHost}:${remoteBuildDir}/BUILD/tests/*" "${localTestDir}/tests/"
	elif test -n "${checkHost}"; then
		scp "${checkHost}:${remoteBuildDir}/BUILD/config.log" "${localTestDir}/"
		scp "${checkHost}:${remoteBuildDir}/BUILD/test-suite.log" "${localTestDir}/"
		scp "${checkHost}:${remoteBuildDir}/BUILD/tests/*" "${localTestDir}/tests/"
	fi

	printf '*** %s\n' 'removing build area'
	rm -rf "${localBuildDir}"
	if test -n "${buildHost}"; then
		ssh "${buildHost}" rm -rf "${remoteBuildDir}"
	elif test -n "${checkHost}"; then
		ssh "${checkHost}" rm -rf "${remoteBuildDir}"
	fi

	printf '\n'

	flock -u 3
	) > "${localTestDir}/framework-output.log" 2>&1 3>>"${localTestDir}/active" &
done

sleep 0.1

allTestHostDirs="$(find "${workDir}" -mindepth 1 -maxdepth 1 -name "[0-9A-Za-z_]*" -type d -printf "%f\n" | sort)"

hostCount=0
for testHostDir in ${allTestHostDirs}; do hostCount=$((1+hostCount)); done
exitedCount=0
overallStatus=""

while test "${exitedCount}" -lt "${hostCount}"; do
	width="$(tput cols 2>/dev/null)"
	test -n "${width}" || width=80

	currentEpoch="$(date +%s)"
	elapsedSeconds=$((currentEpoch-startEpoch))

	{
	nameWidth=0
	runningCount=0
	failedCount=0
	passedCount=0
	exitedCount=0
	overallStatus=""
	for testHostDir in ${allTestHostDirs}; do
		hostSpec="${testHostDir}"
		{ read -r hostSpec < "${workDir}/${testHostDir}/hostSpec"; } 2>/dev/null
		test "${#hostSpec}" -gt "${nameWidth}" && nameWidth="${#hostSpec}"

		exec 3>>"${workDir}/${testHostDir}/active"
		flock -x -n 3 && exitedCount=$((1+exitedCount))
		exec 3<&-

		test -e "${workDir}/${testHostDir}/ended" || runningCount=$((1+runningCount))
		if test -e "${workDir}/${testHostDir}/failed"; then
			overallStatus="FAIL"
			failedCount=$((1+failedCount))
		elif test -e "${workDir}/${testHostDir}/succeeded"; then
			test -z "${overallStatus}" && overallStatus="PASS"
			passedCount=$((1+passedCount))
		fi
	done
	test -n "${overallStatus}" || overallStatus="----"

	printf "%sT+%04d - %s%s\n\n" "${attrUnderline}" "${elapsedSeconds}" "$(date '+%Y-%m-%d %H:%M:%S')" "${attrNone}"

	for testHostDir in ${allTestHostDirs}; do
		hostSpec="${testHostDir}"
		{ read -r hostSpec < "${workDir}/${testHostDir}/hostSpec"; } 2>/dev/null

		lastStatusWord="-"
		lastStatusMessage="-"
		{ read -r lastStatusWord lastStatusMessage < "${workDir}/${testHostDir}/last-status"; } 2>/dev/null

		lastLine="-"
		test -s "${workDir}/${testHostDir}/output.log" && lastLine="$(tail -n 1 "${workDir}/${testHostDir}/output.log" 2>/dev/null | expand)"

		testResult="----"
		test -e "${workDir}/${testHostDir}/failed" && testResult="FAIL"
		test -e "${workDir}/${testHostDir}/succeeded" && testResult="PASS"

		remainingWidth=$((width-nameWidth-20))
		statusWidth=$((remainingWidth*2/3))
		test "${statusWidth}" -gt 40 && statusWidth=40
		lastLineWidth=$((remainingWidth-statusWidth))

		printf " %s%${nameWidth}s%s " "${attrBold}${attrYellow}" "${hostSpec}" "${attrNone}"
		case "${testResult}" in
		"PASS") printf "%s%s%s" "${attrBold}${attrGreen}" "PASS" "${attrNone}" ;;
		"FAIL") printf "%s%s%s" "${attrBold}${attrRed}" "FAIL" "${attrNone}" ;;
		*) printf "%s" "----"
		esac

		case "${lastStatusWord}" in
		"(begin)"|"begin")	printf " (%s%s%s)  " "${attrBold}${attrMagenta}" "begin" "${attrNone}" ;;
		"(end)"|"end")		printf " (%s%s%s)    " "${attrBold}${attrMagenta}" "end" "${attrNone}" ;;
		"(notice)"|"notice")	printf " (%s%s%s) " "${attrBold}${attrCyan}" "notice" "${attrNone}" ;;
		"(ok)"|"ok")		printf " (%s%s%s)     " "${attrBold}${attrGreen}" "ok" "${attrNone}" ;;
		"(warning)"|"warning")	printf " (%s%s%s)" "${attrBold}${attrYellow}" "warning" "${attrNone}" ;;
		"(error)"|"error")	printf " (%s%s%s)  " "${attrBold}${attrRed}" "error" "${attrNone}" ;;
		*)			printf "  %7s " ""; lastStatusMessage="${lastStatusWord} ${lastStatusMessage}" ;;
		esac
		printf " %s%-${statusWidth}.${statusWidth}s%s" "${attrWhite}" "${lastStatusMessage}" "${attrNone}"
		printf " %s%-${lastLineWidth}.${lastLineWidth}s%s" "${attrBlue}" "${lastLine}" "${attrNone}"
		printf "\n"
	done

	printf "\n%s%s%s " "${attrBold}" "Overall status:" "${attrNone}"
	case "${overallStatus}" in
	"PASS") printf "%s%s%s" "${attrBold}${attrGreen}" "PASS" "${attrNone}" ;;
	"FAIL") printf "%s%s%s" "${attrBold}${attrRed}" "FAIL" "${attrNone}" ;;
	*) printf "%s" "----"
	esac

	printf "    %s%s%s " "${attrBold}" "Running,Passed,Failed:" "${attrNone}"
	printf "%sR%d%s" "${attrBold}${attrMagenta}" "${runningCount}" "${attrNone}"
	printf ",%sP%d%s" "${attrBold}${attrGreen}" "${passedCount}" "${attrNone}"
	printf ",%sF%d%s" "${attrBold}${attrRed}" "${failedCount}" "${attrNone}"
	printf " = %d" "${hostCount}"
	printf "\n\n"
	} > "${workDir}/latest-status.txt"

	cat "${workDir}/latest-status.txt"
	sleep 1
done

endEpoch="$(date +%s)"
elapsedSeconds=$((endEpoch-startEpoch))

printf '%s\n' 'Generating results archive'
tar czf "${resultsArchive}" -C "${workDir}" .

printf '%s: %d%s\n' 'Total time taken' "${elapsedSeconds}" "s"
printf '%s: %s\n' 'Results archive' "${resultsArchive}"

test "${overallStatus}" = "PASS" || exit 1
