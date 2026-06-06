#!/bin/bash
#
# Benchmark transfer rates, and analyse benchmark results.
#
# Measures transfer rates and CPU usage with various different options,
# multiple times, then calculates the mean and standard deviation for each
# set of measurements.
#
# The results are written to stdout as tab-separated values, each line
# prefixed with an opaque system ID (based on "uname -a"), the PV version
# expressed as an integer, and a run ID based on the start date and time.
#
# In analysis mode, reads results from multiple benchmark runs on standard
# input, and for each type of measurement, show the differences between
# either the different PV versions or the different runs.

# Defaults.
pv='pv'			# PV executable to run the measurements with
rounds='10'		# how many rounds of measurements to take
testFileMB='256'	# max size of each of the test files, in MiB
testZeroesMB='1024'	# amount of /dev/zero data to use, in MiB
sourcesDir=''		# directory containing sources to benchmark
compareWhat='auto'	# what to compare in the analysis
finalLineOnly='false'	# whether to only show the last measurement's analysis
terseFormat='false'	# whether to use a terse report format

# Script information for --help and --version.
programName='benchmark-pv-transfers'
programVersion='1.0.0'
bugReportsTo='https://codeberg.org/ivarch/pv/issues'
copyrightYear='2026'
copyrightHolder='Andrew Wood'

# Constants.
fieldsPerRecord='4'	# measurements taken: rate, time - real, user, sys.

# Hash function - write hex on stdout based on a hash of stdin.
runHash () {
	command -v md5sum >/dev/null 2>&1 && exec md5sum
	# OpenBSD has no "md5sum" but does have "cksum -a md5".
	cksum -r -a md5 2>/dev/null || cksum
}

# Define the measurements. Each measurement definition contains:
#  - The ID of the measurement
#  - The name of the measurement
#  - A space-separated list of single-letter options that PV must support
#  - How many testFileMB multiples are transferred, or Z for testZeroesMB
#  - The command to run
# In the command to run, {PV} is replaced with the path to PV, {FILE1} and
# {FILE2} are replaced by the random data filenames, {ZSIZE} is replaced by
# testZeroesMB, and {OUTPUT} is replaced by a temporary output filename.
# Each definition is stored as single line, with the above parts separated
# by "!".
measurementDefinitions=''
# Add a definition (name, options list, data size, command) to the
# definitions array.
addDefinition () {
	measurementDefinitions="$(
	  printf '%s\n%s!%s!%s!%s!%s\n' \
	    "${measurementDefinitions}" \
	    "$(printf '%s\n' "$1" | runHash | cut -b1-7)" \
	    "$1" "$2" "$3" "$4" \
	  | grep .
	)"
}
# Add a set of standard definitions for measurements, naming them with a
# prefix "$1: ", each with extra options "$2".
addStandardMeasurements () {
	# From file via stdin to file via stdout.
	addDefinition "$1: stdin file to file" "$2" 1 "{PV} $2 < {FILE1} > {OUTPUT}"
	# From file to file via stdout.
	addDefinition "$1: file to file" "$2" 1 "{PV} $2 {FILE1} > {OUTPUT}"
	# From two files to file via stdout.
	addDefinition "$1: two files to file" "$2" 2 "{PV} $2 {FILE1} {FILE2} > {OUTPUT}"
	# From pipe to file via stdout.
	addDefinition "$1: pipe to file" "$2" 1 "cat {FILE1} | {PV} $2 > {OUTPUT}"
	# From file via stdin to pipe.
	addDefinition "$1: stdin file to pipe" "$2" 1 "{PV} $2 < {FILE1} | cat > {OUTPUT}"
	# From file to pipe.
	addDefinition "$1: file to pipe" "$2" 1 "{PV} $2 {FILE1} | cat > {OUTPUT}"
	# From two files to pipe.
	addDefinition "$1: two files to pipe" "$2" 2 "{PV} $2 {FILE1} {FILE2} | cat > {OUTPUT}"
	# From pipe to pipe.
	addDefinition "$1: pipe to pipe" "$2" 1 "cat {FILE1} | {PV} $2 | cat > {OUTPUT}"
}
# Generate the measurement definitions, most of which are based on repeated
# blocks of the above standard measurements, with various different options.
defineMeasurements () {
	addStandardMeasurements 'Default' ''
	addStandardMeasurements 'No-splice' '-C'
	addStandardMeasurements 'Pipe buffer 1M' '-J 1M'
	addStandardMeasurements 'Transfer buffer 1M' '-B 1M'

	addStandardMeasurements 'Discard' '-X'
	addStandardMeasurements 'Discard with no-splice' '-X -C'
	addStandardMeasurements 'Discard with pipe buffer 1M' '-X -J 1M'
	addStandardMeasurements 'Discard with transfer buffer 1M' '-X -B 1M'

	addDefinition "Zeroes: stdout to /dev/null" '-S -s' Z "{PV} -S -s {ZSIZE}M /dev/zero > /dev/null"
	addDefinition "Zeroes: stdout to pipe"  '-S -s' Z "{PV} -S -s {ZSIZE}M /dev/zero | cat > /dev/null"
	addDefinition "Zeroes: discarded" '-X -S -s' Z "{PV} -X -S -s {ZSIZE}M /dev/zero"
	addDefinition "Zeroes: stdout to /dev/null with no-splice" '-C -S -s' Z "{PV} -C -S -s {ZSIZE}M /dev/zero > /dev/null"
	addDefinition "Zeroes: stdout to pipe with no-splice" '-C -S -s' Z "{PV} -C -S -s {ZSIZE}M /dev/zero | cat > /dev/null"
	addDefinition "Zeroes: discarded with no-splice" '-C -X -S -s' Z "{PV} -C -X -S -s {ZSIZE}M /dev/zero"
}

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

