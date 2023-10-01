/*
 * Main program entry point - read the command line options, then perform
 * the appropriate actions.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "options.h"
#include "pv.h"

/* We do not set this because it breaks "dd" - see below. */
/* #undef MAKE_STDOUT_NONBLOCKING */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>


int pv_remote_set(opts_t, pvstate_t);
void pv_remote_init(void);
void pv_remote_fini(void);


/*
 * Process command-line arguments and set option flags, then call functions
 * to initialise, and finally enter the main loop.
 */
int main(int argc, char **argv)
{
	/*@only@ */ opts_t opts = NULL;
	/*@only@ */ pvstate_t state = NULL;
	int retcode = 0;

#ifdef ENABLE_NLS
	/* Initialise language translation. */
	(void) setlocale(LC_ALL, "");
	(void) bindtextdomain(PACKAGE, LOCALEDIR);
	(void) textdomain(PACKAGE);
#endif

	/* Parse the command line arguments. */
	opts = opts_parse(argc >= 0 ? (unsigned int) argc : 0, argv);
	if (NULL == opts) {
		debug("%s: %d", "exiting with status", 64);
		return 64;
	}

	/* Early exit if necessary, such as with "-h". */
	if (opts->do_nothing) {
		debug("%s", "nothing to do - exiting with status 0");
		opts_free(opts);
		return 0;
	}

	/*
	 * Allocate our internal state buffer.
	 */
	state = pv_state_alloc(opts->program_name);
	if (NULL == state) {
		/*@-mustfreefresh@ */
		/*
		 * splint note: the gettext calls made by _() cause memory
		 * leak warnings, but in this case it's unavoidable, and
		 * mitigated by the fact we only translate each string once.
		 */
		fprintf(stderr, "%s: %s: %s\n", opts->program_name, _("state allocation failed"), strerror(errno));
		opts_free(opts);
		debug("%s: %d", "exiting with status", 64);
		return 64;
		/*@+mustfreefresh@ */
	}

	/*
	 * -R specified - send the message, then exit.
	 */
	if (opts->remote > 0) {
		/* Initialise signal handling. */
		pv_sig_init(state);
		/* Send the message. */
		retcode = pv_remote_set(opts, state);
		/* Close down the signal handling. */
		pv_sig_fini(state);
		/* Free resources. */
		pv_state_free(state);
		opts_free(opts);
		/* Early exit. */
		return retcode;
	}

	/*
	 * Write a PID file if -P was specified.
	 */
	if (opts->pidfile != NULL) {
		char *pidfile_tmp_name;
		size_t pidfile_tmp_bufsize;
		int pidfile_tmp_fd;
		FILE *pidfile_tmp_fptr;
		mode_t prev_umask;

		pidfile_tmp_bufsize = 16 + strlen(opts->pidfile);	/* flawfinder: ignore */
		/*
		 * flawfinder rationale: pidfile was supplied as an argument
		 * so we have to assume it is \0 terminated.
		 */
		pidfile_tmp_name = malloc(pidfile_tmp_bufsize);
		if (NULL == pidfile_tmp_name) {
			fprintf(stderr, "%s: %s\n", opts->program_name, strerror(errno));
			pv_state_free(state);
			opts_free(opts);
			return 1;
		}
		memset(pidfile_tmp_name, 0, pidfile_tmp_bufsize);
		(void) pv_snprintf(pidfile_tmp_name, pidfile_tmp_bufsize, "%s.XXXXXX", opts->pidfile);

		/*@-type@ *//* splint doesn't like mode_t */
		prev_umask = umask(0000);   /* flawfinder: ignore */
		(void) umask(prev_umask | 0133);	/* flawfinder: ignore */

		/*@-unrecog@ *//* splint doesn't know mkstemp() */
		pidfile_tmp_fd = mkstemp(pidfile_tmp_name);	/* flawfinder: ignore */
		/*@+unrecog@ */
		if (pidfile_tmp_fd < 0) {
			fprintf(stderr, "%s: %s: %s\n", opts->program_name, pidfile_tmp_name, strerror(errno));
			(void) umask(prev_umask);	/* flawfinder: ignore */
			free(pidfile_tmp_name);
			pv_state_free(state);
			opts_free(opts);
			return 1;
		}

		(void) umask(prev_umask);   /* flawfinder: ignore */

		/*
		 * flawfinder rationale (umask, mkstemp) - flawfinder
		 * recommends setting the most restrictive umask possible
		 * when calling mkstemp(), so this is what we have done.
		 *
		 * We get the original umask and OR it with 0133 to make
		 * sure new files will be at least chmod 644.  Then we put
		 * the umask back to what it was, after creating the
		 * temporary file.
		 */

		/*@+type@ */

		pidfile_tmp_fptr = fdopen(pidfile_tmp_fd, "w");
		if (NULL == pidfile_tmp_fptr) {
			fprintf(stderr, "%s: %s: %s\n", opts->program_name, pidfile_tmp_name, strerror(errno));
			(void) close(pidfile_tmp_fd);
			(void) remove(pidfile_tmp_name);
			free(pidfile_tmp_name);
			pv_state_free(state);
			opts_free(opts);
			return 1;
		}

		fprintf(pidfile_tmp_fptr, "%d\n", getpid());
		if (0 != fclose(pidfile_tmp_fptr)) {
			fprintf(stderr, "%s: %s: %s\n", opts->program_name, opts->pidfile, strerror(errno));
		}

		if (rename(pidfile_tmp_name, opts->pidfile) < 0) {
			fprintf(stderr, "%s: %s: %s\n", opts->program_name, opts->pidfile, strerror(errno));
			(void) remove(pidfile_tmp_name);
		}

		free(pidfile_tmp_name);
	}

	/*
	 * If no files were given, pretend "-" was given (stdin).
	 */
	if (0 == opts->argc) {
		debug("%s", "no files given - adding fake argument `-'");
		if (!opts_add_file(opts, "-")) {
			pv_state_free(state);
			opts_free(opts);
			return 64;
		}
	}

	/*
	 * Put our list of input files into the PV internal state.
	 */
	if (NULL != opts->argv) {
		pv_state_inputfiles(state, opts->argc, (const char **) (opts->argv));
	}

	/* Total size calculation, in normal transfer mode. */
	if (0 == opts->watch_pid) {
		/*
		 * If no size was given, try to calculate the total size.
		 */
		if (0 == opts->size) {
			pv_state_linemode_set(state, opts->linemode);
			pv_state_null_terminated_lines_set(state, opts->null_terminated_lines);
			opts->size = pv_calc_total_size(state);
			debug("%s: %llu", "no size given - calculated", opts->size);
		}

		/*
		 * If the size is unknown, we cannot have an ETA.
		 */
		if (opts->size < 1) {
			opts->eta = false;
			debug("%s", "size unknown - ETA disabled");
		}
	}

	/*
	 * If stderr is not a terminal and we're neither forcing output nor
	 * outputting numerically, we will have nothing to display at all.
	 */
	if ((0 == isatty(STDERR_FILENO))
	    && (false == opts->force)
	    && (false == opts->numeric)) {
		opts->no_display = true;
		debug("%s", "nothing to display - setting no_display");
	}

	/*
	 * Auto-detect width or height if either are unspecified.
	 */
	if ((0 == opts->width) || (0 == opts->height)) {
		unsigned int width, height;
		width = 0;
		height = 0;
		pv_screensize(&width, &height);
		if (0 == opts->width) {
			opts->width = width;
			debug("%s: %u", "auto-detected terminal width", width);
		}
		if (0 == opts->height) {
			opts->height = height;
			debug("%s: %u", "auto-detected terminal height", height);
		}
	}

	/*
	 * Width and height bounds checking (and defaults).
	 */
	if (opts->width < 1)
		opts->width = 80;
	if (opts->height < 1)
		opts->height = 25;
	if (opts->width > 999999)
		opts->width = 999999;
	if (opts->height > 999999)
		opts->height = 999999;

	/*
	 * Interval must be at least 0.1 second, and at most 10 minutes.
	 */
	if (opts->interval < 0.1)
		opts->interval = 0.1;
	if (opts->interval > 600)
		opts->interval = 600;

	/*
	 * Copy parameters from options into main state.
	 */
	pv_state_interval_set(state, opts->interval);
	pv_state_width_set(state, opts->width, opts->width_set_manually);
	pv_state_height_set(state, opts->height, opts->height_set_manually);
	pv_state_no_display_set(state, opts->no_display);
	pv_state_force_set(state, opts->force);
	pv_state_cursor_set(state, opts->cursor);
	pv_state_numeric_set(state, opts->numeric);
	pv_state_wait_set(state, opts->wait);
	pv_state_delay_start_set(state, opts->delay_start);
	pv_state_linemode_set(state, opts->linemode);
	pv_state_bits_set(state, opts->bits);
	pv_state_null_terminated_lines_set(state, opts->null_terminated_lines);
	pv_state_skip_errors_set(state, opts->skip_errors);
	pv_state_error_skip_block_set(state, opts->error_skip_block);
	pv_state_stop_at_size_set(state, opts->stop_at_size);
	pv_state_sync_after_write_set(state, opts->sync_after_write);
	pv_state_direct_io_set(state, opts->direct_io);
	pv_state_discard_input_set(state, opts->discard_input);
	pv_state_rate_limit_set(state, opts->rate_limit);
	pv_state_target_buffer_size_set(state, opts->buffer_size);
	pv_state_no_splice_set(state, opts->no_splice);
	pv_state_size_set(state, opts->size);
	pv_state_name_set(state, opts->name);
	pv_state_format_string_set(state, opts->format);
	pv_state_watch_pid_set(state, opts->watch_pid);
	pv_state_watch_fd_set(state, opts->watch_fd);
	pv_state_average_rate_window_set(state, opts->average_rate_window);

	pv_state_set_format(state, opts->progress, opts->timer, opts->eta,
			    opts->fineta, opts->rate, opts->average_rate,
			    opts->bytes, opts->bufpercent, opts->lastwritten, opts->name);

#ifdef MAKE_STDOUT_NONBLOCKING
	/*
	 * Try and make standard output use non-blocking I/O.
	 *
	 * Note that this can cause problems with (broken) applications
	 * such as dd.
	 */
	fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK | fcntl(STDOUT_FILENO, F_GETFL));
