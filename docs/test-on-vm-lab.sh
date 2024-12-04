#!/bin/sh
#
# Run "make check" from a source tarball, on a whole lab of VMs.  Hostnames
# need to be listed on the command line after the tarball, or in
# ~/.config/lab-hosts.  They need to be reachable over SSH with no password.
#
# Displays progress to the terminal and produces a tarball of results.

labHosts="$(cat ~/.config/lab-hosts)"

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
remoteBuildDir="test-${dateStamp}"
resultsArchive="result-${dateStamp}.tar.gz"

startEpoch="$(date +%s)"

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
	  -s Command="$*" \
	  run item
}

for remoteHost in ${hostList}; do
	mkdir "${workDir}/${remoteHost}"
	(
	flock -x 3
	(
	cat > "${workDir}/${remoteHost}/run.sh" <<-EOF
	printf '%s %s\n' 'notice' 'creating build area' >&3
	ssh "${remoteHost}" mkdir "${remoteBuildDir}" || exit 1
	printf '%s %s\n' 'ok' 'created build area' >&3

	printf '%s %s\n' 'notice' 'copying source archive' >&3
	scp "${sourceArchive}" "${remoteHost}:${remoteBuildDir}/" || exit 1
	printf '%s %s\n' 'ok' 'source archive copied' >&3

	printf '%s %s\n' 'notice' 'extracting source archive' >&3
	ssh "${remoteHost}" tar xzf "${remoteBuildDir}/${sourceArchive##*/}" -C "${remoteBuildDir}" --strip-components=1 || exit \$?
	printf '%s %s\n' 'ok' 'source archive extracted' >&3

	printf '%s %s\n' 'notice' 'configuring' >&3
	ssh "${remoteHost}" "cd \"${remoteBuildDir}\" && mkdir BUILD && cd BUILD && ../configure" || exit \$?
	printf '%s %s\n' 'ok' 'configuration completed' >&3

	printf '%s %s\n' 'notice' 'building' >&3
	ssh "${remoteHost}" "cd \"${remoteBuildDir}/BUILD\" && make" || exit \$?
	printf '%s %s\n' 'ok' 'build completed' >&3

	printf '%s %s\n' 'notice' 'testing' >&3
	ssh "${remoteHost}" "cd \"${remoteBuildDir}/BUILD\" && make checkcc" || exit \$?
	printf '%s %s\n' 'ok' 'testing completed' >&3

	exit 0
EOF
	chmod 700 "${workDir}/${remoteHost}/run.sh"

	recordCommand "${workDir}/${remoteHost}" "${workDir}/${remoteHost}/run.sh"
	) 3<&-

	exec >>"${workDir}/${remoteHost}/output.log" 2>&1

	printf '*** %s\n' 'retrieving build and test artefacts'
	scp "${remoteHost}:${remoteBuildDir}/BUILD/config.log" "${workDir}/${remoteHost}/"
	scp "${remoteHost}:${remoteBuildDir}/BUILD/test-suite.log" "${workDir}/${remoteHost}/"
	rsync -a "${remoteHost}:${remoteBuildDir}/BUILD/tests/" "${workDir}/${remoteHost}/tests/"

	printf '*** %s\n' 'removing build area'
	ssh "${remoteHost}" rm -rf "${remoteBuildDir}" || exit 1

	flock -u 3
	) > "${workDir}/${remoteHost}/framework-output.log" 2>&1 3>>"${workDir}/${remoteHost}/active" &
done

# TODO: spawn a cross-compilation check as well

sleep 0.1

allTestHosts="$(find "${workDir}" -mindepth 1 -maxdepth 1 -name "[0-9A-Za-z_]*" -type d -printf "%f\n" | sort)"

hostCount=0
for testHost in ${allTestHosts}; do hostCount=$((1+hostCount)); done
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
	for testHost in ${allTestHosts}; do
		test "${#testHost}" -gt "${nameWidth}" && nameWidth="${#testHost}"

		exec 3>>"${workDir}/${testHost}/active"
		flock -x -n 3 && exitedCount=$((1+exitedCount))
		exec 3<&-

		test -e "${workDir}/${testHost}/ended" || runningCount=$((1+runningCount))
		if test -e "${workDir}/${testHost}/failed"; then
			overallStatus="FAIL"
			failedCount=$((1+failedCount))
		elif test -e "${workDir}/${testHost}/succeeded"; then
			test -z "${overallStatus}" && overallStatus="PASS"
			passedCount=$((1+passedCount))
		fi
	done
	test -n "${overallStatus}" || overallStatus="----"

	printf "%sT+%04d - %s%s\n\n" "${attrUnderline}" "${elapsedSeconds}" "$(date '+%Y-%m-%d %H:%M:%S')" "${attrNone}"

	for testHost in ${allTestHosts}; do
		lastStatusWord="-"
		lastStatusMessage="-"
		test -s "${workDir}/${testHost}/last-status" && read -r lastStatusWord lastStatusMessage < "${workDir}/${testHost}/last-status"
		lastLine="-"
		test -s "${workDir}/${testHost}/output.log" && lastLine="$(tail -n 1 "${workDir}/${testHost}/output.log")"
		testResult="----"
		test -e "${workDir}/${testHost}/failed" && testResult="FAIL"
		test -e "${workDir}/${testHost}/succeeded" && testResult="PASS"
		remainingWidth=$((width-nameWidth-20))
		statusWidth=$((remainingWidth*2/3))
		test "${statusWidth}" -gt 40 && statusWidth=40
		lastLineWidth=$((remainingWidth-statusWidth))

		printf " %s%${nameWidth}s%s " "${attrBold}${attrCyan}" "${testHost}" "${attrNone}"
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

	printf "    %s%s%s " "${attrBold}" "Running+Passed+Failed:" "${attrNone}"
	printf "%s%d%s" "${attrBold}${attrMagenta}" "${runningCount}" "${attrNone}"
	printf "+%s%d%s" "${attrBold}${attrGreen}" "${passedCount}" "${attrNone}"
	printf "+%s%d%s" "${attrBold}${attrRed}" "${failedCount}" "${attrNone}"
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
