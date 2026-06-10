# Notes for developers and translators

The following _configure_ options will be of interest to developers and
translators:

 * **--enable-debugging** - build in debugging support
 * **--enable-profiling** - build in support for profiling

These targets are available when running _make_:

 * "`make analyse`" - run _splint_ and _flawfinder_ on all C source files


## Debugging and profiling support

When "`./configure --enable-debugging`" is used, the _pv_ produced by _make_
will support an extra option, "**--debug FILE**", which will cause debugging
output to be written to *FILE*.  This is not recommended for production
builds due to the extra processing it introduces, and the potential size of
the output.

Within the code, "**debug()**" is used in a similar way to "**printf()**". 
It will automatically include the calling function, source file, and line
number, so they don't need to be included in the parameters.  When debugging
support is not enabled, it evaluates to a null statement.

This does mean that if you call "**debug()**", make sure it has no side
effects, as they won't be present in builds without debugging support.

Builds produced after "`./configure --enable-profiling`" will write profile
data when run, to be used with _gprof_.  See "`man gprof`" for details. 
Please note that the memory safety checks will fail with profiling enabled.


## Source code analysis

Running "`make analyse`" runs _splint_ and _flawfinder_ on all C sources,
writing the output of both programs to files named _"*.e"_ for each _"*.c"_.

There are no dependency rules set up for these _".e"_ files, so if a header
file is altered, manually remove the relevant _".e"_ files, or update the
timestamp of the relevant _".c"_ files, before running "`make analyse`"
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

