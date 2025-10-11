#!/bin/sh
#
# Automate a portion of the release checklist.  Expects to be run from a
# build directory, such as one in which "configure" has been run, i.e. 
# there should be a Makefile present.
#
# Requires "parmo" (https://ivarch.com/p/parmo); builds for all targets that
# parmo supports.
#
# All of the release artefacts are placed in a "PV-RELEASE-x" directory,
# where "x" is the version.
#
# Copyright 2024-2025 Andrew Wood
# License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.

srcdir="$(awk '/^VPATH/{print $NF}' < Makefile | sed -n 1p)"
manuals="$(find "${srcdir}/docs" -mindepth 1 -maxdepth 1 -type f -name "*.[0-9]" -printf "%f\n")"
mainProgram="$(awk '/^[a-z]+_PROGRAMS/{print $3}' Makefile |cut -d '$' -f 1 | sed -n 1p)"
labTestScript="${srcdir}/docs/test-on-vm-lab.sh"

status () {
	test -n "$*" && printf "\n"
	printf '\e]0;%s\007\r' "$*" >&2
	if test -n "$*"; then
		tput rev >&2
		printf " -- %s -- " "$*" >&2
		tput sgr0 >&2
		printf "\n\n"
	fi
}

possiblyDie () {
	printf "\n" >&2
	tput bold >&2
	printf "%s: %s\n" "release" "$*" >&2
	tput sgr0 >&2
	printf "\n%s\n" "Type 'y' and hit Enter to continue anyway."
	read -r line
	test "${line}" = "y" || { status ""; exit 1; }
}

test -e "${labTestScript}" || labTestScript=""

# The checklist is re-ordered a little so that we defer making changes (like
# "make indent") until as late as possible, in case any checks fail.

status "Initial checks"

# Check MAINTAINER is provided.
test -n "${MAINTAINER}" || possiblyDie "environment variable MAINTAINER is empty"

# * Check that _po/POTFILES.in_ is up to date
if test -d "${srcdir}/po"; then
	inFile="$(sort < "${srcdir}/po/POTFILES.in")"
	realList="$(find "${srcdir}" -name "*.c" -printf "%P\n" | sort)"
	test "${inFile}" = "${realList}" || possiblyDie "po/POTFILES.in is incorrect"
fi

status "Initial build"
make || possiblyDie "failed 'make'"

# * Run "_make analyse_" and see whether remaining warnings can be addressed
status "Source analysis"
make analyse || possiblyDie "failed 'make analyse'"

# * Version bump and documentation checks:

status "Version checks"

#   * Check that _docs/NEWS.md_ is up to date
sed -n 1p docs/NEWS.md | grep -Fqi unreleased && possiblyDie "NEWS.md says 'unreleased' on line 1"
versionInNews="$(awk 'FNR==1{print $2}' "${srcdir}/docs/NEWS.md")"
printf "%s\n" "${versionInNews}" | grep -Eq '^[0-9]' || possiblyDie "version in NEWS.md (${versionInNews}) is not numeric"

#   * Check the version in both _configure.ac_ and _docs/NEWS.md_ was updated
versionInConfig="$(grep ^AC_INIT "${srcdir}/configure.ac" | cut -d '[' -f 3 | cut -d ']' -f 1)"
test "${versionInConfig}" = "${versionInNews}" || possiblyDie "version in configure.ac (${versionInConfig}) mismatches NEWS.md (${versionInNews})"

#   * Check that the manuals are up to date
for manPage in ${manuals}; do
	versionInManual="$(awk 'FNR==1 {print $5}' "${srcdir}/docs/${manPage}" | cut -d - -f 2)"
	test "${versionInManual}" = "${versionInNews}" || possiblyDie "version in ${manPage} (${versionInManual}) mismatches NEWS.md (${versionInNews})"
done

#   * Check that the program version is correct
versionInVersionOutput="$(./"${mainProgram}" --version | awk 'FNR==1{print $NF}')"
test "${versionInVersionOutput}" = "${versionInNews}" || possiblyDie "version in '${mainProgram} --version' (${versionInVersionOutput}) mismatches NEWS.md (${versionInNews})"

#   * Check that the year displayed by --version is correct
yearInVersion="$(./"${mainProgram}" --version | awk '/^Copyright/{print $2}')"
yearNow="$(date '+%Y')"
test "${yearInVersion}" = "${yearNow}" || possiblyDie "the year in '--version' (${yearInVersion}) is not this year (${yearNow})"

#   * Make the Markdown version of the manuals and, if using VPATH, copy the result to the source directory
# We also wipe everything in "docs" so we don't accidentally package
# leftover working files.

status "Markdown manual"