# Show all measurement definitions.
showMeasurementDefinitions () {
	printf '%s\n%s\n' 'ID!Name!Opts!Size!Command' "${measurementDefinitions}" \
	| awk -F '!' '{printf "%s\t%-60s\t%s\n", $1, $2, $5}'
}

# Sort a list of version numbers on stdin, in version order.
versionSort () {
	# If sort has no -V, like on CentOS 5, fall back to a simplistic
	# substitute.
	sort -V 2>/dev/null \
	|| perl -pe '$x=$_;$x=~s/(0*[0-9]+)/sprintf("%09d",$1)/ge;chomp $x;$_=$x." ".$_' | sort | awk '{print $2}'
}

# Write a line of results for the measurement with ID $1 and name $2, round
# ${thisRound}, reading the times from ${workDir}/times and deriving the
# rate from the elapsed time (the real time in the times file) and the size
# written in ${workDir}/size.  Removes those two files in the process.
#
# The results are also spooled to ${workDir}/results for later analysis,
# prefixed with only the ID $1.
resultsLine () {
	testTimeReal="$(awk '$1=="real" {print $2}' "${workDir}/times")"
	testTimeUser="$(awk '$1=="user" {print $2}' "${workDir}/times")"
	testTimeSystem="$(awk '$1=="sys" {print $2}' "${workDir}/times")"
	testRate="$(awk -v t="${testTimeReal}" '{if (t>0) { printf "%.3f\n", $1/t } else { print "-" }}' < "${workDir}/size")"
	test -n "${testRate}" || testRate='-'
	rm -f "${workDir}/times" "${workDir}/size"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${outputPrefix}" "${thisRound}" "$1" "${testRate}" "${testTimeReal}" "${testTimeUser}" "${testTimeSystem}" "$2"
	printf '%s\t%s\t%s\t%s\t%s\n' "$1" "${testRate}" "${testTimeReal}" "${testTimeUser}" "${testTimeSystem}" >> "${workDir}/results"
}

# Run $2 in a shell under "time", writing $1 to ${workDir}/size and the
# times to ${workDir}/times.
captureTimes () {
	printf '%s\n' "$1" > "${workDir}/size"
	(
	TIMEFORMAT="real %3R
user %3U
sys %3S"
	time sh -c "{ $2; } 2>&3"
	) 3>&2 2>"${workDir}/times"
	rm -f "${workDir}/elapsed"
}

