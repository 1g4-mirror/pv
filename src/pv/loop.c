/*
 * Functions providing the main transfer or file descriptor watching loop.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define _GNU_SOURCE 1
#include <limits.h>

#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>


/*
 * Pipe data from a list of files to standard output, giving information
 * about the transfer on standard error according to the given options.
 *
 * Returns nonzero on error.
 */
int pv_main_loop(pvstate_t state)
{
	long lineswritten;
	off_t total_written, transferred_since_last, cansend;
	ssize_t written;
	long double target;
	bool eof_in, eof_out, final_update;
	struct timespec start_time, next_update, next_ratecheck, cur_time;
	struct timespec init_time, next_remotecheck, transfer_elapsed;
	long double elapsed_seconds;
	int fd;
	unsigned int file_idx;

	/*
	 * "written" is ALWAYS bytes written by the last transfer.
	 *
	 * "lineswritten" is the lines written by the last transfer,
	 * but is only updated in line mode.
	 *
	 * "total_written" is the total bytes written since the start,
	 * or in line mode, the total lines written since the start.
	 *
	 * "transferred_since_last" is the bytes written since the last
	 * display, or in line mode, the lines written since the last
	 * display.
	 *
	 * The remaining variables are all unchanged by linemode.
	 */

	fd = -1;

	pv_crs_init(state);

	eof_in = false;
	eof_out = false;
	total_written = 0;
	lineswritten = 0;
	transferred_since_last = 0;
	state->display.initial_offset = 0;

	memset(&cur_time, 0, sizeof(cur_time));
	memset(&start_time, 0, sizeof(start_time));

	pv_elapsedtime_read(&cur_time);
	pv_elapsedtime_copy(&start_time, &cur_time);

	memset(&next_ratecheck, 0, sizeof(next_ratecheck));
	memset(&next_remotecheck, 0, sizeof(next_remotecheck));
	memset(&next_update, 0, sizeof(next_update));

	pv_elapsedtime_copy(&next_ratecheck, &cur_time);
	pv_elapsedtime_copy(&next_remotecheck, &cur_time);
	pv_elapsedtime_copy(&next_update, &cur_time);
	if ((state->control.delay_start > 0)
	    && (state->control.delay_start > state->control.interval)) {
		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->control.delay_start));
	} else {
		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->control.interval));
	}

	target = 0;
	final_update = false;
	file_idx = 0;

	/*
	 * Open the first readable input file.
	 */
	fd = -1;
	while (fd < 0 && file_idx < state->files.file_count) {
		fd = pv_next_file(state, file_idx, -1);
		if (fd < 0)
			file_idx++;
	}

	/*
	 * Exit early if there was no readable input file.
	 */
	if (fd < 0) {
		if (state->control.cursor)
			pv_crs_fini(state);
		return state->status.exit_status;
	}
#if HAVE_POSIX_FADVISE
	/* Advise the OS that we will only be reading sequentially. */
	(void) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

#ifdef O_DIRECT
	/*
	 * Set or clear O_DIRECT on the output.
	 */
	if (0 !=
	    fcntl(STDOUT_FILENO, F_SETFL, (state->control.direct_io ? O_DIRECT : 0) | fcntl(STDOUT_FILENO, F_GETFL))) {
		debug("%s: %s", "fcntl", strerror(errno));
	}
	state->control.direct_io_changed = false;
#endif				/* O_DIRECT */

#if HAVE_STRUCT_STAT_ST_BLKSIZE
	/*
	 * Set target buffer size if the initial file's block size can be
	 * read and we weren't given a target buffer size.
	 */
	if (0 == state->control.target_buffer_size) {
		struct stat sb;
		memset(&sb, 0, sizeof(sb));
		if (0 == fstat(fd, &sb)) {
			size_t sz;
			sz = (size_t) (sb.st_blksize * 32);
			if (sz > BUFFER_SIZE_MAX) {
				sz = BUFFER_SIZE_MAX;
			}
			state->control.target_buffer_size = sz;
		}
	}