#endif				/* MAKE_STDOUT_NONBLOCKING */

	/* Initialise the signal handling. */
	pv_sig_init(state);

	/* Run the appropriate main loop. */
	if (0 == opts->watch_pid) {
		/* Normal "transfer data" mode. */
		pv_remote_init();
		retcode = pv_main_loop(state);
		pv_remote_fini();
	} else if (0 != opts->watch_pid && -1 == opts->watch_fd) {
		/* "Watch all file descriptors of another process" mode. */
		retcode = pv_watchpid_loop(state);
	} else if (0 != opts->watch_pid && -1 != opts->watch_fd) {
		/* "Watch a specific file descriptor of another process" mode. */
		retcode = pv_watchfd_loop(state);
	}

	/* Clear up the PID file, if one was written. */
	if (opts->pidfile != NULL) {
		if (0 != remove(opts->pidfile)) {
			fprintf(stderr, "%s: %s: %s\n", opts->program_name, opts->pidfile, strerror(errno));
		}
	}

	/* Close down the signal handling. */
	pv_sig_fini(state);

	/* Free the internal PV state. */
	pv_state_free(state);

	/* Free the data from parsing the command-line arguments. */
	opts_free(opts);

	debug("%s: %d", "exiting with status", retcode);

	return retcode;
}

/* EOF */
