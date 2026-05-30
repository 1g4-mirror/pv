#!/bin/bash
#
# Measure transfer rates and CPU usage with various different options,
# multiple times, then calculate the mean and standard deviation for each
# set of measurements.
#
# The report is written to stdout as tab-separated values, each line
# prefixed with an opaque system ID (based on "uname -a"), the PV version
# expressed as an integer, and a run ID based on the start date and time.
#
# Takes a path to a pv binary as an argument.

# Defaults.
pv='pv'			# pv executable to run the measurements with
rounds='10'		# how many rounds of measurements to take
testFileMB='256'	# max size of each of the test files, in MiB
testZeroesMB='1024'	# amount of /dev/zero data to use, in MiB

# Script information for --help and --version.
programName='benchmark-pv-transfers'
programVersion='0.1.0'
bugReportsTo='https://codeberg.org/ivarch/pv/issues'
copyrightYear='2026'
copyrightHolder='Andrew Wood'

# Constants.
fieldsPerRecord='4'	# measurements taken: rate, time - real, user, sys.

# Write an error message $1 to standard error, prefixed with the program
# name.
error () {
	printf '%s: %s\n' "${programName}" "$1" >&2
}

# Output an error message $1, and exit with a failure status.
die () {
	error "$1"
	exit 1
}

# Write an output line of up to 7 arguments, prefixed with a system ID and
# the current time.
outputLine () {
	test -n "${sysId}" || sysId="$(uname -a | md5sum | cut -b1-7)"
	test -n "${pvId}" || pvId="$(${pv} --version | awk 'FNR==1{print $2}' | awk -F . '{print 1000000*$1+1000*$2+$3}')"
	test -n "${runId}" || runId="$(date '+%Y%m%d%H%M%S')"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${sysId}" "${pvId}" "${runId}" "$1" "$2" "$3" "$4" "$5" "$6" "$7"
}

# Write a line of results for measurement $1 (prefixed with ${thisRound} and
# a hash of $1), reading the times from ${workDir}/times and deriving the
# rate from the elapsed time (from ${workDir}/elapsed, or the real time from
# ${workDir}/times if that's not present) and the size written in
# ${workDir}/size.  Removes those three files in the process.
#
# The results without the prefix are spooled to ${workDir}/results for later
# analysis.
resultsLine () {
	testTimeReal="$(awk '$1=="real" {print $2}' "${workDir}/times")"
	testTimeUser="$(awk '$1=="user" {print $2}' "${workDir}/times")"
	testTimeSystem="$(awk '$1=="sys" {print $2}' "${workDir}/times")"
	testTimeElapsed="$(cat "${workDir}/elapsed" 2>/dev/null)"
	test -n "${testTimeElapsed}" || testTimeElapsed="${testTimeReal}"
	testRate="$(awk -v t="${testTimeReal}" '{if (t>0) { printf "%.3f\n", $1/t } else { print "-" }}' < "${workDir}/size")"
	test -n "${testRate}" || testRate='-'
	rm -f "${workDir}/elapsed" "${workDir}/times" "${workDir}/size"
	outputLine "${thisRound}" "$(printf '%s\n' "$1" | md5sum | cut -b1-7)" "${testRate}" "${testTimeReal}" "${testTimeUser}" "${testTimeSystem}" "$1"
	printf '%s\t%s\t%s\t%s\t%s\n' "$1" "${testRate}" "${testTimeReal}" "${testTimeUser}" "${testTimeSystem}" >> "${workDir}/results"
}

# Run $2 in a shell under "time", writing $1 to ${workDir}/size and the
# times to ${workDir}/times.  If measurable, the elapsed real time is
# written with greater precision to ${workDir}/elapsed, otherwise that file
# is removed.
captureTimes () {
	printf '%s\n' "$1" > "${workDir}/size"
	t0="$(date '+%s.%N' 2>/dev/null)"
	(
	TIMEFORMAT="real %3R
user %3U
sys %3S"
	time sh -c "{ $2; } 2>&3"
	) 3>&2 2>"${workDir}/times"
	t1="$(date '+%s.%N' 2>/dev/null)"
	if test -n "${t0}" && test -n "${t1}"; then
		awk -v "t0=${t0}" -v "t1=${t1}" 'BEGIN{printf "%.6f\n", t1-t0}' < /dev/null > "${workDir}/elapsed"
	else
		rm -f "${workDir}/elapsed"
	fi
}