#endif

	if (0 == state->control.target_buffer_size)
		state->control.target_buffer_size = BUFFER_SIZE;

	while ((!(eof_in && eof_out)) || (!final_update)) {

		cansend = 0;

		/*
		 * Check for remote messages from -R every short while.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_remotecheck) > 0) {
			pv_remote_check(state);
			pv_elapsedtime_add_nsec(&next_remotecheck, REMOTE_INTERVAL);
		}

		if (1 == state->flag.trigger_exit)
			break;

		if (state->control.rate_limit > 0) {
			pv_elapsedtime_read(&cur_time);
			if (pv_elapsedtime_compare(&cur_time, &next_ratecheck) > 0) {
				target +=
				    ((long double) (state->control.rate_limit)) / (long double) (1000000000.0 /
												 (long double)
												 (RATE_GRANULARITY));
				long double burst_max = ((long double) (state->control.rate_limit * RATE_BURST_WINDOW));
				if (target > burst_max) {
					target = burst_max;
				}
				pv_elapsedtime_add_nsec(&next_ratecheck, RATE_GRANULARITY);
			}
			cansend = (off_t) target;
		}

		/*
		 * If we have to stop at "size" bytes, make sure we don't
		 * try to write more than we're allowed to.
		 */
		if ((0 < state->control.size) && (state->control.stop_at_size)) {
			if ((state->control.size < (total_written + cansend))
			    || ((0 == cansend)
				&& (0 == state->control.rate_limit))) {
				cansend = state->control.size - total_written;
				if (0 >= cansend) {
					eof_in = true;
					eof_out = true;
				}
			}
		}

		if ((0 < state->control.size) && (state->control.stop_at_size)
		    && (0 >= cansend) && eof_in && eof_out) {
			written = 0;
		} else {
			written = pv_transfer(state, fd, &eof_in, &eof_out, cansend, &lineswritten);
		}

		/* End on write error. */
		if (written < 0) {
			if (state->control.cursor)
				pv_crs_fini(state);
			return state->status.exit_status;
		}

		if (state->control.linemode) {
			transferred_since_last += lineswritten;
			total_written += lineswritten;
			if (state->control.rate_limit > 0)
				target -= lineswritten;
		} else {
			transferred_since_last += written;
			total_written += written;
			if (state->control.rate_limit > 0)
				target -= written;
		}

		/*
		 * EOF, and files remain - advance to the next file.
		 */
		while (eof_in && eof_out && file_idx < (state->files.file_count - 1)) {
			file_idx++;
			fd = pv_next_file(state, file_idx, fd);
			if (fd >= 0) {
				eof_in = false;
				eof_out = false;
			}
		}

		/* Now check the current time. */
		pv_elapsedtime_read(&cur_time);

		/* If full EOF, final update, and force a display updaate. */
		if (eof_in && eof_out) {
			final_update = true;
			if ((state->display.display_visible)
			    || (state->control.delay_start < 0.001)) {
				pv_elapsedtime_copy(&next_update, &cur_time);
			}
		}

		/* Just go round the loop again if there's no display. */
		if (state->control.no_display)
			continue;

		/*
		 * If -W was given, we don't output anything until we have
		 * written a byte (or line, in line mode), at which point
		 * we then count time as if we started when the first byte
		 * was received.
		 */
		if (state->control.wait) {
			/* Restart the loop if nothing written yet. */
			if (state->control.linemode) {
				if (lineswritten < 1)
					continue;
			} else {
				if (written < 1)
					continue;
			}

			state->control.wait = 0;

			/*
			 * Reset the timer offset counter now that data
			 * transfer has begun, otherwise if we had been
			 * stopped and started (with ^Z / SIGTSTOP)
			 * previously (while waiting for data), the timers
			 * will be wrongly offset.
			 *
			 * While we reset the offset counter we must disable
			 * SIGTSTOP so things don't mess up.
			 */
			pv_sig_nopause();
			pv_elapsedtime_read(&start_time);
			pv_elapsedtime_zero(&(state->signal.toffset));
			pv_sig_allowpause();

			/*
			 * Start the display, but only at the next interval,
			 * not immediately.
			 */
			pv_elapsedtime_copy(&next_update, &start_time);
			pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->control.interval));
		}

		/* Restart the loop if it's not time to update the display. */
		if (pv_elapsedtime_compare(&cur_time, &next_update) < 0) {
			continue;
		}

		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->control.interval));

		/* Set the "next update" time to now, if it's in the past. */
		if (pv_elapsedtime_compare(&next_update, &cur_time) < 0)
			pv_elapsedtime_copy(&next_update, &cur_time);

		/*
		 * Calculate the effective start time: the time we actually
		 * started, plus the total time we spent stopped.
		 */
		memset(&init_time, 0, sizeof(init_time));
		pv_elapsedtime_add(&init_time, &start_time, &(state->signal.toffset));

		/*
		 * Now get the effective elapsed transfer time - current
		 * time minus effective start time.
		 */
		memset(&transfer_elapsed, 0, sizeof(transfer_elapsed));
		pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &init_time);

		elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		if (final_update)
			transferred_since_last = -1;

		/* Resize the display, if a resize signal was received. */
		if (1 == state->flag.terminal_resized) {
			unsigned int new_width, new_height;

			state->flag.terminal_resized = 0;

			new_width = state->control.width;
			new_height = state->control.height;
			pv_screensize(&new_width, &new_height);

			if (!state->control.width_set_manually)
				state->control.width = new_width;
			if (!state->control.height_set_manually)
				state->control.height = new_height;
		}

		pv_display(state, elapsed_seconds, transferred_since_last, total_written);

		transferred_since_last = 0;
	}

	if (state->control.cursor) {
		pv_crs_fini(state);
	} else {
		if ((!state->control.numeric) && (!state->control.no_display)
		    && (state->display.display_visible))
			pv_write_retry(STDERR_FILENO, "\n", 1);
	}

	if (1 == state->flag.trigger_exit)
		state->status.exit_status |= 32;

	if (fd >= 0)
		(void) close(fd);

	return state->status.exit_status;
}