# Run all defined measurements for which the required options are available.
#
# If "${workDir}/permitted-measurements" is not empty, only measurements
# with IDs listed in that file will be taken.
gatherMeasurements () {
	printf '%s\n' "${measurementDefinitions}" \
	| {
	while read -r definitionLine; do
		measurementId="${definitionLine%%!*}"
		measurementName="${definitionLine#*!}"
		measurementName="${measurementName%%!*}"
		requiredOptions="${definitionLine#*!}"
		requiredOptions="${requiredOptions#*!}"
		requiredOptions="${requiredOptions%%!*}"
		dataSize="${definitionLine#*!}"
		dataSize="${dataSize#*!}"
		dataSize="${dataSize#*!}"
		dataSize="${dataSize%%!*}"
		templateCommand="${definitionLine##*!}"

		test -n "${templateCommand}" || continue

		if test -s "${workDir}/permitted-measurements"; then
			grep -Fqx "${measurementId}" "${workDir}/permitted-measurements" || continue
		fi

		if test -n "${requiredOptions}"; then
			optionLetters="$(printf 'x%s\n' "${requiredOptions}" | tr -d '!' | sed 's/-/!-/g' | tr '!' '\n' | grep '^-' | cut -b 2)"
			optionsPresent='true'
			for pvOption in ${optionLetters}; do
				grep -Fq " -${pvOption}" "${workDir}/help" || optionsPresent='false'
			done
			${optionsPresent} || continue
		fi

		# Add some display options to make more information visible
		# on versions that support them.
		pvExtraOptions=''
		pvFormatString='%b %t %r %p %e %T'
		if grep -Fq " -N" "${workDir}/help"; then
			pvExtraOptions="${pvExtraOptions} -N '${measurementId}'"
			pvFormatString="%N ${pvFormatString}"
		fi
		grep -Fq " -F" "${workDir}/help" && pvExtraOptions="${pvExtraOptions} -F '${pvFormatString}'"

		activeCommand="$(
		  printf '%s\n' "${templateCommand}" | sed \
		    -e "s!{PV}!${pv}${pvExtraOptions}!g" \
		    -e "s!{FILE1}!${workDir}/file1!g" \
		    -e "s!{FILE2}!${workDir}/file2!g" \
		    -e "s!{ZSIZE}!${testZeroesMB}!g" \
		    -e "s!{OUTPUT}!${workDir}/output!g"
		)"

		if test "${dataSize}" = "Z"; then
			dataSize="${testZeroesMB}"
		else
			dataSize=$((dataSize*testFileMB))
		fi

		rm -f "${workDir}/output1" "${workDir}/output2"
		captureTimes "${dataSize}" "${activeCommand}"
		resultsLine "${measurementId}" "${measurementName}"
	done
	}
}

