#!/bin/sh
#
# Automate a portion of the release checklist.  Expects to be run from a
# build directory, such as one in which "configure" has been run, i.e. 
# there should be a Makefile present.

srcdir="$(awk '/^VPATH/{print $NF}' < Makefile | sed -n 1p)"

possiblyDie () {
	printf "\n" >&2
	tput bold >&2
	printf "%s: %s\n" "release" "$*" >&2
	tput sgr0 >&2
	printf "\n%s\n" "Type 'y' and hit Enter to continue anyway."
	read -r line
	test "${line}" = "y" || exit 1
}

# The checklist is re-ordered a little so that we defer making changes (like
# "make indent") until as late as possible, in case any checks fail.

# Check MAINTAINER is provided.
test -n "${MAINTAINER}" || possiblyDie "environment variable MAINTAINER is empty"

# * Check that `po/POTFILES.in` is up to date
inFile="$(sort < "${srcdir}/po/POTFILES.in")"
realList="$(find "${srcdir}" -name "*.c" -printf "%P\n" | sort)"
test "${inFile}" = "${realList}" || possiblyDie "po/POTFILES.in is incorrect"

# * Run "`make analyse`" and see whether remaining warnings can be addressed
make analyse || possiblyDie "failed 'make analyse'"

# * Version bump and documentation checks:
#   * Check that `docs/NEWS.md` is up to date
versionInNews="$(awk 'FNR==1{print $2}' "${srcdir}/docs/NEWS.md")"
printf "%s\n" "${versionInNews}" | grep -Eq '^[0-9]' || possiblyDie "version in NEWS.md (${versionInNews}) is not numeric"

#   * Check the version in `configure.ac` and `docs/NEWS.md` were updated
versionInConfig="$(grep ^AC_INIT "${srcdir}/configure.ac" | cut -d '[' -f 3 | cut -d ']' -f 1)"
test "${versionInConfig}" = "${versionInNews}" || possiblyDie "version in configure.ac (${versionInConfig}) mismatches NEWS.md (${versionInNews})"

#   * Check that the manual `docs/pv.1` is up to date
versionInManual="$(awk 'FNR==1 {print $5}' "${srcdir}/docs/pv.1" | cut -d - -f 2)"
test "${versionInManual}" = "${versionInNews}" || possiblyDie "version in pv.1 (${versionInManual}) mismatches NEWS.md (${versionInNews})"

#   * Check that the year displayed by src/main/version.c is correct
yearInSource="$(grep -F 'printf("Copyright' "${srcdir}/src/main/version.c" | cut -d '"' -f 4)"
yearNow="$(date '+%Y')"
test "${yearInSource}" = "${yearNow}" || possiblyDie "the year in src/main/version.c (${yearInSource}) is not this year (${yearNow})"

#   * Run "`make docs/pv.1.md`" and, if using VPATH, copy the result to the source directory
# We also wipe everything in "docs" so we don't accidentally package
# leftover working files.
rm -f docs/*
make docs/pv.1.md
cp docs/pv.1.md "${srcdir}/docs/pv.1.md"

# * Run "`make indent; make indent indentclean check`"
make indent
make indent indentclean
# The check will run later as part of "make distcheck".

# * Run "`make -C po update-po`"
make -C po update-po || possiblyDie "update-po failed"

# * Run "`autoreconf`" in the source directory
(cd "${srcdir}" && autoreconf -is) || possiblyDie "autoreconf failed"

# * Ensure everything has been committed to the repository
gitStatus="$(cd "${srcdir}" && git status --porcelain=v1)" || possiblyDie "failed to run 'git status'"
test -z "${gitStatus}" || possiblyDie "not everything is committed - 'git status' is not empty"

# * Consistency and build checks:
#   * Wipe the build directory, and run "`configure`" there
#   * Run "`make distcheck`"
# * Run "`make release MAINTAINER=<signing-user>`"
# NB "make release" implies "make distcheck".
workDir="$(mktemp -d)" || possiblyDie "mktemp failed"
trap 'chmod -R u+w "${workDir}"; rm -rf "${workDir}"' EXIT
(
cd "${workDir}" || exit 1
sh "${srcdir}/configure" || exit 1
export SKIP_VALGRIND_TESTS=1
make -j8 release || exit 1
exit 0
) || possiblyDie "failed on 'make release'"

sourceArchive="$(find "${workDir}" -mindepth 1 -maxdepth 1 -type f -name "*.tar.gz")"
test -e "${sourceArchive}.asc" || possiblyDie "release was not signed"

#   * Run "`./configure && make check`" on all test systems including Cygwin, using the `tar.gz` that was just created
#   * Run a cross-compilation check
sh "${srcdir}/docs/test-on-vm-lab.sh" "${sourceArchive}" || possiblyDie "lab test failed"

# * Update the project web site:
#   * Copy the release `.tar.gz`, `.txt`, and `.asc` files to the web site
#   * Use "`pandoc --from markdown --to html`" to convert the news and manual to HTML
rm -rf "RELEASE-${versionInNews}"
mkdir "RELEASE-${versionInNews}"
cp "${sourceArchive}" "${sourceArchive}.asc" "${sourceArchive}.txt" "RELEASE-${versionInNews}/" || possiblyDie "failed to copy release files"

pandoc --from markdown --to html --shift-heading-level-by=1 < "${srcdir}/docs/pv.1.md" > "RELEASE-${versionInNews}/manual.html"
pandoc --from markdown --to html < "${srcdir}/docs/NEWS.md" > "RELEASE-${versionInNews}/news.html"

chmod 644 "RELEASE-${versionInNews}/"*

cat <<EOF

Files are under: RELEASE-${versionInNews}/

Still to do:
  * Update the news and manual on the web site
  * Update the version numbers on the web site
  * Update the package index on the web site
  * Create a new release in the repository, and apply the associated tag

EOF

exit 0
