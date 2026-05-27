#!/bin/bash
#
# Measure transfer rates and CPU usage with various different options.

pv="$1"
test -n "${pv}" || pv='pv'

# Function to write a stats line - a system ID, current time, and 5
# parameters, separated by TAB characters.
# If ${workDir}/times exists, use the "real", "user", "sys" value from it as
# the last 3 parameters.
statsLine () {
	test -n "${sysId}" || sysId="$({ uname -a; ${pv} -V; } | md5sum | awk '{print $1}')"
	if test -s "${workDir}/times"; then
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${sysId}" "$(date +%Y-%m-%dT%H:%M:%S)" "$1" "$2" \
		  "$(awk '$1=="real" {print $2}' "${workDir}/times")" \
		  "$(awk '$1=="user" {print $2}' "${workDir}/times")" \
		  "$(awk '$1=="sys" {print $2}' "${workDir}/times")"
	else
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${sysId}" "$(date +%Y-%m-%dT%H:%M:%S)" "$1" "$2" "$3" "$4" "$5"
	fi
}

# Run $1 in a shell under "time -p", writing $1's stderr to
# ${workDir}/stderr, and the times to ${workDir}/times.
captureTimes () {
	{ time -p sh -c "{ $1; } 2>${workDir}/stderr"; } 2>"${workDir}/times"
}

# Temporary working area, cleaned up on exit.
workDir="$(mktemp -d)" || exit 1
trap 'rm -rf "${workDir}"' EXIT

# Set everything to the "C" locale.
LANG=C
LC_ALL=C
export LANG LC_ALL

# Basic system information, and a header line for the test results.
statsLine 'System hostname' "$(uname -n)" '' '' ''
statsLine 'System load' "$(uptime | awk '{printf "%.2f\n",$(NF-2)}')" '' '' ''
statsLine 'System kernel type' "$(uname -s)" '' '' ''
statsLine 'System kernel release' "$(uname -r)" '' '' ''
statsLine 'System OS' "$(uname -o)" '' '' ''
statsLine 'Test PV path' "${pv}" '' '' ''
statsLine 'Test PV version' "$(${pv} -V | awk 'FNR==1 {print $NF}')" '' '' ''
statsLine 'Test' 'Rate' 'Real time' 'User CPU time' 'System CPU time'

# Generate some data.
dd if=/dev/urandom of="${workDir}/file1" bs=1048576 count=512 2>/dev/null

# Run various of transfer types with extra options "$2", naming them "$1".
runTransfers () {
	# From file via stdin to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes "${pv} -nr$2 < ${workDir}/file1 > ${workDir}/stdout"
	statsLine "$1: stdin file to file" "$(sed -n '$p' "${workDir}/stderr")"

	# From file to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes "${pv} -nr$2 ${workDir}/file1 > ${workDir}/stdout"
	statsLine "$1: file to file" "$(sed -n '$p' "${workDir}/stderr")"

	# From two files to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes "${pv} -nr$2 ${workDir}/file1 ${workDir}/file2 > ${workDir}/stdout"
	statsLine "$1: two files to file" "$(sed -n '$p' "${workDir}/stderr")"

	# From pipe to file via stdout.
	rm -f "${workDir}/stdout"
	captureTimes "cat ${workDir}/file1 | ${pv} -nr$2 > ${workDir}/stdout"
	statsLine "$1: pipe to file" "$(sed -n '$p' "${workDir}/stderr")"

	# From file via stdin to pipe.
	rm -f "${workDir}/stdout"
	captureTimes "${pv} -nr$2 < ${workDir}/file1 | cat > ${workDir}/stdout"
	statsLine "$1: stdin file to pipe" "$(sed -n '$p' "${workDir}/stderr")"

	# From file to pipe.
	rm -f "${workDir}/stdout"
	captureTimes "${pv} -nr$2 ${workDir}/file1 | cat > ${workDir}/stdout"
	statsLine "$1: file to pipe" "$(sed -n '$p' "${workDir}/stderr")"

	# From two files to pipe.
	rm -f "${workDir}/stdout"
	captureTimes "${pv} -nr$2 ${workDir}/file1 ${workDir}/file2 | cat > ${workDir}/stdout"
	statsLine "$1: two files to pipe" "$(sed -n '$p' "${workDir}/stderr")"

	# From pipe to pipe.
	rm -f "${workDir}/stdout"
	captureTimes "cat ${workDir}/file1 | ${pv} -nr$2 | cat > ${workDir}/stdout"
	statsLine "$1: pipe to pipe" "$(sed -n '$p' "${workDir}/stderr")"
}

runTransfers 'Default' ''
runTransfers 'No-splice' 'C'
runTransfers 'Pipe buffer 1M' ' -J 1M'
runTransfers 'Transfer buffer 1M' ' -B 1M'

runTransfers 'Directed output' " -o ${workDir}/stdout"
runTransfers 'Directed output with no-splice' " -o ${workDir}/stdout -C"
runTransfers 'Directed output with pipe buffer 1M' " -o ${workDir}/stdout -J 1M"
runTransfers 'Directed output with transfer buffer 1M' " -o ${workDir}/stdout -B 1M"

runTransfers 'Discard' 'X'
runTransfers 'Discard with no-splice' 'XC'
runTransfers 'Discard with pipe buffer 1M' 'X -J 1M'
runTransfers 'Discard with transfer buffer 1M' 'X -B 1M'

captureTimes "${pv} -nr -Ss 1G /dev/zero > /dev/null"
statsLine "1GB of zeroes: stdout to /dev/null" "$(sed -n '$p' "${workDir}/stderr")"
captureTimes "${pv} -nr -Ss 1G /dev/zero | cat > /dev/null"
statsLine "1GB of zeroes: stdout to pipe" "$(sed -n '$p' "${workDir}/stderr")"
captureTimes "${pv} -nrX -Ss 1G /dev/zero"
statsLine "1GB of zeroes: discarded" "$(sed -n '$p' "${workDir}/stderr")"
captureTimes "${pv} -nrC -Ss 1G /dev/zero > /dev/null"
statsLine "1GB of zeroes: stdout to /dev/null with no-splice" "$(sed -n '$p' "${workDir}/stderr")"
captureTimes "${pv} -nrC -Ss 1G /dev/zero | cat > /dev/null"
statsLine "1GB of zeroes: stdout to pipe with no-splice" "$(sed -n '$p' "${workDir}/stderr")"
captureTimes "${pv} -nrCX -Ss 1G /dev/zero"
statsLine "1GB of zeroes: discarded with no-splice" "$(sed -n '$p' "${workDir}/stderr")"