rm -f docs/*
for manPage in ${manuals}; do
	make "docs/${manPage}.md"
	cp "docs/${manPage}.md" "${srcdir}/docs/${manPage}.md"
done

# * Run "_make indent; make indent indentclean check_"
status "Reformat source"
make indent
make indent indentclean
# The check will run later as part of "make distcheck".

# * Run "_make -C po update-po_"
status "Update po files"
if test -d "${srcdir}/po"; then
	make -C po update-po || possiblyDie "update-po failed"
fi

# * Run "_autoreconf_" in the source directory
status "Run autoreconf"
(cd "${srcdir}" && autoreconf -is) || possiblyDie "autoreconf failed"

# * Ensure everything has been committed to the repository
status "Commit check"
gitStatus="$(cd "${srcdir}" && git status --porcelain=v1)" || possiblyDie "failed to run 'git status'"
test -z "${gitStatus}" || possiblyDie "not everything is committed - 'git status' is not empty"

# * Consistency and build checks:
#   * Wipe the build directory, and run "_configure_" there
#   * Run "_make distcheck_"
# * Run "_make release MAINTAINER=<signing-user>_"
# NB "make release" implies "make distcheck".
# We set SKIP_VALGRIND_TESTS=1 because the full tests will run in the lab
# check.
status "Release archive"
workDir="$(mktemp -d)" || possiblyDie "mktemp failed"
trap 'chmod -R u+w "${workDir}"; rm -rf "${workDir}"' EXIT
(
cd "${workDir}" || exit 1
sh "${srcdir}/configure" || exit 1
make -j8 release SKIP_VALGRIND_TESTS=1 || exit 1
exit 0
) || possiblyDie "failed on 'make release'"

sourceArchive="$(find "${workDir}" -mindepth 1 -maxdepth 1 -type f -name "*.tar.gz")"
test -e "${sourceArchive}.asc" || possiblyDie "release was not signed"

#   * Run "_./configure && make check_" on all test systems, using the _tar.gz_ that was just created
#   * Run a cross-compilation check
if test -n "${labTestScript}"; then
	status "Lab test"
	sh "${labTestScript}" "${sourceArchive}" || possiblyDie "lab test failed"
fi

# * Update the project web site:
#   * Copy the release _.tar.gz_, _.txt_, and _.asc_ files to the web site
#   * Use "_pandoc --from markdown --to html_" to convert the news and manual to HTML
status "Release dir"
releaseDir="PV-RELEASE-${versionInNews}"
rm -rf "${releaseDir}"
mkdir "${releaseDir}"
cp "${sourceArchive}" "${sourceArchive}.asc" "${sourceArchive}.txt" "${releaseDir}/" || possiblyDie "failed to copy release files"

status "HTML docs"
for manPage in ${manuals}; do
	pandoc --from markdown --to html --shift-heading-level-by=1 < "${srcdir}/docs/${manPage}.md" > "${releaseDir}/${manPage}.html"
done
pandoc --from markdown --to html < "${srcdir}/docs/NEWS.md" > "${releaseDir}/news.html"

# Build OS packages.
buildTargets=$(parmo targets)
if test -n "${MAINTAINER}"; then
	gpg --export-secret-key --armor "${MAINTAINER}" > "${workDir}/signing-key" \
	|| possiblyDie 'failed to export signing key'
fi
for buildTarget in ${buildTargets}; do
	status "Package: ${buildTarget}"
	buildOK=true
	rm -rf "${workDir}/package"
	mkdir "${workDir}/package"
	if test -s "${workDir}/signing-key"; then
		parmo --key "${workDir}/signing-key" --source "${sourceArchive}" --destination "${workDir}/package" --target "${buildTarget}" build-package \
		|| possiblyDie "${buildTarget}: build failed"
	else
		parmo --source "${sourceArchive}" --destination "${workDir}/package" --target "${buildTarget}" build-package \
		|| possiblyDie "${buildTarget}: build failed"
	fi
	find "${workDir}/package" -type f \
	| while read -r packageFile; do
		destinationFile="${releaseDir}/${packageFile##*/}"
		case "${destinationFile}" in
		*.deb) destinationFile="${destinationFile%.deb}_${buildTarget}.deb" ;;
		*.txt) destinationFile="" ;;
		esac
		test -n "${destinationFile}" || continue
		cp "${packageFile}" "${destinationFile}"
		chmod 644 "${destinationFile}"
	done
done

find "${releaseDir}/" -type f -exec chmod 644 '{}' ';'
find "${releaseDir}/" -type d -exec chmod 755 '{}' ';'

status "Done"

cat <<EOF

Files are under: ${releaseDir}/

Still to do:
  * Update the news and manual on the web site
  * Update the version numbers on the web site
  * Update the package index on the web site
  * Create a new release in the repository, and apply the associated tag

EOF

exit 0
