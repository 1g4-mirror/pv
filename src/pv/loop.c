/*
 * Functions providing the main transfer or file descriptor watching loop.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * Distributed under the Artistic License v2.0; see `doc/COPYING'.
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
	long written, lineswritten;
	long long total_written, transferred_since_last, cansend;
	long double target;
	int eof_in, eof_out, final_update;
	struct timespec start_time, next_update, next_ratecheck, cur_time;
	struct timespec init_time, next_remotecheck, transfer_elapsed;
	long double elapsed_seconds;
	struct stat sb;
	int fd, file_idx;

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

	eof_in = 0;
	eof_out = 0;
	total_written = 0;
	transferred_since_last = 0;
	state->initial_offset = 0;

	pv_elapsedtime_read(&cur_time);
	pv_elapsedtime_copy(&start_time, &cur_time);

	pv_elapsedtime_copy(&next_ratecheck, &cur_time);
	pv_elapsedtime_copy(&next_remotecheck, &cur_time);
	pv_elapsedtime_copy(&next_update, &cur_time);
	if ((state->delay_start > 0)
	    && (state->delay_start > state->interval)) {
		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->delay_start));
	} else {
		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->interval));
	}

	target = 0;
	final_update = 0;
	file_idx = 0;

	/*
	 * Open the first readable input file.
	 */
	fd = -1;
	while (fd < 0 && file_idx < state->input_file_count) {
		fd = pv_next_file(state, file_idx, -1);
		if (fd < 0)
			file_idx++;
	}

	/*
	 * Exit early if there was no readable input file.
	 */
	if (fd < 0) {
		if (state->cursor)
			pv_crs_fini(state);
		return state->exit_status;
	}
#if HAVE_POSIX_FADVISE
	/* Advise the OS that we will only be reading sequentially. */
	(void) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

#ifdef O_DIRECT
	/*
	 * Set or clear O_DIRECT on the output.
	 */
	fcntl(STDOUT_FILENO, F_SETFL, (state->direct_io ? O_DIRECT : 0) | fcntl(STDOUT_FILENO, F_GETFL));
	state->direct_io_changed = false;