# Run several rounds of benchmark measurements using $1 as the pv
# executable, producing tab-separated data, including a final section
# containing the means and standard deviations for each measurement type.
#
# If $2 is not blank, only run the measurements whose IDs are listed in it.
runBenchmarks () {
	pv="$1"
	restrictTo="$2"

	# Check there's enough room for the test files, meaning that 2 test
	# files + 1 output file + 2MB doesn't add up to more than 75% of the
	# space on the filesystem holding TMPDIR - make the test files
	# smaller, if not.
	tmpSpaceMB="$(df -kP "${TMPDIR:-/tmp}" | awk 'FNR==2 {print int($4*3/4096)}')"
	while test "${testFileMB}" -gt 4; do
		test "${tmpSpaceMB}" -gt $((2+3*testFileMB)) && break
		testFileMB=$((testFileMB/2))
	done

	# Capture the help text so that capabilities can be checked.
	${pv} -h > "${workDir}/help"

	# Write a list of permitted measurement IDs, one per line.  An empty
	# file means no restriction.
	# shellcheck disable=SC2020 # silence the warning about "tr" here.
	printf '%s\n' "${restrictTo}" \
	| tr ',; \t' '\n\n\n\n' \
	| sort -u \
	| grep . \
	> "${workDir}/permitted-measurements"

	# Define identifiers for the system this is running on, the pv
	# version being benchmarked, and this specific benchmark run.
	sysId="$(uname -a | runHash | cut -b1-7)"
	pvId="$(${pv} --version | awk 'FNR==1{print $2}' | awk -F . '{print 1000000*$1+1000*$2+$3}')"
	runId="$(date '+%Y%m%d%H%M%S')"
	outputPrefix="$(printf '%s\t%s\t%s\n' "${sysId}" "${pvId}" "${runId}")"

	# Basic system information, and a header line for the test results.
	printf '%s\t%s\t%s\n' "${outputPrefix}" 'System hostname' "$(uname -n)"
	printf '%s\t%s\t%s\n' "${outputPrefix}" 'System load' "$(uptime | awk '{printf "%.2f\n",$(NF-2)}')"
	printf '%s\t%s\t%s\n' "${outputPrefix}" 'System kernel type' "$(uname -s)"
	printf '%s\t%s\t%s\n' "${outputPrefix}" 'System kernel release' "$(uname -r)"
	printf '%s\t%s\t%s\n' "${outputPrefix}" 'System OS' "$(uname -o)"
	printf '%s\t%s\t%s\n' "${outputPrefix}" 'PV path' "${pv}"
	printf '%s\t%s\t%s\n' "${outputPrefix}" 'PV version' "$(${pv} -V | awk 'FNR==1 {print $2}')"
	printf '%s\t%s\t%s\n' "${outputPrefix}" 'Test file size (MB)' "${testFileMB}"

	# Generate two files of random data.
	dd if='/dev/urandom' of="${workDir}/file1" bs=1048576 count="${testFileMB}" 2>/dev/null
	dd if='/dev/urandom' of="${workDir}/file2" bs=1048576 count="${testFileMB}" 2>/dev/null

	# Run several rounds of measurements.
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${outputPrefix}" '#' 'ID' 'MiB/sec' 'Real time' 'User CPU time' 'System CPU time' 'Raw measurement'
	thisRound=0
	while test ${thisRound} -lt "${rounds}"; do
		thisRound=$((1+thisRound))
		gatherMeasurements
	done

	# For each of the types of measurement, report the mean and standard
	# deviation of each field.
	awk -F "\t" '{print $1}' < "${workDir}/results" > "${workDir}/measurement-ids"
	true > "${workDir}/measurement-ids-used"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${outputPrefix}" 'μ/σ' 'ID' 'MiB/sec' 'Real time' 'User CPU time' 'System CPU time' 'Aggregated measurement'
	{
	while read -r measurementId; do
		# Skip this type of measurement if already processed.
		grep -Fqx "${measurementId}" "${workDir}/measurement-ids-used" && continue
		printf '%s\n' "${measurementId}" >> "${workDir}/measurement-ids-used"
		# Get the associated measurement name.
		measurementName="$(printf '%s\n' "${measurementDefinitions}" | awk -F '!' -v "x=${measurementId}" '$1==x{print $2}')"
		# Separate out this measurement type's results.
		awk -F "\t" -v "mId=${measurementId}" '$1==mId {print}' < "${workDir}/results" \
		> "${workDir}/measurements"
		# Calculate the mean of each field.
		awk -F "\t" -v "mId=${measurementId}" -v "mName=${measurementName}" -v "fieldcount=${fieldsPerRecord}" \
'BEGIN { samples=0 }
{ samples++; for (field=1; field<=fieldcount; field++) { total[field] += $(1+field) } }
END { printf "%s\t%s", "μ", mId; for (field=1; field<=fieldcount; field++) { printf "\t%.3f", total[field]/samples }; printf "\t%s\n", mName }' \
		< "${workDir}/measurements" > "${workDir}/mean"
		# Calculate the standard deviation of each field.
		cat "${workDir}/mean" "${workDir}/measurements" \
		| awk -F "\t" -v "mId=${measurementId}" -v "mName=${measurementName}" -v "fieldcount=${fieldsPerRecord}" \
'BEGIN { samples=0 }
FNR==1 { for (field=1; field<=fieldcount; field++) { mean[field] += $(2+field) } }
FNR>1 { samples++; for (field=1; field<=fieldcount; field++) { variance=$(1+field)-mean[field]; sum_variance_squared[field] += (variance*variance) } }
END { printf "%s\t%s", "σ", mId; for (field=1; field<=fieldcount; field++) { printf "\t%.3f", sqrt(sum_variance_squared[field]/samples) }; printf "\t%s\n", mName }' \
		> "${workDir}/stddev"
		sed "s!^!${outputPrefix}\t!" "${workDir}/mean" "${workDir}/stddev"
	done
	} < "${workDir}/measurement-ids"
	rm -f "${workDir}/file1" "${workDir}/file2" "${workDir}/output1" "${workDir}/output2"
}