# Run various of transfer types with extra options "$2", naming them "$1".
runTransfers () {
	# From file via stdin to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "${pv} $2 < ${workDir}/file1 > ${workDir}/stdout"
	resultsLine "$1: stdin file to file"

	# From file to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "${pv} $2 ${workDir}/file1 > ${workDir}/stdout"
	resultsLine "$1: file to file"

	# From two files to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes $((2*testFileMB)) "${pv} $2 ${workDir}/file1 ${workDir}/file2 > ${workDir}/stdout"
	resultsLine "$1: two files to file"

	# From pipe to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "cat ${workDir}/file1 | ${pv} $2 > ${workDir}/stdout"
	resultsLine "$1: pipe to file"

	# From file via stdin to pipe.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "${pv} $2 < ${workDir}/file1 | cat > ${workDir}/stdout"
	resultsLine "$1: stdin file to pipe"

	# From file to pipe.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "${pv} $2 ${workDir}/file1 | cat > ${workDir}/stdout"
	resultsLine "$1: file to pipe"

	# From two files to pipe.
	rm -f "${workDir}/stdout"
	captureTimes $((2*testFileMB)) "${pv} $2 ${workDir}/file1 ${workDir}/file2 | cat > ${workDir}/stdout"
	resultsLine "$1: two files to pipe"

	# From pipe to pipe.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "cat ${workDir}/file1 | ${pv} $2 | cat > ${workDir}/stdout"
	resultsLine "$1: pipe to pipe"
}

# Run the full set of measurements.
gatherMeasurements () {
	runTransfers 'Default' ''
	grep -Fq ' -C' "${workDir}/help" && runTransfers 'No-splice' '-C'
	grep -Fq ' -J' "${workDir}/help" && runTransfers 'Pipe buffer 1M' '-J 1M'
	grep -Fq ' -B' "${workDir}/help" && runTransfers 'Transfer buffer 1M' '-B 1M'

	if grep -Fq ' -o' "${workDir}/help"; then
		runTransfers 'Directed output' " -o ${workDir}/stdout"
		grep -Fq ' -C' "${workDir}/help" && runTransfers 'Directed output with no-splice' " -o ${workDir}/stdout -C"
		grep -Fq ' -J' "${workDir}/help" && runTransfers 'Directed output with pipe buffer 1M' " -o ${workDir}/stdout -J 1M"
		grep -Fq ' -B' "${workDir}/help" && runTransfers 'Directed output with transfer buffer 1M' " -o ${workDir}/stdout -B 1M"
	fi

	if grep -Fq ' -X' "${workDir}/help"; then
		runTransfers 'Discard' '-X'
		grep -Fq ' -C' "${workDir}/help" && runTransfers 'Discard with no-splice' '-X -C'
		grep -Fq ' -J' "${workDir}/help" && runTransfers 'Discard with pipe buffer 1M' '-X -J 1M'
		grep -Fq ' -B' "${workDir}/help" && runTransfers 'Discard with transfer buffer 1M' '-X -B 1M'
	fi

	if grep -Fq ' -S' "${workDir}/help"; then
		captureTimes $((testZeroesMB)) "${pv} -Ss ${testZeroesMB}M /dev/zero > /dev/null"
		resultsLine "Zeroes: stdout to /dev/null"
		captureTimes $((testZeroesMB)) "${pv} -Ss ${testZeroesMB}M /dev/zero | cat > /dev/null"
		resultsLine "Zeroes: stdout to pipe"
		if grep -Fq ' -X' "${workDir}/help"; then
			captureTimes $((testZeroesMB)) "${pv} -X -Ss ${testZeroesMB}M /dev/zero"
			resultsLine "Zeroes: discarded"
		fi
		if grep -Fq ' -C' "${workDir}/help"; then
			captureTimes $((testZeroesMB)) "${pv} -C -Ss ${testZeroesMB}M /dev/zero > /dev/null"
			resultsLine "Zeroes: stdout to /dev/null with no-splice"
			captureTimes $((testZeroesMB)) "${pv} -C -Ss ${testZeroesMB}M /dev/zero | cat > /dev/null"
			resultsLine "Zeroes: stdout to pipe with no-splice"
		fi
		if grep -Fq ' -C' "${workDir}/help" && grep -Fq ' -X' "${workDir}/help"; then
			captureTimes $((testZeroesMB)) "${pv} -C -X -Ss ${testZeroesMB}M /dev/zero"
			resultsLine "Zeroes: discarded with no-splice"
		fi
	fi
}