#endif				/* O_DIRECT */

	/*
	 * Set target buffer size if the initial file's block size can be
	 * read and we weren't given a target buffer size.
	 */
	if ((0 == fstat(fd, &sb)) && (0 == state->target_buffer_size)) {
		unsigned long long sz;
		sz = sb.st_blksize * 32;
		if (sz > BUFFER_SIZE_MAX)
			sz = BUFFER_SIZE_MAX;
		state->target_buffer_size = sz;
	}

	if (0 == state->target_buffer_size)
		state->target_buffer_size = BUFFER_SIZE;

	while ((!(eof_in && eof_out)) || (!final_update)) {

		cansend = 0;

		/*
		 * Check for remote messages from -R every short while.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_remotecheck) > 0) {
			pv_remote_check(state);
			pv_elapsedtime_add_nsec(&next_remotecheck, REMOTE_INTERVAL);
		}

		if (state->pv_sig_abort)
			break;

		if (state->rate_limit > 0) {
			pv_elapsedtime_read(&cur_time);
			if (pv_elapsedtime_compare(&cur_time, &next_ratecheck) > 0) {
				target +=
				    ((long double) (state->rate_limit)) / (long double) (1000000000.0 /
											 RATE_GRANULARITY);
				long double burst_max = ((long double) (state->rate_limit * RATE_BURST_WINDOW));
				if (target > burst_max) {
					target = burst_max;
				}
				pv_elapsedtime_add_nsec(&next_ratecheck, RATE_GRANULARITY);
			}
			cansend = target;
		}

		/*
		 * If we have to stop at "size" bytes, make sure we don't
		 * try to write more than we're allowed to.
		 */
		if ((0 < state->size) && (state->stop_at_size)) {
			if (((long) (state->size) < (total_written + cansend))
			    || ((0 == cansend)
				&& (0 == state->rate_limit))) {
				cansend = state->size - total_written;
				if (0 >= cansend) {
					eof_in = 1;
					eof_out = 1;
				}
			}
		}

		if ((0 < state->size) && (state->stop_at_size)
		    && (0 >= cansend) && eof_in && eof_out) {
			written = 0;
		} else {
			written = pv_transfer(state, fd, &eof_in, &eof_out, cansend, &lineswritten);
		}

		if (written < 0) {
			if (state->cursor)
				pv_crs_fini(state);
			return state->exit_status;
		}

		if (state->linemode) {
			transferred_since_last += lineswritten;
			total_written += lineswritten;
			if (state->rate_limit > 0)
				target -= lineswritten;
		} else {
			transferred_since_last += written;
			total_written += written;
			if (state->rate_limit > 0)
				target -= written;
		}

		/*
		 * EOF, and files remain - advance to the next file.
		 */
		while (eof_in && eof_out && file_idx < (state->input_file_count - 1)) {
			file_idx++;
			fd = pv_next_file(state, file_idx, fd);
			if (fd >= 0) {
				eof_in = 0;
				eof_out = 0;
			}
		}

		/* Now check the current time. */
		pv_elapsedtime_read(&cur_time);

		/* If full EOF, final update, and force a display updaate. */
		if (eof_in && eof_out) {
			final_update = 1;
			if ((state->display_visible)
			    || (0 == state->delay_start)) {
				pv_elapsedtime_copy(&next_update, &cur_time);
			}
		}

		/* Just go round the loop again if there's no display. */
		if (state->no_display)
			continue;

		/*
		 * If -W was given, we don't output anything until we have
		 * written a byte (or line, in line mode), at which point
		 * we then count time as if we started when the first byte
		 * was received.
		 */
		if (state->wait) {
			/* Restart the loop if nothing written yet. */
			if (state->linemode) {
				if (lineswritten < 1)
					continue;
			} else {
				if (written < 1)
					continue;
			}

			state->wait = 0;

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
			pv_elapsedtime_zero(&(state->pv_sig_toffset));
			pv_sig_allowpause();

			/*
			 * Start the display, but only at the next interval,
			 * not immediately.
			 */
			pv_elapsedtime_copy(&next_update, &start_time);
			pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->interval));
		}

		/* Restart the loop if it's not time to update the display. */
		if (pv_elapsedtime_compare(&cur_time, &next_update) < 0) {
			continue;
		}

		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->interval));

		/* Set the "next update" time to now, if it's in the past. */
		if (pv_elapsedtime_compare(&next_update, &cur_time) < 0)
			pv_elapsedtime_copy(&next_update, &cur_time);

		/*
		 * Calculate the effective start time: the time we actually
		 * started, plus the total time we spent stopped.
		 */
		pv_elapsedtime_add(&init_time, &start_time, &(state->pv_sig_toffset));

		/*
		 * Now get the effective elapsed transfer time - current
		 * time minus effective start time.
		 */
		pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &init_time);

		elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		if (final_update)
			transferred_since_last = -1;

		/* Resize the display, if a resize signal was received. */
		if (state->pv_sig_newsize) {
			unsigned int new_width, new_height;

			state->pv_sig_newsize = 0;

			new_width = state->width;
			new_height = state->height;
			pv_screensize(&new_width, &new_height);

			if (!state->width_set_manually)
				state->width = new_width;
			if (!state->height_set_manually)
				state->height = new_height;
		}

		pv_display(state, elapsed_seconds, transferred_since_last, total_written);

		transferred_since_last = 0;
	}

	if (state->cursor) {
		pv_crs_fini(state);
	} else {
		if ((!state->numeric) && (!state->no_display)
		    && (state->display_visible))
			pv_write_retry(STDERR_FILENO, "\n", 1);
	}

	if (state->pv_sig_abort)
		state->exit_status |= 32;

	if (fd >= 0)
		close(fd);

	return state->exit_status;
}


/*
 * Watch the progress of file descriptor state->watch_fd in process
 * state->watch_pid and show details about the transfer on standard error
 * according to the given options.
 *
 * Returns nonzero on error.
 */