/*
 * Watch the progress of file descriptor state->control.watch_fd in process
 * state->control.watch_pid and show details about the transfer on standard error
 * according to the given options.
 *
 * Returns nonzero on error.
 */
int pv_watchfd_loop(pvstate_t state)
{
	struct pvwatchfd_s info;
	off_t position_now, total_written, transferred_since_last;
	struct timespec next_update, cur_time;
	struct timespec init_time, next_remotecheck, transfer_elapsed;
	long double elapsed_seconds;
	bool ended, first_check;
	int rc;

	memset(&info, 0, sizeof(info));
	info.watch_pid = state->control.watch_pid;
	info.watch_fd = state->control.watch_fd;
	rc = pv_watchfd_info(state, &info, false);
	if (0 != rc) {
		state->status.exit_status |= 2;
		/*@-compdestroy@ */
		return state->status.exit_status;
		/*@+compdestroy@ */
		/* splint: no leak of info.state as it is unused so far. */
	}

	/*
	 * Use a size if one was passed, otherwise use the total size
	 * calculated.
	 */
	if (0 >= state->control.size)
		state->control.size = info.size;

	if (state->control.size < 1) {
		char *fmt;
		while (NULL != (fmt = strstr(state->control.default_format, "%e"))) {
			debug("%s", "zero size - removing ETA");
			/* strlen-1 here to include trailing \0 */
			memmove(fmt, fmt + 2, strlen(fmt) - 1);	/* flawfinder: ignore */
			/* flawfinder: default_format is always \0 terminated */
			state->flag.reparse_display = 1;
		}
	}

	memset(&cur_time, 0, sizeof(cur_time));
	memset(&next_remotecheck, 0, sizeof(next_remotecheck));
	memset(&next_update, 0, sizeof(next_update));
	memset(&init_time, 0, sizeof(init_time));
	memset(&transfer_elapsed, 0, sizeof(transfer_elapsed));

	pv_elapsedtime_read(&cur_time);
	pv_elapsedtime_copy(&(info.start_time), &cur_time);
	pv_elapsedtime_copy(&next_remotecheck, &cur_time);
	pv_elapsedtime_copy(&next_update, &cur_time);
	pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->control.interval));

	ended = false;
	total_written = 0;
	transferred_since_last = 0;
	first_check = true;

	while (!ended) {
		/*
		 * Check for remote messages from -R every short while.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_remotecheck) > 0) {
			pv_remote_check(state);
			pv_elapsedtime_add_nsec(&next_remotecheck, REMOTE_INTERVAL);
		}

		if (1 == state->flag.trigger_exit)
			break;

		position_now = pv_watchfd_position(&info);

		if (position_now < 0) {
			ended = true;
		} else {
			transferred_since_last += position_now - total_written;
			total_written = position_now;
			if (first_check) {
				state->display.initial_offset = position_now;
				first_check = false;
			}
		}

		pv_elapsedtime_read(&cur_time);

		/* Ended - force a display update. */
		if (ended) {
			pv_elapsedtime_copy(&next_update, &cur_time);
		}

		/*
		 * Restart the loop after a brief delay, if it's not time to
		 * update the display.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_update) < 0) {
			pv_nanosleep(50000000);
			continue;
		}

		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->control.interval));

		/* Set the "next update" time to now, if it's in the past. */
		if (pv_elapsedtime_compare(&next_update, &cur_time) < 0)
			pv_elapsedtime_copy(&next_update, &cur_time);

		/*
		 * Calculate the effective start time: the time we actually
		 * started, plus the total time we spent stopped.
		 */
		pv_elapsedtime_add(&init_time, &(info.start_time), &(state->signal.toffset));

		/*
		 * Now get the effective elapsed transfer time - current
		 * time minus effective start time.
		 */
		pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &init_time);

		elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		if (ended)
			transferred_since_last = -1;

		/* Resize the display, if a resize signal was received. */
		if (1 == state->flag.terminal_resized) {
			unsigned int new_width, new_height;

			state->flag.terminal_resized = 0;

			new_width = state->control.width;
			new_height = state->control.height;
			pv_screensize(&new_width, &new_height);

			if (!state->control.width_set_manually)
				state->control.width = new_width;
			if (!state->control.height_set_manually)
				state->control.height = new_height;
		}

		pv_display(state, elapsed_seconds, transferred_since_last, total_written);

		transferred_since_last = 0;
	}

	if (!state->control.numeric)
		pv_write_retry(STDERR_FILENO, "\n", 1);

	if (1 == state->flag.trigger_exit)
		state->status.exit_status |= 32;

	/*
	 * Free the state structure specific to this file descriptor. 
	 * Uunused so far - this is so that in future we could watch
	 * multiple file descriptors similar to pv_watchpid_loop().
	 */
	if (NULL != info.state) {
		pv_state_free(info.state);
		info.state = NULL;
	}

	/*@-compdestroy@ */
	return state->status.exit_status;
	/*@+compdestroy@ */
	/* splint: no leak of info.state as we just freed it above. */
}