# Run several rounds of benchmark measurements using $1 as the pv
# executable, producing tab-separated data, including a final section
# containing the means and standard deviations for each measurement type.
#
runBenchmarks () {
	pv="$1"

	# Check there's enough room for the test files - make them smaller,
	# if not.
	tmpSpaceMB="$(df -kP "${TMPDIR:-/tmp}" | awk 'FNR==2 {print int($4/1024)}')"
	while test "${testFileMB}" -gt 4; do
		test "${tmpSpaceMB}" -gt $((2+3*testFileMB)) && break
		testFileMB=$((testFileMB/2))
	done

	# Capture the help text so that capabilities can be checked.
	${pv} -h > "${workDir}/help"

	# Basic system information, and a header line for the test results.
	outputLine 'System hostname' "$(uname -n)"
	outputLine 'System load' "$(uptime | awk '{printf "%.2f\n",$(NF-2)}')"
	outputLine 'System kernel type' "$(uname -s)"
	outputLine 'System kernel release' "$(uname -r)"
	outputLine 'System OS' "$(uname -o)"
	outputLine 'PV path' "${pv}"
	outputLine 'PV version' "$(${pv} -V | awk 'FNR==1 {print $2}')"
	outputLine 'Test file size (MB)' "${testFileMB}"

	# Generate two files of random data.
	dd if='/dev/urandom' of="${workDir}/file1" bs=1048576 count="${testFileMB}" 2>/dev/null
	dd if='/dev/urandom' of="${workDir}/file2" bs=1048576 count="${testFileMB}" 2>/dev/null

	# Run several rounds of measurements.
	outputLine '#' 'ID' 'MiB/sec' 'Real time' 'User CPU time' 'System CPU time' 'Raw measurement'
	thisRound=0
	while test ${thisRound} -lt "${rounds}"; do
		thisRound=$((1+thisRound))
		gatherMeasurements
	done

	# For each of the types of measurement, report the mean and standard
	# deviation of each field.
	awk -F "\t" '{print $1}' < "${workDir}/results" > "${workDir}/measurement-types"
	true > "${workDir}/measurement-types-used"
	outputLine 'μ/σ' 'ID' 'MiB/sec' 'Real time' 'User CPU time' 'System CPU time' 'Aggregated measurement'
	{
	while read -r measurement; do
		# Skip this type of measurement if already processed.
		grep -Fqx "${measurement}" "${workDir}/measurement-types-used" && continue
		printf '%s\n' "${measurement}" >> "${workDir}/measurement-types-used"
		# Hash the measurement name.
		measurementHash="$(printf '%s\n' "${measurement}" | md5sum | cut -b1-7)"
		# Separate out this measurement type's results.
		awk -F "\t" -v "m=${measurement}" '$1==m {print}' < "${workDir}/results" \
		> "${workDir}/measurements"
		# Calculate the mean of each field.
		awk -F "\t" -v "h=${measurementHash}" -v "fieldcount=${fieldsPerRecord}" \
'BEGIN { samples=0 }
{ m=$1; samples++; for (field=1; field<=fieldcount; field++) { total[field] += $(1+field) } }
END { printf "%s\t%s", "μ", h; for (field=1; field<=fieldcount; field++) { printf "\t%.3f", total[field]/samples }; printf "\t%s\n", m }' \
		< "${workDir}/measurements" > "${workDir}/mean"
		# Calculate the standard deviation of each field.
		cat "${workDir}/mean" "${workDir}/measurements" \
		| awk -F "\t" -v "h=${measurementHash}" -v "m=${measurement}" -v "fieldcount=${fieldsPerRecord}" \
'BEGIN { samples=0 }
FNR==1 { for (field=1; field<=fieldcount; field++) { mean[field] += $(2+field) } }
FNR>1 { samples++; for (field=1; field<=fieldcount; field++) { variance=$(1+field)-mean[field]; sum_variance_squared[field] += (variance*variance) } }
END { printf "%s\t%s", "σ", h; for (field=1; field<=fieldcount; field++) { printf "\t%.3f", sqrt(sum_variance_squared[field]/samples) }; printf "\t%s\n", m }' \
		> "${workDir}/stddev"
		sed "s!^!${sysId}\t${pvId}\t${runId}\t!" "${workDir}/mean" "${workDir}/stddev"
	done
	} < "${workDir}/measurement-types"
}