int pv_watchfd_loop(pvstate_t state)
{
	struct pvwatchfd_s info;
	long long position_now, total_written, transferred_since_last;
	struct timespec next_update, cur_time;
	struct timespec init_time, next_remotecheck, transfer_elapsed;
	long double elapsed_seconds;
	int ended;
	int first_check;
	int rc;

	info.watch_pid = state->watch_pid;
	info.watch_fd = state->watch_fd;
	rc = pv_watchfd_info(state, &info, 0);
	if (0 != rc) {
		state->exit_status |= 2;
		return state->exit_status;
	}

	/*
	 * Use a size if one was passed, otherwise use the total size
	 * calculated.
	 */
	if (0 >= state->size)
		state->size = info.size;

	if (state->size < 1) {
		char *fmt;
		while (NULL != (fmt = strstr(state->default_format, "%e"))) {
			debug("%s", "zero size - removing ETA");
			/* strlen-1 here to include trailing NUL */
			memmove(fmt, fmt + 2, strlen(fmt) - 1);
			state->reparse_display = 1;
		}
	}

	pv_elapsedtime_read(&cur_time);
	pv_elapsedtime_copy(&(info.start_time), &cur_time);
	pv_elapsedtime_copy(&next_remotecheck, &cur_time);
	pv_elapsedtime_copy(&next_update, &cur_time);
	pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->interval));

	ended = 0;
	total_written = 0;
	transferred_since_last = 0;
	first_check = 1;

	while (!ended) {
		/*
		 * Check for remote messages from -R every short while.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_remotecheck) > 0) {
			pv_remote_check(state);
			pv_elapsedtime_add_nsec(&next_remotecheck, REMOTE_INTERVAL);
		}

		if (state->pv_sig_abort)
			break;

		position_now = pv_watchfd_position(&info);

		if (position_now < 0) {
			ended = 1;
		} else {
			transferred_since_last += position_now - total_written;
			total_written = position_now;
			if (first_check) {
				state->initial_offset = position_now;
				first_check = 0;
			}
		}

		pv_elapsedtime_read(&cur_time);

		/* Ended - force a display update. */
		if (ended) {
			ended = 1;
			pv_elapsedtime_copy(&next_update, &cur_time);
		}

		/*
		 * Restart the loop after a brief delay, if it's not time to
		 * update the display.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_update) < 0) {
			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 50000;
			select(0, NULL, NULL, NULL, &tv);
			continue;
		}

		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->interval));

		/* Set the "next update" time to now, if it's in the past. */
		if (pv_elapsedtime_compare(&next_update, &cur_time) < 0)
			pv_elapsedtime_copy(&next_update, &cur_time);

		/*
		 * Calculate the effective start time: the time we actually
		 * started, plus the total time we spent stopped.
		 */
		pv_elapsedtime_add(&init_time, &(info.start_time), &(state->pv_sig_toffset));

		/*
		 * Now get the effective elapsed transfer time - current
		 * time minus effective start time.
		 */
		pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &init_time);

		elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		if (ended)
			transferred_since_last = -1;

		/* Resize the display, if a resize signal was received. */
		if (state->pv_sig_newsize) {
			unsigned int new_width, new_height;

			state->pv_sig_newsize = 0;

			new_width = state->width;
			new_height = state->height;
			pv_screensize(&new_width, &new_height);

			if (!state->width_set_manually)
				state->width = new_width;
			if (!state->height_set_manually)
				state->height = new_height;
		}

		pv_display(state, elapsed_seconds, transferred_since_last, total_written);

		transferred_since_last = 0;
	}

	if (!state->numeric)
		pv_write_retry(STDERR_FILENO, "\n", 1);

	if (state->pv_sig_abort)
		state->exit_status |= 32;

	return state->exit_status;
}


/*
 * Watch the progress of all file descriptors in process state->watch_pid
 * and show details about the transfers on standard error according to the
 * given options.
 *
 * Returns nonzero on error.
 */
