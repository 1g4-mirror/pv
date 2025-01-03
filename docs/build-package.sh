#!/bin/sh
#
# Usage: sh build-package.sh source-1.2.3.tar.gz
#
# Given a tar.gz, build a package for the current OS.
#
# Supports these packaging types:
#
#   .deb (Debian, Ubuntu, etc) - requires dpkg-dev, debhelper
#   .rpm (AlmaLinux, Rocky Linux, OpenSUSE etc) - requires rpmbuild
#
# If the MAINTAINER environment variable is set, this is used as a GPG key
# ID for package signing.
#
# The packages built with this script are not "official", in that they are
# not part of any Linux distribution and may not follow the distribution's
# policies.  Use at your own risk.
#
# Copyright 2024-2025 Andrew Wood
# License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.

sourceArchive="$1"
test -n "${sourceArchive}" && test -f "${sourceArchive}" \
|| { printf "%s: %s: %s\n" "${0##*/}" "${sourceArchive}" "source archive not found" 1>&2; exit 1; }

packageName="pv"
packageSummary="Shell pipeline element to meter data passing through"
packageDescription="\
pv (Pipe Viewer) can be inserted into any normal pipeline between two
processes to give a visual indication of how quickly data is passing
through, how long it has taken, how near to completion it is, and an
estimate of how long it will be until completion."
packageUrl="https://www.ivarch.com/programs/pv.shtml"
sourceUrl="https://www.ivarch.com/programs/sources/${packageName}-%{version}.tar.gz"
packageEmail="Andrew Wood <pv@ivarch.com>"

workDir="$(mktemp -d)" || exit 1
trap 'rm -rf "${workDir}"' EXIT

# Infer the version from the most recent NEWS entry.
version="$(tar xzf "${sourceArchive}" --wildcards -O '*/docs/NEWS.md' 2>/dev/null | awk '/^### [0-9]/{print $2}' | sed -n 1p)"

if command -v rpmbuild >/dev/null 2>&1; then
	# Build an RPM.
	cat > "${workDir}"/spec <<EOF
Summary: ${packageSummary}
Name: ${packageName}
Version: ${version}
Release: 1%{?dist}
License: GPLv3+
Source: ${sourceUrl}
Url: ${packageUrl}

%description
${packageDescription}

%prep
%setup -q

%build
%configure
make %{?_smp_mflags}

%install
test -n "\${RPM_BUILD_ROOT}" && test "\${RPM_BUILD_ROOT}" != '/' && rm -rf "\${RPM_BUILD_ROOT}"
make install DESTDIR="\${RPM_BUILD_ROOT}"
rm -rf "\${RPM_BUILD_ROOT}/usr/share/doc"
%find_lang %{name}

%check
make check SKIP_VALGRIND_TESTS=1

%clean
test -n "\${RPM_BUILD_ROOT}" && test "\${RPM_BUILD_ROOT}" != '/' && rm -rf "\${RPM_BUILD_ROOT}"

%files -f %{name}.lang
%defattr(-, root, root)
%{_bindir}/*
%{_mandir}/man1/*

%doc README.md docs/NEWS.md docs/ACKNOWLEDGEMENTS.md docs/COPYING

%changelog
* $(date '+%a %b %d %Y') ${packageEmail} - ${version}-1
- Package built from main source.
EOF
	mkdir "${workDir}/SOURCES"
	cp "${sourceArchive}" "${workDir}/SOURCES/${packageName}-${version}.tar.gz"
	rpmbuild -D "%_topdir ${workDir}" -bb "${workDir}/spec" || exit 1
	if test -n "${MAINTAINER}"; then
		# Sign the RPMs.
		rpm -D "%_gpg_name ${MAINTAINER}" --addsign "${workDir}/RPMS"/*/*.rpm || exit 1
	fi
	cp -v "${workDir}/RPMS"/*/*.rpm .
fi

if command -v dpkg-buildpackage >/dev/null 2>&1; then
	# Build a Debian package.
	mkdir "${workDir}/build" "${workDir}/build/debian" "${workDir}/build/debian/source"

	tar xzf "${sourceArchive}" -C "${workDir}/build" --strip-components=1

	printf "%s\n" "10" > "${workDir}/build/debian/compat"
	touch "${workDir}/build/debian/copyright"
	printf "%s\n" "3.0 (quilt)" > "${workDir}/build/debian/source/format"

	cat > "${workDir}/build/debian/rules" <<EOF
#!/usr/bin/make -f
export DH_VERBOSE = 1
export DEB_BUILD_MAINT_OPTIONS = hardening=+all
export DEB_CFLAGS_MAINT_APPEND  = -Wall -pedantic
export DEB_LDFLAGS_MAINT_APPEND = -Wl,--as-needed

%:
	dh \$@
EOF
	chmod 700 "${workDir}/build/debian/rules"

	cat > "${workDir}/build/debian/control" <<EOF
Source: ${packageName}
Maintainer: ${packageEmail}
Section: misc
Priority: optional
Standards-Version: 3.9.2
Build-Depends: debhelper (>= 9)

Package: ${packageName}
Architecture: any
Depends: \${misc:Depends}
Description: ${packageSummary}
$(printf "%s\n.\n" "${packageDescription}" | sed 's,^, ,')
EOF

	cat > "${workDir}/build/debian/changelog" <<EOF
${packageName} (${version}-1) UNRELEASED; urgency=low

  * Package built from main source.

 -- ${packageEmail}  $(date -R)
EOF

	if test -n "${MAINTAINER}"; then
		(cd "${workDir}/build" && dpkg-buildpackage --build=binary --force-sign --sign-key="${MAINTAINER}") || exit 1
	else
		(cd "${workDir}/build" && dpkg-buildpackage --build=binary) || exit 1
	fi

	cp -v "${workDir}"/*.deb . || exit 1
fi
