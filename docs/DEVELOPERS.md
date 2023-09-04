# Notes for developers and translators

The following "`configure`" options will be of interest to developers and
translators:

 * `--enable-debugging` - build in debugging support
 * `--enable-profiling` - build in support for profiling

These "`make`" targets are available:

 * `make analyse` - run _splint_ and _flawfinder_ on all C source files


## Debugging and profiling support

When "`./configure --enable-debugging`" is used, the "`pv`" produced by
"`make`" will support an extra option, "`--debug FILE`", which will cause
debugging output to be written to *FILE*.  This is not recommended for
production builds due to the extra processing it introduces, and the
potential size of the output.

Within the code, "`debug()`" is used in a similar way to "`printf()`".  It
will automatically include the calling function, source file, and line
number, so they don't need to be included in the parameters.  When debugging
support is not enabled, it evaluates to a null statement.

This does mean that if you call "`debug()`", make sure it has no side
effects, as they won't be present in builds without debugging support.

Builds produced after "`./configure --enable-profiling`" will write profile
data when run, to be used with _gprof_.  See "`man gprof`" for details. 
Please note that the memory safety checks will fail with profiling enabled.


## Source code analysis

Running "`make analyse`" runs _splint_ and _flawfinder_ on all C sources,
writing the output of both programs to files named "`*.e`" for each "`*.c`".

There are no dependency rules set up for these "`.e`" files, so if a header
file is altered, manually remove the relevant "`.e`" files, or update the
timestamp of the relevant "`.c`" files, before running "`make analyse`"
again.

The goal is for all C source files to generate zero warnings from either
tool.

Wherever warnings are disabled by annotations to the source, there should be
associated commentary nearby explaining the rationale behind it, and any
assumptions made.  This is so that there can be some assurance that the
issue highlighted by the analyser has been considered properly, as well as
documenting anything that might need further work if the surrounding code is
altered in future.


## Translation notes

The message catalogues used to translate program messages into other
languages are in the "`po/`" directory, named "`xx.po`", where "`xx`"
is the ISO 639-1 2-letter language code, such as "`fr`" for French.

Each of these files contains lines like this:

    #: src/pv/cursor.c:85
    msgid "failed to get terminal name"
    msgstr "erro ao ler o nome do terminal"

The comment line, starting "`#`", shows the source filename and line number
at which this message can be found.  The "`msgid`" is the original message
in the program, in English.  The "`msgstr`" is the translated text.

It is the "`msgstr`" lines which need to be updated by translators.

Message catalogue files should all be encoded as UTF-8.

_FIXME: The test below doesn't work on Debian with only en_GB installed, and
also it looks like the compiled-in LOCALEDIR is overriding LOCPATH._

After making a change to a "`.po`" file, test it by compiling it and installing
to a temporary location, like this:

    make install DESTDIR=/tmp/yourtest
    localedef -f UTF-8 -i de_DE /tmp/yourtest/usr/local/share/locale/de_DE.UTF-8
    LOCPATH=/tmp/yourtest/usr/local/share/locale \
    LC_ALL=de_DE.UTF-8 ./pv --help

Replace "`--help`" with whatever is appropriate for your test.  In this
example, the language being tested is "`de`" (German), on a system which is
running with UTF-8 support.

To add a new language, create the new message catalogue file under "`po/`"
by copying "`po/pv.pot`" to "`po/xx.po`", where "`xx`" is the language code,
and adjusting it.  The "`.pot`" file is generated automatically by "`make`".

Next, add the language code to "`po/LINGUAS`" - this is a list of the
2-letter codes of the supported languages.

Finally, run "`./config.status`" and "`make -C po update-po`".

When the source code is updated, running "`make -C po update-po`" will
update the "`pv.pot`" file so that it lists where all the messages are in
the source.  It will also use _msgmerge_ to update all of the "`.po`" files
from the updated "`pv.pot`" file.  After doing this, look for missing
translations (empty "`msgstr`" lines) or translations marked as "fuzzy", as
these will need to be corrected by translators.


## Release checklist

The package maintainer should run through these steps for a new release:

 * Check for patches and bug reports at https://tracker.debian.org/pkg/pv
 * Run "`make indent; make indent indentclean check`"
 * Run "`make -C po update-po`"
 * Run "`make analyse`" and see whether remaining warnings can be addressed
 * Version bump and documentation checks:
   * Update the version in `configure.ac` and `docs/NEWS.md`
   * Check that `docs/NEWS.md` is up to date
   * Check that `docs/TODO.md` is up to date
   * Check that the manual `docs/pv.1` is up to date
   * Run "`make docs/pv.1.md`" and, if using VPATH, copy the result to the source directory
 * Ensure everything has been committed to the repository
 * Run "`autoreconf`" in the source directory
 * Consistency and build checks:
   * Wipe the build directory, and run "`configure`" there
   * Run "`make distcheck`"
   * Run "`./configure && make check`" on all test systems including Cygwin, using the `tar.gz` that was just created
 * Run "`make release MAINTAINER=<signing-user>`"
 * Update the project web site:
   * Copy the release `.tar.gz`, `.txt`, and `.asc` files to the web site
   * Use "`pandoc --from markdown --to html`" to convert the TODO, news, and manual to HTML
   * Update the TODO, news, and manual on the web site
   * Update the version numbers on the web site
   * Update the package index on the web site
 * Create a new release in the repository, and apply the associated tag

