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

pv="$1"
test -n "${pv}" || pv='pv'

rounds='10'		# how many rounds of measurements to take
testFileMB='256'	# max size of each of the test files, in MiB
testZeroesMB='1024'	# amount of /dev/zero data to use, in MiB

# Use /dev/shm for workspace if possible to eliminate disk I/O as a factor.
if test -d /dev/shm && mountpoint -q /dev/shm; then
	export TMPDIR=/dev/shm
fi
# Check there's enough room for the test files - make them smaller, if not.
tmpSpaceMB="$(df -kP "${TMPDIR:-/tmp}" | awk 'FNR==2 {print int($4/1024)}')"
while test ${testFileMB} -gt 4; do
	test "${tmpSpaceMB}" -gt $((2+3*testFileMB)) && break
	testFileMB=$((testFileMB/2))
done

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

# Temporary working area, cleaned up on exit.
workDir="$(mktemp -d)" || exit 1
trap 'rm -rf "${workDir}"' EXIT

# Set everything to the "C" locale.
LANG=C
LC_ALL=C
export LANG LC_ALL

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

# Run various of transfer types with extra options "$2", naming them "$1".
runTransfers () {
	# From file via stdin to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "${pv} -q $2 < ${workDir}/file1 > ${workDir}/stdout"
	resultsLine "$1: stdin file to file"

	# From file to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "${pv} -q $2 ${workDir}/file1 > ${workDir}/stdout"
	resultsLine "$1: file to file"

	# From two files to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes $((2*testFileMB)) "${pv} -q $2 ${workDir}/file1 ${workDir}/file2 > ${workDir}/stdout"
	resultsLine "$1: two files to file"

	# From pipe to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "cat ${workDir}/file1 | ${pv} -q $2 > ${workDir}/stdout"
	resultsLine "$1: pipe to file"

	# From file via stdin to pipe.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "${pv} -q $2 < ${workDir}/file1 | cat > ${workDir}/stdout"
	resultsLine "$1: stdin file to pipe"

	# From file to pipe.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "${pv} -q $2 ${workDir}/file1 | cat > ${workDir}/stdout"
	resultsLine "$1: file to pipe"

	# From two files to pipe.
	rm -f "${workDir}/stdout"
	captureTimes $((2*testFileMB)) "${pv} -q $2 ${workDir}/file1 ${workDir}/file2 | cat > ${workDir}/stdout"
	resultsLine "$1: two files to pipe"

	# From pipe to pipe.
	rm -f "${workDir}/stdout"
	captureTimes $((testFileMB)) "cat ${workDir}/file1 | ${pv} -q $2 | cat > ${workDir}/stdout"
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
		captureTimes $((testZeroesMB)) "${pv} -q -Ss ${testZeroesMB}M /dev/zero > /dev/null"
		resultsLine "Zeroes: stdout to /dev/null"
		captureTimes $((testZeroesMB)) "${pv} -q -Ss ${testZeroesMB}M /dev/zero | cat > /dev/null"
		resultsLine "Zeroes: stdout to pipe"
		if grep -Fq ' -X' "${workDir}/help"; then
			captureTimes $((testZeroesMB)) "${pv} -q -X -Ss ${testZeroesMB}M /dev/zero"
			resultsLine "Zeroes: discarded"
		fi
		if grep -Fq ' -C' "${workDir}/help"; then
			captureTimes $((testZeroesMB)) "${pv} -q -C -Ss ${testZeroesMB}M /dev/zero > /dev/null"
			resultsLine "Zeroes: stdout to /dev/null with no-splice"
			captureTimes $((testZeroesMB)) "${pv} -q -C -Ss ${testZeroesMB}M /dev/zero | cat > /dev/null"
			resultsLine "Zeroes: stdout to pipe with no-splice"
		fi
		if grep -Fq ' -C' "${workDir}/help" && grep -Fq ' -X' "${workDir}/help"; then
			captureTimes $((testZeroesMB)) "${pv} -q -C -X -Ss ${testZeroesMB}M /dev/zero"
			resultsLine "Zeroes: discarded with no-splice"
		fi
	fi
}

# Run several rounds of measurements.
outputLine '#' 'ID' 'MiB/sec' 'Real time' 'User CPU time' 'System CPU time' 'Raw measurement'
thisRound=0
while test ${thisRound} -lt ${rounds}; do
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
	awk -F "\t" -v "h=${measurementHash}" -v fieldcount=5 \
'BEGIN { samples=0 }
{ m=$1; samples++; for (field=1; field<=fieldcount; field++) { total[field] += $(1+field) } }
END { printf "%s\t%s", "μ", h; for (field=1; field<=fieldcount; field++) { printf "\t%.3f", total[field]/samples }; printf "\t%s\n", m }' \
	< "${workDir}/measurements" > "${workDir}/mean"
	# Calculate the standard deviation of each field.
	cat "${workDir}/mean" "${workDir}/measurements" \
	| awk -F "\t" -v "h=${measurementHash}" -v "m=${measurement}" -v fieldcount=5 \
'BEGIN { samples=0 }
FNR==1 { for (field=1; field<=fieldcount; field++) { mean[field] += $(2+field) } }
FNR>1 { samples++; for (field=1; field<=fieldcount; field++) { variance=$(1+field)-mean[field]; sum_variance_squared[field] += (variance*variance) } }
END { printf "%s\t%s", "σ", h; for (field=1; field<=fieldcount; field++) { printf "\t%.3f", sqrt(sum_variance_squared[field]/samples) }; printf "\t%s\n", m }' \
	> "${workDir}/stddev"
	sed "s!^!${sysId}\t${pvId}\t${runId}\t!" "${workDir}/mean" "${workDir}/stddev"
done
} < "${workDir}/measurement-types"
