## Process this file with automake to produce Makefile.in

AUTOMAKE_OPTIONS = subdir-objects

SUBDIRS = po

bin_PROGRAMS = pv
dist_doc_DATA = README.md docs/INSTALL docs/COPYING docs/NEWS.md docs/ACKNOWLEDGEMENTS.md docs/DEVELOPERS.md docs/TODO.md docs/benchmark.sh
dist_man1_MANS = docs/pv.1

EXTRA_DIST = docs/pv.1.md

pv_SOURCES = \
src/main/debug.c \
src/main/help.c \
src/main/main.c \
src/main/options.c \
src/main/remote.c \
src/main/version.c \
src/pv/cursor.c \
src/pv/display.c \
src/pv/elapsedtime.c \
src/pv/file.c \
src/pv/loop.c \
src/pv/number.c \
src/pv/signal.c \
src/pv/state.c \
src/pv/string.c \
src/pv/transfer.c \
src/pv/watchpid.c \
src/include/config-aux.h \
src/include/options.h \
src/include/pv-internal.h \
src/include/pv.h

## Allow tests to write diagnostic info to fd 9 to reach the original
## stderr even when the test driver is sending test output to a file.
AM_TESTS_FD_REDIRECT = 9>&2

AM_TESTS_ENVIRONMENT = \
  . $(srcdir)/tests/test-env.sh \
  ;
TESTS = \
tests/Bug_-_Display_length_at_magnitude_boundary_-_Bytes.test \
tests/Bug_-_Display_length_at_magnitude_boundary_-_Rate.test \
tests/Bug_-_Install_all_files.test \
tests/Display_-_--average-rate.test \
tests/Display_-_--bits.test \
tests/Display_-_--buffer-percent.test \
tests/Display_-_--bytes.test \
tests/Display_-_--eta_-_plausible_values.test \
tests/Display_-_--fineta_-_plausible_values.test \
tests/Display_-_--last-written.test \
tests/Display_-_--numeric_--bytes_--line-mode.test \
tests/Display_-_--numeric_--bytes.test \
tests/Display_-_--numeric.test \
tests/Display_-_--numeric_--timer.test \
tests/Display_-_--progress_-_basic_movement.test \
tests/Display_-_--progress_-_increasing.test \
tests/Display_-_--quiet.test \
tests/Display_-_--rate_-_displayed_value_changes.test \
tests/Display_-_--timer_-_displayed_value_changes.test \
tests/General_-_--pidfile.test \
tests/General_-_--size_argument_handling.test \
tests/Integrity_-_Basic.test \
tests/Integrity_-_Binary_data.test \
tests/Integrity_-_From_bursty_source.test \
tests/Integrity_-_Large_file_support.test \
tests/Integrity_-_On_output_pipe_close.test \
tests/Integrity_-_When_adjusted_remotely.test \
tests/Memory_safety_-_Basic.test \
tests/Memory_safety_-_Remote_control_receiver.test \
tests/Memory_safety_-_Remote_control_sender.test \
tests/Modifiers_-_--direct-io.test \
tests/Modifiers_-_--force.test \
tests/Modifiers_-_--interval.test \
tests/Modifiers_-_--line-mode.test \
tests/Modifiers_-_--size_from_file_size.test \
tests/Modifiers_-_--size.test \
tests/Modifiers_-_--sync.test \
tests/Terminal_-_Detect_width.test \
tests/Transfer_-_--rate-limit.test \
tests/Transfer_-_--remote.test \
tests/Transfer_-_--stop-at-size.test

EXTRA_DIST += $(TESTS) tests/run-valgrind.sh tests/test-env.sh

docs/pv.1.md: $(srcdir)/docs/pv.1
	test -d docs || mkdir docs
	pandoc --from man --to markdown < $< | sed '/\*\*\*\*/{s/\*//g;s/^/**/;s/$$/**/}' | sed '/^```/,/^```/d' > $@

SUFFIXES = .c .o .e

.c.e:
	-splint -badflag +posixlib $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) -DSPLINT=1 $< > $@ 2>&1
	-flawfinder $< >> $@ 2>&1

indent:
	cd $(srcdir) && indent -npro -kr -i8 -cd42 -c45 -l120 src/*/*.c

indentclean:
	rm -f $(srcdir)/*/*.c~

analyse: $(pv_SOURCES:.c=.e) FORCE
	grep -E -e '^Finished checking --' -e 'Hits = ' $+ 2>/dev/null || true

FORCE:

clean-local:
	rm -f src/*/*.e

# Convenience alias for "make check": "make test"
test: check

# "release" target - requires MAINTAINER variable to select GPG key.
release: distcheck
	gpg --list-secret-keys 2>&1 | grep -F 'uid' | grep -Fq "$(MAINTAINER)"
	gpg -u "$(MAINTAINER)" -ab $(distdir).tar.gz
	cp -f $(distdir).tar.gz.asc $(distdir).tar.gz.txt
	chmod 644 $(distdir).tar.gz $(distdir).tar.gz.asc $(distdir).tar.gz.txt