# Reformat data on stdin so that each (whitespace-separated) column is
# space-padded and right-aligned to a consistent width for all rows.
#
# Note that this can only handle ASCII, not UTF-8.
lineUpColumns () {
	tee "${workDir}/lineup-temp" \
	| awk 'BEGIN {cols=0}
/./ {
  if (NF > cols)
    cols=NF
  for (col=1; col<=NF; col++) {
    if (length($col) >= max[col])
      max[col]=length($col)
  }
}
END {
  for (col=1; col<=cols; col++) {
    printf "%d ", max[col]
  }
  printf "\n"
}
' > "${workDir}/lineup-colwidths"
	cat "${workDir}/lineup-colwidths" "${workDir}/lineup-temp" \
	| awk '
FNR==1 { cols=NF; for (col=1; col<=NF; col++) { max[col]=$col } }
FNR>1 {
  for (col=1; col<=cols; col++) {
    if (col>1)
      printf " "
    val=(col<=NF ? $col : "")
    printf "%" max[col] "s", val
  }
  printf "\n"
}'
}

# Read a stream of benchmark data on stdin containing runs from a single
# system, and report how the measurements changed across $1 - either "runs"
# for benchmark runs, or "versions" for PV versions.
compareResults () {
	comparisonSelector="$1"
	cat > "${workDir}/raw-system-data"
	# List the measurement IDs in the order they appear in the data.
	awk -F "\t" '$4=="σ"{print $5}' < "${workDir}/raw-system-data" > "${workDir}/measurement-ids"
	case "${comparisonSelector}" in
	'versions')
		# List all PV versions for which any data is available.
		awk -F "\t" '$4=="PV version"{print $2,$5}' "${workDir}/raw-system-data" | sort -nu > "${workDir}/comparison-items"
		itemHeading='Version'
		itemField=2
		;;
	'runs')
		# List all runs.
		awk -F "\t" '$4=="σ"{print $3,$3}' "${workDir}/raw-system-data" | sort -nu > "${workDir}/comparison-items"
		itemHeading='Run'
		itemField=3
		;;
	esac
	# Report on each measurement type in turn.
	true > "${workDir}/measurement-ids-seen"
	{
	measurementsCounter=0
	while read -r measurementId; do
		# Skip if this measurement was already processed.
		grep -Fqx "${measurementId}" "${workDir}/measurement-ids-seen" && continue
		printf '%s\n' "${measurementId}" >> "${workDir}/measurement-ids-seen"
		# Collect this measurement's mean and standard deviation
		# records for each distinct item (version or run).  If
		# there's more than one for a single item, average them.
		{
		while read -r itemId itemName; do
			awk -F "\t" \
			  -v "itemField=${itemField}" -v "itemId=${itemId}" -v "itemName=${itemName}" \
			  -v "mId=${measurementId}" \
			  -v "fieldcount=${fieldsPerRecord}" \
'BEGIN {samples=0}
$itemField==itemId && $5==mId && $4=="μ" { samples++; for (field=1; field<=fieldcount; field++) { mean[field] += $(5+field) } }
$itemField==itemId && $5==mId && $4=="σ" { for (field=1; field<=fieldcount; field++) { stddev[field] += $(5+field) } }
END {
  if (samples > 0) {
    printf "%s", itemName
    for (field=1; field<=fieldcount; field++) {
      printf "\t%.3f\t%.3f", mean[field]/samples, stddev[field]/samples
    }
    printf "\n"
  }
}' \
< "${workDir}/raw-system-data"
		done
		} < "${workDir}/comparison-items" > "${workDir}/measurements-per-item"
		# If there are not at least 2 items for which this
		# measurement was available, report nothing as no comparison
		# can be made.
		test "$(grep -c . "${workDir}/measurements-per-item")" -lt 2 && continue
		# Show the measurement name and associated command.
		# Take the measurement name from the input data.
		measurementName="$(awk -F "\t" -v "mId=${measurementId}" '$4=="σ" && $5==mId {print $NF;exit}' "${workDir}/raw-system-data")"
		if ! ${terseFormat}; then
			# Take the command from the definitions.
			templateCommand="$(printf '%s\n' "${measurementDefinitions}" | awk -F '!' -v "mId=${measurementId}" '$1==mId {print $5}')"
			printf '\n%s\n' "${measurementName}"
			test -n "${templateCommand}" && printf ' (%s)\n' "${templateCommand}"
		fi
		# Report each item's measurements and how they compare to
		# the previous item.
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
}' \
		< "${workDir}/measurements-per-item" \
		> "${workDir}/item-report"
		# Leave only the last line if finalLineOnly is set.
		${finalLineOnly} && sed -i -n '$p' "${workDir}/item-report"
		# In terse mode, prefix each line with the measurement name,
		# having replaced spaces in it with underscores.
		if ${terseFormat}; then
			showName="$(printf '%s\n' "${measurementName}" | tr ' ' '_')"
			sed -i "s!^!${showName}\t!" "${workDir}/item-report"
		fi
		# Format the report.
		# Since the column widths are set by an awk script which
		# doesn't support UTF-8, ASCII headings are used initially,
		# and adjusted after formatting.
		measurementsCounter=$((1+measurementsCounter))
		{
		# In terse mode, only print one header per system.
		if test ${measurementsCounter} -eq 1 || ! ${terseFormat}; then
			printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
			  "${itemHeading}" \
			  'M:Rate' 'S:Rate' 'C:Rate' \
			  '-' \
			  'M:tReal' 'S:tReal' 'C:tReal' \
			  '-' \
			  'M:tUser' 'S:tUser' 'C:tUser' \
			  '-' \
			  'M:tSys' 'S:tSys' 'C:tSys' \
			| {
				if ${terseFormat}; then
					sed "s!^!Measurement\t!"
				else
					cat
				fi
			}
		fi
		cat "${workDir}/item-report"
		} \
		> "${workDir}/item-report-with-heading"
		if ${terseFormat}; then
			cat "${workDir}/item-report-with-heading"
		else
			lineUpColumns < "${workDir}/item-report-with-heading" \
			| sed '1{s,M:,μ:,g;s,S:,σ:,g;s,C:,±:,g}'
		fi
	done
	} < "${workDir}/measurement-ids" \
	> "${workDir}/measurements-report"
	if "${terseFormat}"; then
		lineUpColumns < "${workDir}/measurements-report" \
		| sed '1{s,M:,μ:,g;s,S:,σ:,g;s,C:,±:,g}'
	else
		cat "${workDir}/measurements-report"
	fi
}