# Read a stream of benchmark data on stdin containing runs from a single
# system, and report how the measurements changed across the different PV
# versions.
compareVersionResults () {
	cat > "${workDir}/raw-system-data"
	# List the measurement IDs in the order they appear in the data.
	awk -F "\t" '$4=="σ"{print $5}' < "${workDir}/raw-system-data" > "${workDir}/measurement-ids"
	# List all PV versions for which any data is available.
	awk -F "\t" '$4=="PV version"{print $2,$5}' "${workDir}/raw-system-data" | sort -nu > "${workDir}/pv-versions"
	# Report on each measurement type in turn.
	true > "${workDir}/measurement-ids-seen"
	{
	while read -r measurementId; do
		# Skip if this measurement was already processed.
		grep -Fqx "${measurementId}" "${workDir}/measurement-ids-seen" && continue
		printf '%s\n' "${measurementId}" >> "${workDir}/measurement-ids-seen"
		# Collect this measurement's mean and standard deviation
		# records for each PV version.  If there's more than one for
		# a single version, average them.
		# TODO: split out collection into a separate function for re-use later
		{
		while read -r pvId pvVersion; do
			awk -F "\t" \
			  -v "pvId=${pvId}" -v "pvVersion=${pvVersion}" \
			  -v "h=${measurementId}" \
			  -v "fieldcount=${fieldsPerRecord}" \
'BEGIN {samples=0}
$2==pvId && $5==h && $4=="μ" { samples++; for (field=1; field<=fieldcount; field++) { mean[field] += $(5+field) } }
$2==pvId && $5==h && $4=="σ" { for (field=1; field<=fieldcount; field++) { stddev[field] += $(5+field) } }
END {
  if (samples > 0) {
    printf "%s", pvVersion
    for (field=1; field<=fieldcount; field++) {
      printf "\t%.3f\t%.3f", mean[field]/samples, stddev[field]/samples
    }
    printf "\n"
  }
}' \
< "${workDir}/raw-system-data"
		done
		} < "${workDir}/pv-versions" > "${workDir}/measurements-per-version"
		# If there are not at least 2 PV versions for which this
		# measurement was available, report nothing as no comparison
		# can be made.
		test "$(grep -c . "${workDir}/measurements-per-version")" -lt 2 && continue
		# Show the measurement name.
		measurementName="$(awk -F "\t" -v "h=${measurementId}" '$4=="σ" && $5==h {print $NF;exit}' "${workDir}/raw-system-data")"
		printf '\n%s\n' "${measurementName}"
		# Report each version's measurements and how they compare to
		# the previous version.
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		  'PV' \
		  'μ:Rate' 'σ:Rate' '±:Rate' \
		  '-' \
		  'μ:tReal' 'σ:tReal' '±:tReal' \
		  '-' \
		  'μ:tUser' 'σ:tUser' '±:tUser' \
		  '-' \
		  'μ:tSys' 'σ:tSys' '±:tSys'
		awk -F "\t" -v "fieldcount=${fieldsPerRecord}" \
'{
  printf "%s", $1
  for (field=0; field<fieldcount; field++) {
    if (field > 0) {
      printf "\t%s", "-";
    }
    mean=$(2+2*field)
    stddev=$(3+2*field)
    changescore=0
    printf "\t%.3f\t%.3f", mean, stddev;
    if (FNR>1) {
      low=mean-stddev;
      high=mean+stddev;
      range=high-low;
      prevlow=pmean[field]-pstddev[field];
      prevhigh=pmean[field]+pstddev[field];
      prevrange=prevhigh-prevlow;
      if (prevrange < 1)
        prevrange = 1;
      if (high > prevhigh) {
        changescore = (high - prevhigh) / prevrange;
      } else if (low < prevlow) {
        changescore = 0 - ((prevlow - low) / prevrange);
      }
    }
    if (changescore < 0.01 && changescore > -0.01) {
      printf "\t%d", 0
    } else {
      printf "\t%s%.2f", (changescore > 0.00 ? "+" : ""), changescore
    }
    pmean[field]=mean;
    pstddev[field]=stddev;
  }
  printf "\n"
}
' "${workDir}/measurements-per-version"
# TODO: pass through something to line up the columns in a more readable way
	done
	} < "${workDir}/measurement-ids"
}