Translators can use the Weblate instance hosted by Codeberg:
[https://translate.codeberg.org/projects/pv/](https://translate.codeberg.org/projects/pv/)

If you don't see your language listed, raise an issue on the issue tracker
(or just email the maintainer) asking for it to be added.

Alternatively, read on for details of how to maintain the translations
directly within the source tree.

The message catalogues used to translate program messages into other
languages are in the _"po/"_ directory, named _"xx.po"_, where _"xx"_
is the ISO 639-1 2-letter language code, such as "**fr**" for French.

Each of these files contains lines like this:

    #: src/pv/cursor.c:85
    msgid "failed to get terminal name"
    msgstr "erro ao ler o nome do terminal"

The comment line, starting _"#"_, shows the source filename and line number
at which this message can be found.  The _"msgid"_ is the original message
in the program, in English.  The _"msgstr"_ is the translated text.

It is the _"msgstr"_ lines which need to be updated by translators.

Message catalogue files should all be encoded as UTF-8.

After making a change to a _".po"_ file, test it by compiling it and
installing to a temporary location, like this:

    ./configure --enable-debugging --prefix=/tmp/yourtest
    make clean
    make install
    localedef -f UTF-8 -i de_DE /tmp/yourtest/share/locale/de_DE.UTF-8
    bash
    export LOCPATH=/tmp/yourtest/share/locale
    export LC_ALL=de_DE.UTF-8
    export LANGUAGE=de_DE
    /tmp/yourtest/bin/pv --help
    exit

Replace "**--help**" with whatever is appropriate for your test.  In this
example, the language being tested is _"de"_ (German), on a system which is
running with UTF-8 support.

In the example above, a new shell is started by typing "`bash`" so that
after your tests, "`exit`" will return you to your previous shell with your
language settings intact.

Note that at the start, you have to re-run _configure_ with "**--prefix**"
rather than using "`make DESTDIR=...`", because the locale directory is set
at compile time.  Also, on Debian at least, both the environment variables
_"LC_ALL"_ and _"LANGUAGE"_ must be set.

To find untranslated strings in a language that's already supported but
which you can help with gaps in, use this command:

    msgattrib --untranslated < po/fr.po

replacing _"po/fr.po"_ as appropriate.

To add a new language, create the new message catalogue file under _"po/"_
by copying _"po/pv.pot"_ to _"po/xx.po"_, where _"xx"_ is the language code,
and adjusting it.  The _".pot"_ file is generated automatically by _make_.

Next, add the language code to _"po/LINGUAS"_ - this is a list of the
2-letter codes of the supported languages.

Finally, run "`./config.status`" and "`make -C po update-po`".

When the source code is updated, running "`make -C po update-po`" will
update the _"pv.pot"_ file so that it lists where all the messages are in
the source.  It will also use _msgmerge_ to update all of the _".po"_ files
from the updated _"pv.pot"_ file.  After doing this, look for missing
translations (empty _"msgstr"_ lines) or translations marked as "fuzzy", as
these will need to be corrected by translators.


## Benchmarking

The "`docs/benchmark-pv-transfers.sh`" script, introduced in
[#198](https://codeberg.org/ivarch/pv/issues/198), is used to check that new
changes to **pv** aren't making it less efficient at transferring data.  Run
it with the **--help** option for a summary of its options.

In _benchmark_ mode, it takes multiple rounds of measurements of the
transfer rate using various different transfer options.  The mean (μ) and
standard deviation (σ) of each measurement is produced at the end of the
run.  All data is written to the standard output - pipe to "`tee OUTPUT`" to
write to a file _OUTPUT_ while still displaying it on the terminal.

Either a specific **pv** binary can be benchmarked (with the **-p** option),
or multiple **pv** versions can be benchmarked in a single call, each one
counting as an individual run (with the **-d** option, pointing to a
directory containing _tar.gz_ source archives).

Transfer rate measurements use up to two test files, the size of which can
be overridden by the **-s** option, and some measurements will transfer and
discard zeros from _/dev/zero_, the amount of which can be overridden with
the **-z** option.

The measured rates may vary significantly due to external factors such as
system load, so multiple measurement rounds (the **-r** option) are
recommended per run.

In _analyse_ mode, the results of multiple runs are read from standard
input, and a report is produced that shows how each measurement differs
between runs.

If the input data is from multiple versions of **pv**, the differences
between versions will be compared; otherwise, the differences between
individual runs will be compared.  The **-c** option overrides this.

When re-using data from multiple runs, the **-f** option causes the report
to only show the differences between the last two runs (or versions).

The **-t** option produces a terse report that is more machine-readable.

In _measurements_ mode, a list of all of the measurement types is produced.
Each type of measurement has an associated ID - these IDs can be passed to
the benchmark mode to only run a subset of the measurements in that run.


## Release checklist

The package maintainer should run through these steps for a new release:

 * Check for patches and bug reports:
   * <https://codeberg.org/ivarch/pv/issues>
   * <https://tracker.debian.org/pkg/pv>
   * <https://launchpad.net/ubuntu/+source/pv>
   * <https://archlinux.org/packages/extra/x86_64/pv/>
   * <https://packages.gentoo.org/packages/sys-apps/pv>
   * <https://cvsweb.openbsd.org/ports/sysutils/pv/>
   * <https://packages.fedoraproject.org/pkgs/pv/pv/>
 * Check for translation updates <https://translate.codeberg.org/projects/pv>
 * Run "`make indent; make indent indentclean check`"
 * Check that _po/POTFILES.in_ is up to date
 * Run "`make -C po update-po`"
 * Run "`make analyse`" and see whether remaining warnings can be addressed
 * Version bump and documentation checks:
   * Update the version in _configure.ac_ and _docs/NEWS.md_
   * Check that _docs/NEWS.md_ is up to date
   * Check that the manual _docs/pv.1_ is up to date
   * Run "`make docs/pv.1.md`" and, if using VPATH, copy the result to the source directory
   * Check that the year displayed by _src/main/version.c_ is correct
 * Ensure everything has been committed to the repository
 * Run "`autoreconf -is`" in the source directory
 * Consistency and build checks:
   * Wipe the build directory, and run _configure_ there
   * Run "`make distcheck`"
   * Run "`./configure && make check`" on all test systems including Cygwin, using the `tar.gz` that was just created
   * Run a cross-compilation check
 * Run "`make release MAINTAINER=<signing-user>`"
 * Update the project web site:
   * Copy the release _.tar.gz_, _.txt_, and _.asc_ files to the web site
   * Use "`pandoc --from markdown --to html`" to convert the news and manual to HTML
   * Update the news and manual on the web site
   * Update the version numbers on the web site
   * Update the package index on the web site
 * Create a new release in the repository, and apply the associated tag