# Read a stream of benchmark data from stdin containing one or more runs
# from one or more systems, and for each individual system, report how the
# measurements changed across either the different PV versions or between
# runs, depending on whether $1 is "versions" or "runs".
#
# If $1 is "auto", then it will be "versions" if data for more than one PV
# version is present, otherwise it will be "runs".
runAnalysis () {
	analysisSelector="$1"
	cat > "${workDir}/raw-data"
	# Auto-detect what to analyse.
	if test "${analysisSelector}" = 'auto'; then
		if test "$(awk -F "\t" '$4=="σ"{print $2}' < "${workDir}/raw-data" | sort -u | grep -c .)" -gt 1; then
			analysisSelector='versions'
		else
			analysisSelector='runs'
		fi
	fi
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
		${terseFormat} || cat <<EOF

For each measurement type, each of its aggregate benchmark results are
shown.  Each field's mean and standard deviation are displayed along with a
change indicator showing how different this line's value is from the
previous line.
EOF
		compareResults "${analysisSelector}" < "${workDir}/system-data"
	done
	} < "${workDir}/sysids"
}

##############################################################################
# Main entry point.

# Process any command-line options.
action='benchmark'
restrictMeasurementIdList=''
while test -n "$1"; do
	arg="$1"
	shift
	case "${arg}" in
	'measurements'|'benchmark'|'analyse') action="${arg}" ;;
	'-h'|'--help')
		cat - <<EOF