# Read a stream of benchmark data from stdin containing one or more runs
# from one or more systems and multiple PV versions, and for each individual
# system, report how the measurements changed across the different PV
# versions.
runVersionComparisons () {
	cat > "${workDir}/raw-data"
	# List the system IDs in the order they appear in the data.
	awk -F "\t" '{print $1}' < "${workDir}/raw-data" | uniq > "${workDir}/sysids"
	# Report on each system in turn.
	true > "${workDir}/sysids-seen"
	{
	while read -r sysId; do
		# Skip if this system was already processed.
		grep -Fqx "${sysId}" "${workDir}/sysids-seen" && continue
		printf '%s\n' "${sysId}" >> "${workDir}/sysids-seen"
		# Extract the data for just this system.
		awk -F "\t" -v "s=${sysId}" '$1==s {print}' < "${workDir}/raw-data" > "${workDir}/system-data"
		# Report preamble.
		printf '%79s\n' '' | tr ' ' '-'
		printf '%s: %s - %s - %s %s\n' \
		  'System' \
		  "$(awk -F "\t" '$4=="System hostname"{print $5;exit}' "${workDir}/system-data")" \
		  "$(awk -F "\t" '$4=="System OS"{print $5;exit}' "${workDir}/system-data")" \
		  "$(awk -F "\t" '$4=="System kernel type"{print $5;exit}' "${workDir}/system-data")" \
		  "$(awk -F "\t" '$4=="System kernel release"{print $5;exit}' "${workDir}/system-data")"
		compareVersionResults < "${workDir}/system-data"
	done
	} < "${workDir}/sysids"
}

##############################################################################
# Main entry point.

# TODO: options to list available measurements and to run specific ones only

# Process any command-line options.
action='benchmark'
while test -n "$1"; do
	arg="$1"
	shift
	case "${arg}" in
	'benchmark'|'analyse') action="${arg}" ;;
	'-h'|'--help')
		cat - <<EOF
Usage: ${programName} [OPTIONS] [ACTION]
Benchmark pv transfers.

Actions:

  benchmark  - run several rounds of measurements and produce benchmark data
  analyse    - analyse benchmark data on stdin from multiple pv versions

Options:

  -p, --program FILE   benchmark using FILE as the pv executable
  -r, --rounds ROUNDS  run ROUNDS sets of measurements (${rounds})
  -s, --size SIZE      attempt to use a test file of SIZE MiB (${testFileMB})
  -z, --zeroes SIZE    stop at SIZE MiB for /dev/zero measurements (${testZeroesMB})

  -h, --help           show this help
  -V, --version        show script version

Default values are shown in brackets.

Please report any bugs to: ${bugReportsTo}
EOF
		exit 0
		;;
	'-V'|'--version')
		cat - <<EOF
${programName} (pv) ${programVersion}
Copyright ${copyrightYear} ${copyrightHolder}
License: GPLv3+ <https://www.gnu.org/licenses/gpl-3.0.html>
This is free software: you are free to change and redistribute it.
There is NO WARRANTY, to the extent permitted by law.
EOF
		exit 0
		;;
	'-p'|'--program'|'--pv') pv="$1"; test $# -gt 0 && shift ;;
	'--program='*|'--pv='*) pv="${arg#*=}" ;;
	'-r'|'--rounds') rounds="$1"; test $# -gt 0 && shift ;;
	'--rounds='*) rounds="${arg#*=}" ;;
	'-s'|'--size') testFileMB="$1"; test $# -gt 0 && shift ;;
	'--size='*) testFileMB="${arg#*=}" ;;
	'-z'|'--zeroes') testZeroesMB="$1"; test $# -gt 0 && shift ;;
	'--zeroes='*) testZeroesMB="${arg#*=}" ;;
	'-'*) die "${arg}: unknown option - try \`--help'" ;;
	*) die "${arg}: unexpected argument - try \`--help'" ;;
	esac
done

# Use /dev/shm for workspace if possible, to eliminate disk I/O as a factor.
if test -z "${TMPDIR}" && test -d '/dev/shm' && mountpoint -q '/dev/shm'; then
	TMPDIR='/dev/shm'
	export TMPDIR
fi

# Temporary working area, cleaned up on exit.
workDir="$(mktemp -d)" || exit 1
trap 'rm -rf "${workDir}"' EXIT

# Set everything to the "C" locale.
LANG=C
LC_ALL=C
export LANG LC_ALL

# Run the selected action.
case "${action}" in
'benchmark') runBenchmarks "${pv}" ;;
'analyse') runVersionComparisons ;;
esac