/*
 * Watch the progress of all file descriptors in process state->control.watch_pid
 * and show details about the transfers on standard error according to the
 * given options.
 *
 * Replaces format_string in "state" so that starts with "%N " if it doesn't
 * already do so.
 *
 * Returns nonzero on error.
 */
int pv_watchpid_loop(pvstate_t state)
{
	const char *original_format_string;
	char new_format_string[512];	 /* flawfinder: ignore */
	struct pvwatchfd_s *info_array = NULL;
	int array_length = 0;
	int fd_to_idx[FD_SETSIZE];
	struct timespec next_update, cur_time;
	int idx;
	int prev_displayed_lines, blank_lines;
	bool first_pass = true;

	/*
	 * flawfinder rationale (new_format_string): zeroed with memset(),
	 * only written to with pv_snprintf() which checks boundaries, and
	 * explicitly terminated with \0.
	 */

	/*
	 * Make sure the process exists first, so we can give an error if
	 * it's not there at the start.
	 */
	if (kill(state->control.watch_pid, 0) != 0) {
		pv_error(state, "%s %u: %s", _("pid"), state->control.watch_pid, strerror(errno));
		state->status.exit_status |= 2;
		return 2;
	}

	/*
	 * Make sure there's a format string, and then insert %N into it if
	 * it's not present.
	 */
	original_format_string =
	    NULL != state->control.format_string ? state->control.format_string : state->control.default_format;
	memset(new_format_string, 0, sizeof(new_format_string));
	if (NULL == original_format_string) {
		(void) pv_snprintf(new_format_string, sizeof(new_format_string), "%%N");
	} else if (NULL == strstr(original_format_string, "%N")) {
		(void) pv_snprintf(new_format_string, sizeof(new_format_string), "%%N %s", original_format_string);
	} else {
		(void) pv_snprintf(new_format_string, sizeof(new_format_string), "%s", original_format_string);
	}
	new_format_string[sizeof(new_format_string) - 1] = '\0';
	if (NULL != state->control.format_string)
		free(state->control.format_string);
	state->control.format_string = pv_strdup(new_format_string);

	/*
	 * Get things ready for the main loop.
	 */

	memset(&cur_time, 0, sizeof(cur_time));
	memset(&next_update, 0, sizeof(next_update));

	pv_elapsedtime_read(&cur_time);
	pv_elapsedtime_copy(&next_update, &cur_time);
	pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->control.interval));

	for (idx = 0; idx < FD_SETSIZE; idx++) {
		fd_to_idx[idx] = -1;
	}

	prev_displayed_lines = 0;

	while (true) {
		int rc, fd, displayed_lines;

		if (1 == state->flag.trigger_exit)
			break;

		pv_elapsedtime_read(&cur_time);

		if (kill(state->control.watch_pid, 0) != 0) {
			if (first_pass) {
				pv_error(state, "%s %u: %s", _("pid"), state->control.watch_pid, strerror(errno));
				state->status.exit_status |= 2;
				if (NULL != info_array)
					free(info_array);
				return 2;
			}
			break;
		}

		/*
		 * Restart the loop after a brief delay, if it's not time to
		 * update the display.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_update) < 0) {
			pv_nanosleep(50000000);
			continue;
		}

		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->control.interval));

		/* Set the "next update" time to now, if it's in the past. */
		if (pv_elapsedtime_compare(&next_update, &cur_time) < 0)
			pv_elapsedtime_copy(&next_update, &cur_time);

		/* Resize the display, if a resize signal was received. */
		if (1 == state->flag.terminal_resized) {
			state->flag.terminal_resized = 0;
			pv_screensize(&(state->control.width), &(state->control.height));
			for (idx = 0; NULL != info_array && idx < array_length; idx++) {
				if (NULL == info_array[idx].state)
					continue;
				info_array[idx].state->control.width = state->control.width;
				info_array[idx].state->control.height = state->control.height;
				pv_watchpid_setname(state, &(info_array[idx]));
				info_array[idx].state->flag.reparse_display = 1;
			}
		}

		rc = pv_watchpid_scanfds(state, state->control.watch_pid, &array_length, &info_array, fd_to_idx);
		if (rc != 0) {
			if (first_pass) {
				pv_error(state, "%s %u: %s", _("pid"), state->control.watch_pid, strerror(errno));
				state->status.exit_status |= 2;
				if (NULL != info_array)
					free(info_array);
				return 2;
			}
			break;
		}

		first_pass = false;
		displayed_lines = 0;

		for (fd = 0; fd < FD_SETSIZE && NULL != info_array; fd++) {
			off_t position_now, transferred_since_last;
			struct timespec init_time, transfer_elapsed;
			long double elapsed_seconds;

			if (displayed_lines >= (int) (state->control.height))
				break;

			idx = fd_to_idx[fd];

			if (idx < 0)
				continue;

			if (info_array[idx].watch_fd < 0) {
				/*
				 * Non-displayable fd - just remove if
				 * changed
				 */
				if (pv_watchfd_changed(&(info_array[idx]))) {
					fd_to_idx[fd] = -1;
					info_array[idx].watch_pid = 0;
					debug("%s %d: %s", "fd", fd, "removing");
				}
				continue;
			}

			if (NULL == info_array[idx].state) {
				debug("%s %d: %s", "fd", fd, "null state - skipping");
				continue;
			}

			/*
			 * Displayable fd - display, or remove if changed
			 */

			position_now = pv_watchfd_position(&(info_array[idx]));

			if (position_now < 0) {
				fd_to_idx[fd] = -1;
				info_array[idx].watch_pid = 0;
				debug("%s %d: %s", "fd", fd, "removing");
				continue;
			}

			transferred_since_last = position_now - info_array[idx].position;
			info_array[idx].position = position_now;

			memset(&init_time, 0, sizeof(init_time));
			memset(&transfer_elapsed, 0, sizeof(transfer_elapsed));

			/*
			 * Calculate the effective start time: the time we actually
			 * started, plus the total time we spent stopped.
			 */
			pv_elapsedtime_add(&init_time, &(info_array[idx].start_time), &(state->signal.toffset));

			/*
			 * Now get the effective elapsed transfer time - current
			 * time minus effective start time.
			 */
			pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &init_time);

			elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

			if (displayed_lines > 0) {
				debug("%s", "adding newline");
				pv_write_retry(STDERR_FILENO, "\n", 1);
			}

			debug("%s %d [%d]: %Lf / %Ld / %Ld", "fd", fd, idx, elapsed_seconds, transferred_since_last,
			      position_now);

			pv_display(info_array[idx].state, elapsed_seconds, transferred_since_last, position_now);
			displayed_lines++;
		}

		/*
		 * Write blank lines if we're writing fewer lines than last
		 * time.
		 */
		blank_lines = prev_displayed_lines - displayed_lines;
		prev_displayed_lines = displayed_lines;

		if (blank_lines > 0)
			debug("%s: %d", "adding blank lines", blank_lines);

		while (blank_lines > 0) {
			unsigned int x;
			if (displayed_lines > 0)
				pv_write_retry(STDERR_FILENO, "\n", 1);
			for (x = 0; x < state->control.width; x++)
				pv_write_retry(STDERR_FILENO, " ", 1);
			pv_write_retry(STDERR_FILENO, "\r", 1);
			blank_lines--;
			displayed_lines++;
		}

		debug("%s: %d", "displayed lines", displayed_lines);

		while (displayed_lines > 1) {
			pv_write_retry(STDERR_FILENO, "\033[A", 3);
			displayed_lines--;
		}
	}

	/*
	 * Clean up our displayed lines on exit.
	 */
	blank_lines = prev_displayed_lines;
	while (blank_lines > 0) {
		unsigned int x;
		for (x = 0; x < state->control.width; x++)
			pv_write_retry(STDERR_FILENO, " ", 1);
		pv_write_retry(STDERR_FILENO, "\r", 1);
		blank_lines--;
		if (blank_lines > 0)
			pv_write_retry(STDERR_FILENO, "\n", 1);
	}
	while (prev_displayed_lines > 1) {
		pv_write_retry(STDERR_FILENO, "\033[A", 3);
		prev_displayed_lines--;
	}

	/*
	 * Free the per-fd state.
	 */
	for (idx = 0; NULL != info_array && idx < array_length; idx++) {
		if (NULL == info_array[idx].state)
			continue;
		pv_state_free(info_array[idx].state);
		info_array[idx].state = NULL;
	}

	if (NULL != info_array)
		free(info_array);

	return 0;
}

/* EOF */