Usage: ${programName} [OPTIONS] [ACTION]
Benchmark pv transfers - take measurements of transfer rate and CPU usage
when calling pv in various different ways, and analyse results from multiple
benchmark runs to show how each measurement changes between runs or versions.

Actions:
  benchmark    - take several rounds of measurements (default action)
  analyse      - analyse benchmark data on stdin from multiple runs
  measurements - list all benchmark measurement definitions

Benchmark options:
  -p, --program FILE    benchmark using FILE as the pv executable (${pv})
  -r, --rounds ROUNDS   run ROUNDS sets of measurements (${rounds})
  -m, --measurement ID  only run this specific measurement
  -s, --size SIZE       attempt to use a test file of SIZE MiB (${testFileMB})
  -z, --zeroes SIZE     stop at SIZE MiB for /dev/zero measurements (${testZeroesMB})
  -d, --dir DIR         compile and benchmark each of the tar.gz files in DIR

Analysis options:
  -c, --compare WHAT    analyse differences in runs, versions, or auto (${compareWhat})
  -f, --final           show only the final item's line in each measurement analysis
  -t, --terse           produce a terser report

Other options:
  -h, --help            show this help
  -V, --version         show script version

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
	'-m'|'--measurement') restrictMeasurementIdList="${restrictMeasurementIdList} $1"; test $# -gt 0 && shift ;;
	'--measurement='*) restrictMeasurementIdList="${restrictMeasurementIdList} ${arg#*=}" ;;
	'-s'|'--size') testFileMB="$1"; test $# -gt 0 && shift ;;
	'--size='*) testFileMB="${arg#*=}" ;;
	'-z'|'--zeroes') testZeroesMB="$1"; test $# -gt 0 && shift ;;
	'--zeroes='*) testZeroesMB="${arg#*=}" ;;
	'-d'|'--dir') sourcesDir="$1"; test $# -gt 0 && shift ;;
	'--dir='*) sourcesDir="${arg#*=}" ;;
	'-c'|'--compare') compareWhat="$1"; test $# -gt 0 && shift ;;
	'--compare='*) compareWhat="${arg#*=}" ;;
	'-f'|'--final') finalLineOnly='true' ;;
	'-t'|'--terse') terseFormat='true' ;;
	'-'*) die "${arg}: unknown option - try \`--help'" ;;
	*) die "${arg}: unexpected argument - try \`--help'" ;;
	esac
done

# Check validity of compareWhat, and normalise it.
case "${compareWhat}" in
'runs'|'run') compareWhat='runs' ;;
'versions'|'version') compareWhat='versions' ;;
'auto') ;;
*) die "--compare: ${compareWhat}: invalid value" ;;
esac

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

# Define the measurements.
defineMeasurements

# Run the selected action.
case "${action}" in
'measurements') showMeasurementDefinitions ;;
'benchmark')
	if test -z "${sourcesDir}"; then
		runBenchmarks "${pv}" "${restrictMeasurementIdList}"
	else
		find "${sourcesDir}" -type f -name "*.tar.gz" \
		| versionSort \
		| while read -r sourcesFile; do
			buildDir="$(mktemp -d "${sourcesFile}.build.XXXXXX")" || continue
			trap 'rm -rf "${workDir}" "${buildDir}"' EXIT
			tar xzf "${sourcesFile}" -C "${buildDir}" \
			&& (
			cd "${buildDir}"/* \
			&& sh ./configure 1>&2 \
			&& make 1>&2 \
			&& mv pv ..
			) \
			&& runBenchmarks "${buildDir}/pv" "${restrictMeasurementIdList}"
			rm -rf "${buildDir}"
			trap 'rm -rf "${workDir}"' EXIT
		done
	fi
	;;
'analyse') runAnalysis "${compareWhat}" ;;
esac