int pv_watchpid_loop(pvstate_t state)
{
	struct pvstate_s state_copy;
	const char *original_format_string;
	char new_format_string[512] = { 0, };
	struct pvwatchfd_s *info_array = NULL;
	struct pvstate_s *state_array = NULL;
	int array_length = 0;
	int fd_to_idx[FD_SETSIZE] = { 0, };
	struct timespec next_update, cur_time;
	int idx;
	int prev_displayed_lines, blank_lines;
	int first_pass = 1;

	/*
	 * Make sure the process exists first, so we can give an error if
	 * it's not there at the start.
	 */
	if (kill(state->watch_pid, 0) != 0) {
		pv_error(state, "%s %u: %s", _("pid"), state->watch_pid, strerror(errno));
		state->exit_status |= 2;
		return 2;
	}

	/*
	 * Make a copy of our state, ready to change in preparation for
	 * duplication.
	 */
	memcpy(&state_copy, state, sizeof(state_copy));

	/*
	 * Make sure there's a format string, and then insert %N into it if
	 * it's not present.
	 */
	original_format_string = state->format_string ? state->format_string : state->default_format;
	if (NULL == strstr(original_format_string, "%N")) {
		(void) pv_snprintf(new_format_string, sizeof(new_format_string), "%%N %s", original_format_string);
	} else {
		(void) pv_snprintf(new_format_string, sizeof(new_format_string), "%s", original_format_string);
	}
	new_format_string[sizeof(new_format_string) - 1] = '\0';
	state_copy.format_string = NULL;
	(void) pv_snprintf(state_copy.default_format, PV_SIZEOF_DEFAULT_FORMAT, "%.510s", new_format_string);
	state_copy.default_format[PV_SIZEOF_DEFAULT_FORMAT - 1] = '\0';

	/*
	 * Get things ready for the main loop.
	 */

	pv_elapsedtime_read(&cur_time);
	pv_elapsedtime_copy(&next_update, &cur_time);
	pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->interval));

	for (idx = 0; idx < FD_SETSIZE; idx++) {
		fd_to_idx[idx] = -1;
	}

	prev_displayed_lines = 0;

	while (1) {
		int rc, fd, displayed_lines;

		if (state->pv_sig_abort)
			break;

		pv_elapsedtime_read(&cur_time);

		if (kill(state->watch_pid, 0) != 0) {
			if (first_pass) {
				pv_error(state, "%s %u: %s", _("pid"), state->watch_pid, strerror(errno));
				state->exit_status |= 2;
				if (NULL != info_array)
					free(info_array);
				if (NULL != state_array)
					free(state_array);
				return 2;
			}
			break;
		}

		/*
		 * Restart the loop after a brief delay, if it's not time to
		 * update the display.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_update) < 0) {
			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 50000;
			select(0, NULL, NULL, NULL, &tv);
			continue;
		}

		pv_elapsedtime_add_nsec(&next_update, (long long) (1000000000.0 * state->interval));

		/* Set the "next update" time to now, if it's in the past. */
		if (pv_elapsedtime_compare(&next_update, &cur_time) < 0)
			pv_elapsedtime_copy(&next_update, &cur_time);

		/* Resize the display, if a resize signal was received. */
		if (state->pv_sig_newsize) {
			state->pv_sig_newsize = 0;
			pv_screensize(&(state->width), &(state->height));
			for (idx = 0; idx < array_length; idx++) {
				state_array[idx].width = state->width;
				state_array[idx].height = state->height;
				pv_watchpid_setname(state, &(info_array[idx]));
				state_array[idx].reparse_display = 1;
			}
		}

		rc = pv_watchpid_scanfds(state, &state_copy,
					 state->watch_pid, &array_length, &info_array, &state_array, fd_to_idx);
		if (rc != 0) {
			if (first_pass) {
				pv_error(state, "%s %u: %s", _("pid"), state->watch_pid, strerror(errno));
				state->exit_status |= 2;
				if (NULL != info_array)
					free(info_array);
				if (NULL != state_array)
					free(state_array);
				return 2;
			}
			break;
		}

		first_pass = 0;
		displayed_lines = 0;

		for (fd = 0; fd < FD_SETSIZE; fd++) {
			long long position_now, transferred_since_last;
			struct timespec init_time, transfer_elapsed;
			long double elapsed_seconds;

			if (displayed_lines >= (int) (state->height))
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

			/*
			 * Calculate the effective start time: the time we actually
			 * started, plus the total time we spent stopped.
			 */
			pv_elapsedtime_add(&init_time, &(info_array[idx].start_time), &(state->pv_sig_toffset));

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

			pv_display(&(state_array[idx]), elapsed_seconds, transferred_since_last, position_now);
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
			for (x = 0; x < state->width; x++)
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
		for (x = 0; x < state->width; x++)
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

	if (NULL != info_array)
		free(info_array);
	if (NULL != state_array)
		free(state_array);

	return 0;
}

/* EOF */
