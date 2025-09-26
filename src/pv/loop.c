/*
 * Functions providing the main transfer or file descriptor watching loop.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023-2025 Andrew Wood
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

#if HAVE_MATH_H
#include <math.h>
#endif


#if HAVE_SQRTL
#else
/*
 * Square root of a long double.  Adapted from iputils ping/ping_common.c.
 */
static long ldsqrt(long double value)
{
	long double previous = (long double) LLONG_MAX;
	long double result = value;

	if (result > 0) {
		while (result < previous) {
			previous = result;
			result = (result + (value / result)) / 2;
		}
	}

	return result;
}
#endif


/*
 * Pipe data from a list of files to standard output, giving information
 * about the transfer on standard error according to the given options.
 *
 * Returns nonzero on error.
 */
int pv_main_loop(pvstate_t state)
{
	long lineswritten;
	off_t cansend;
	ssize_t written;
	long double target;
	bool eof_in, eof_out, final_update;
	struct timespec start_time, next_update, next_ratecheck, cur_time;
	struct timespec init_time, next_remotecheck, transfer_elapsed;
	int input_fd, output_fd;
	unsigned int file_idx;
	bool output_is_pipe;

	/*
	 * "written" is ALWAYS bytes written by the last transfer.
	 *
	 * "lineswritten" is the lines written by the last transfer,
	 * but is only updated in line mode.
	 *
	 * "state->transfer.total_written" is the total bytes written since
	 * the start, or in line mode, the total lines written since the
	 * start.
	 *
	 * The remaining variables are all unchanged by linemode.
	 */

	input_fd = -1;

	output_fd = state->control.output_fd;
	if (output_fd < 0)
		output_fd = STDOUT_FILENO;

	/* Determine whether the output is a pipe. */
	output_is_pipe = false;
	{
		struct stat sb;
		memset(&sb, 0, sizeof(sb));
		if (0 == fstat(output_fd, &sb)) {
			/*@-type@ */
			if ((sb.st_mode & S_IFMT) == S_IFIFO) {
				output_is_pipe = true;
				debug("%s", "output is a pipe");
			}
			/*@+type@ *//* splint says st_mode is __mode_t, not mode_t */
		} else {
			debug("%s(%d): %s", "fstat", output_fd, strerror(errno));
		}
	}

	/*
	 * Note that we could reduce the size of the output pipe buffer like
	 * this, to avoid the case where we write a load of data and it just
	 * goes into the buffer so we think we're done, but the consumer
	 * takes ages to process it:
	 *
	 *   fcntl(output_fd, F_SETPIPE_SZ, 4096);
	 *
	 * If we can peek at how much has been consumed by the other end, we
	 * don't need to.
	 */

	pv_crs_init(&(state->cursor), &(state->control), &(state->flags));

	eof_in = false;
	eof_out = false;
	state->transfer.total_written = 0;
	lineswritten = 0;
	state->display.initial_offset = 0;
	state->transfer.written_but_not_consumed = 0;

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
	input_fd = -1;
	while (input_fd < 0 && file_idx < state->files.file_count) {
		input_fd = pv_next_file(state, file_idx, -1);
		if (input_fd < 0)
			file_idx++;
	}

	/*
	 * Exit early if there was no readable input file.
	 */
	if (input_fd < 0) {
		if (state->control.cursor)
			pv_crs_fini(&(state->cursor), &(state->control), &(state->flags));
		return state->status.exit_status;
	}
#if HAVE_POSIX_FADVISE
	/* Advise the OS that we will only be reading sequentially. */
	(void) posix_fadvise(input_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

#ifdef O_DIRECT
	/*
	 * Set or clear O_DIRECT on the output.
	 */
	if (0 != fcntl(output_fd, F_SETFL, (state->control.direct_io ? O_DIRECT : 0) | fcntl(output_fd, F_GETFL))) {
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
		if (0 == fstat(input_fd, &sb)) {
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

	/*
	 * Repeat until eof_in is true, eof_out is true, and final_update is
	 * true.
	 */

	while ((!(eof_in && eof_out)) || (!final_update)) {

		cansend = 0;

		/*
		 * Check for remote messages from -R every short while.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_remotecheck) > 0) {
			pv_remote_check(state);
			pv_elapsedtime_add_nsec(&next_remotecheck, REMOTE_INTERVAL);
		}

		if (1 == state->flags.trigger_exit)
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
			if ((state->control.size < (state->transfer.total_written + cansend))
			    || ((0 == cansend)
				&& (0 == state->control.rate_limit))) {
				cansend = state->control.size - state->transfer.total_written;
				if (0 >= cansend) {
					debug("%s", "write limit reached (size explicitly set) - setting EOF flags");
					eof_in = true;
					eof_out = true;
				}
			}
		}

		if ((0 < state->control.size) && (state->control.stop_at_size)
		    && (0 >= cansend) && eof_in && eof_out) {
			written = 0;
		} else {
			written = pv_transfer(state, input_fd, &eof_in, &eof_out, cansend, &lineswritten);
		}

		/* End on write error. */
		if (written < 0) {
			debug("%s: %s", "write error from pv_transfer", strerror(errno));
			if (state->control.cursor)
				pv_crs_fini(&(state->cursor), &(state->control), &(state->flags));
			return state->status.exit_status;
		}

		if (state->control.linemode) {
			state->transfer.total_written += lineswritten;
			if (state->control.rate_limit > 0)
				target -= lineswritten;
		} else {
			state->transfer.total_written += written;
			if (state->control.rate_limit > 0)
				target -= written;
		}

#ifdef FIONREAD
		/*
		 * If writing to a pipe, look at how much is sitting in the
		 * pipe buffer waiting for the receiver to read.
		 */
		if (output_is_pipe) {
			int nbytes;
			nbytes = 0;
			if (0 != state->flags.pipe_closed) {
				if (0 != state->transfer.written_but_not_consumed)
					debug("%s",
					      "clearing written_but_not_consumed because the output pipe was closed");
				state->transfer.written_but_not_consumed = 0;
			} else if (0 == ioctl(output_fd, FIONREAD, &nbytes)) {
				if (nbytes >= 0) {
					if (((size_t) nbytes) != state->transfer.written_but_not_consumed)
						debug("%s: %d", "written_but_not_consumed is now", nbytes);
					state->transfer.written_but_not_consumed = (size_t) nbytes;
				} else {
					debug("%s: %d", "FIONREAD gave a negative byte count", nbytes);
					state->transfer.written_but_not_consumed = 0;
				}
			} else {
				debug("%s(%d,%s): %s", "ioctl", output_fd, "FIONREAD", strerror(errno));
				state->transfer.written_but_not_consumed = 0;
			}
		}
#endif

		state->transfer.transferred = state->transfer.total_written;
		if (output_is_pipe && !state->control.linemode) {
			/*
			 * Writing bytes to a pipe - the amount transferred
			 * to the receiver is the total amount we've
			 * written, minus what's sitting in the pipe buffer
			 * waiting for the receiver to consume it.
			 */
			state->transfer.transferred -= state->transfer.written_but_not_consumed;

		} else if (output_is_pipe && state->control.linemode && state->transfer.written_but_not_consumed > 0
			   && NULL != state->transfer.line_positions) {
			/*
			 * Writing lines to a pipe - similar to above, but
			 * we have to work out how many lines the
			 * yet-to-be-consumed data in the buffer equates to.
			 *
			 * To do this, we walk backwards through our record
			 * of the line positions in the output we've
			 * written.
			 */
			off_t last_consumed_position =
			    state->transfer.last_output_position - state->transfer.written_but_not_consumed;
			size_t lines_not_consumed = 0;
			size_t line_from_end = 0;

			/*
			 * positions[head-1] = position of last separator written
			 * positions[head-2] = position of second last separator written
			 * etc
			 *
			 * We start at [head-1] and go backwards, wrapping
			 * around as it's a circular buffer, stopping at the
			 * length (number of positions stored), or when we
			 * have gone before the last consumed position.
			 */
			for (line_from_end = 0; line_from_end < state->transfer.line_positions_length; line_from_end++) {
				size_t array_index;
				array_index =
				    state->transfer.line_positions_head + state->transfer.line_positions_capacity -
				    line_from_end - 1;
				while (array_index >= state->transfer.line_positions_capacity)
					array_index -= state->transfer.line_positions_capacity;
				if (state->transfer.line_positions[array_index] <= last_consumed_position)
					break;
				lines_not_consumed++;
			}

			debug("%s: %lld -> %lld", "written_but_not_consumed bytes to lines",
			      (unsigned long long) (state->transfer.written_but_not_consumed),
			      (unsigned long long) lines_not_consumed);

			state->transfer.transferred -= lines_not_consumed;
		}

		/*
		 * EOF, and files remain - advance to the next file.
		 */
		while (eof_in && eof_out && file_idx < (state->files.file_count - 1)) {
			file_idx++;
			input_fd = pv_next_file(state, file_idx, input_fd);
			if (input_fd >= 0) {
				eof_in = false;
				eof_out = false;
#if HAVE_POSIX_FADVISE
				/* Advise the OS that we will only be reading sequentially. */
				(void) posix_fadvise(input_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
			}
		}

		/* Now check the current time. */
		pv_elapsedtime_read(&cur_time);

		/*
		 * If we've read everything and written everything, and the
		 * output pipe buffer is empty, then set the final update
		 * flag, and force a display update.
		 */
		if (eof_in && eof_out && 0 == state->transfer.written_but_not_consumed) {
			final_update = true;
			if ((state->display.output_produced)
			    || (state->control.delay_start < 0.001)) {
				pv_elapsedtime_copy(&next_update, &cur_time);
			}
		}

		/*
		 * If we've read everything and written everything, and the
		 * output pipe buffer is NOT empty, then pause a short while
		 * so we don't spin in a tight loop waiting for the output
		 * buffer to empty (#164).
		 */
		if (eof_in && eof_out && state->transfer.written_but_not_consumed > 0) {
			debug("%s", "EOF but bytes remain in output pipe - sleeping");
			pv_nanosleep(50000000);
		}

		/*
		 * Just go round the loop again if there's no display and
		 * we're not reporting statistics.
		 */
		if (state->control.no_display && !state->control.show_stats)
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

		state->transfer.elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		/* Resize the display, if a resize signal was received. */
		if (1 == state->flags.terminal_resized) {
			unsigned int new_width, new_height;

			state->flags.terminal_resized = 0;

			new_width = (unsigned int) (state->control.width);
			new_height = state->control.height;
			pv_screensize(&new_width, &new_height);

			if (new_width > PVDISPLAY_WIDTH_MAX)
				new_width = PVDISPLAY_WIDTH_MAX;
			if (!state->control.width_set_manually)
				state->control.width = (pvdisplay_width_t) new_width;
			if (!state->control.height_set_manually)
				state->control.height = new_height;
		}

		if (state->control.no_display) {
			/* If there's no display, calculate rate for the statistics. */
			pv_calculate_transfer_rate(&(state->calc), &(state->transfer), &(state->control),
						   &(state->display), final_update);
		} else {
			/* Produce the display. */
			pv_display(&(state->status), &(state->control), &(state->flags), &(state->transfer),
				   &(state->calc), &(state->cursor), &(state->display), &(state->extra_display),
				   final_update);
		}
	}

	debug("%s: %s=%s, %s=%s", "loop ended", "eof_in", eof_in ? "true" : "false", "eof_out",
	      eof_out ? "true" : "false");

	if (state->control.cursor) {
		pv_crs_fini(&(state->cursor), &(state->control), &(state->flags));
	} else {
		if ((!state->control.numeric) && (!state->control.no_display)
		    && (state->display.output_produced))
			pv_tty_write(&(state->flags), "\n", 1);
	}

	if (1 == state->flags.trigger_exit)
		state->status.exit_status |= PV_ERROREXIT_SIGNAL;

	if (input_fd >= 0)
		(void) close(input_fd);

	/* Calculate and display the transfer statistics. */
	if (state->control.show_stats && state->calc.measurements_taken > 0) {
		char stats_buf[256];	 /* flawfinder: ignore */
		long double rate_mean, rate_variance, rate_deviation;
		int stats_size;

		/* flawfinder: made safe by use of pv_snprintf() */

		rate_mean = state->calc.rate_sum / ((long double) (state->calc.measurements_taken));
		rate_variance =
		    (state->calc.ratesquared_sum / ((long double) (state->calc.measurements_taken))) -
		    (rate_mean * rate_mean);
#if HAVE_SQRTL
		rate_deviation = sqrtl(rate_variance);
#else
		rate_deviation = ldsqrt(rate_variance);
#endif

		debug("%s: %ld", "measurements taken", state->calc.measurements_taken);
		debug("%s: %.3Lf", "rate_sum", state->calc.rate_sum);
		debug("%s: %.3Lf", "ratesquared_sum", state->calc.ratesquared_sum);
		debug("%s: %.3Lf", "rate_mean", rate_mean);
		debug("%s: %.3Lf", "rate_variance", rate_variance);
		debug("%s: %.3Lf", "rate_deviation", rate_deviation);

		memset(stats_buf, 0, sizeof(stats_buf));
		stats_size =
		    pv_snprintf(stats_buf, sizeof(stats_buf), "%s = %.3Lf/%.3Lf/%.3Lf/%.3Lf %s\n",
				_("rate min/avg/max/mdev"), state->calc.rate_min, rate_mean, state->calc.rate_max,
				rate_deviation, state->control.bits ? _("b/s") : _("B/s"));

		if (stats_size > 0 && stats_size < (int) (sizeof(stats_buf)))
			pv_tty_write(&(state->flags), stats_buf, (size_t) stats_size);
	} else if (state->control.show_stats && state->calc.measurements_taken < 1) {
		char msg_buf[256];	 /* flawfinder: ignore */
		int msg_size;

		/* flawfinder: made safe by use of pv_snprintf() */

		memset(msg_buf, 0, sizeof(msg_buf));
		msg_size = pv_snprintf(msg_buf, sizeof(msg_buf), "%s\n", _("rate not measured"));

		if (msg_size > 0 && msg_size < (int) (sizeof(msg_buf)))
			pv_tty_write(&(state->flags), msg_buf, (size_t) msg_size);
	}

	return state->status.exit_status;
}


/*
 * Watch the progress of file descriptor state->watchfd.fd[0] in process
 * state->watch_fd.pid[0] and show details about the transfer on standard
 * error according to the given options.
 *
 * Returns nonzero on error.
 *
 * TODO: unify this with pv_watchpid_loop.
 * TODO: watch more than one fd, if watchfd.count > 1.
 */
int pv_watchfd_loop(pvstate_t state)
{
	struct pvwatchfd_s info;
	off_t position_now;
	struct timespec next_update, cur_time;
	struct timespec init_time, next_remotecheck, transfer_elapsed;
	bool ended, first_check;
	int rc;

	/* If there's nothing to watch - do nothing. */
	if (state->watchfd.count < 1)
		return 0;
	if (NULL == state->watchfd.pid)
		return PV_ERROREXIT_MEMORY;
	if (NULL == state->watchfd.fd)
		return PV_ERROREXIT_MEMORY;

	/* Call pv_watchpid_loop() instead if no specific fd was given. */
	if (-1 == state->watchfd.fd[0]) {
		return pv_watchpid_loop(state);
	}

	memset(&info, 0, sizeof(info));
	info.watch_pid = state->watchfd.pid[0];
	info.watch_fd = state->watchfd.fd[0];
	info.displayable = true;
	info.unused = false;
	pv_reset_watchfd(&info);
	rc = pv_watchfd_info(state, &info, false);
	if (0 != rc) {
		state->status.exit_status |= PV_ERROREXIT_ACCESS;
		pv_freecontents_watchfd(&info);
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
			state->flags.reparse_display = 1;
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
	first_check = true;

	while (!ended) {
		/*
		 * Check for remote messages from -R every short while.
		 */
		if (pv_elapsedtime_compare(&cur_time, &next_remotecheck) > 0) {
			pv_remote_check(state);
			pv_elapsedtime_add_nsec(&next_remotecheck, REMOTE_INTERVAL);
		}

		if (1 == state->flags.trigger_exit)
			break;

		position_now = pv_watchfd_position(&info);

		if (position_now < 0) {
			ended = true;
		} else {
			if (first_check) {
				state->display.initial_offset = position_now;
				first_check = false;
			}
			state->transfer.transferred = position_now;
			state->transfer.total_written = position_now;
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

		state->transfer.elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		/* Resize the display, if a resize signal was received. */
		if (1 == state->flags.terminal_resized) {
			unsigned int new_width, new_height;

			state->flags.terminal_resized = 0;

			new_width = (unsigned int) (state->control.width);
			new_height = state->control.height;
			pv_screensize(&new_width, &new_height);

			if (new_width > PVDISPLAY_WIDTH_MAX)
				new_width = PVDISPLAY_WIDTH_MAX;

			if (!state->control.width_set_manually)
				state->control.width = (pvdisplay_width_t) new_width;
			if (!state->control.height_set_manually)
				state->control.height = new_height;
		}

		pv_display(&(state->status), &(state->control), &(state->flags), &(state->transfer),
			   &(state->calc), &(state->cursor), &(state->display), &(state->extra_display), ended);
	}

	if (!state->control.numeric)
		pv_tty_write(&(state->flags), "\n", 1);

	if (1 == state->flags.trigger_exit)
		state->status.exit_status |= PV_ERROREXIT_SIGNAL;

	pv_freecontents_watchfd(&info);

	/*@-compdestroy@ */
	return state->status.exit_status;
	/*@+compdestroy@ */
	/* splint: no leak of info.state as we just freed it above. */
}


/*
 * Watch the progress of all file descriptors in process
 * state->watchfd.pid[0] and show details about the transfers on standard
 * error according to the given options.
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

	/* If there's nothing to watch - do nothing. */
	if (state->watchfd.count < 1)
		return 0;
	if (NULL == state->watchfd.pid)
		return PV_ERROREXIT_MEMORY;

	/*
	 * Make sure the process exists first, so we can give an error if
	 * it's not there at the start.
	 */
	if (kill(state->watchfd.pid[0], 0) != 0) {
		pv_error("%s %u: %s", _("pid"), state->watchfd.pid[0], strerror(errno));
		state->status.exit_status |= PV_ERROREXIT_ACCESS;
		return PV_ERROREXIT_ACCESS;
	}

	/*
	 * Make sure there's no name set.
	 */
	if (NULL != state->control.name) {
		free(state->control.name);
		state->control.name = NULL;
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

		if (1 == state->flags.trigger_exit)
			break;

		pv_elapsedtime_read(&cur_time);

		if (kill(state->watchfd.pid[0], 0) != 0) {
			if (first_pass) {
				pv_error("%s %u: %s", _("pid"), state->watchfd.pid[0], strerror(errno));
				state->status.exit_status |= PV_ERROREXIT_ACCESS;
				if (NULL != info_array)
					free(info_array);
				return PV_ERROREXIT_ACCESS;
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
		if (1 == state->flags.terminal_resized) {
			unsigned int new_width, new_height;

			state->flags.terminal_resized = 0;

			new_width = (unsigned int) (state->control.width);
			new_height = state->control.height;
			pv_screensize(&new_width, &new_height);

			if (new_width > PVDISPLAY_WIDTH_MAX)
				new_width = PVDISPLAY_WIDTH_MAX;

			state->control.width = (pvdisplay_width_t) new_width;
			state->control.height = new_height;

			for (idx = 0; NULL != info_array && idx < array_length; idx++) {
				if (!info_array[idx].displayable)
					continue;
				pv_watchpid_setname(state, &(info_array[idx]));
				info_array[idx].flags.reparse_display = 1;
			}
		}

		rc = pv_watchpid_scanfds(state, state->watchfd.pid[0], &array_length, &info_array, fd_to_idx);
		if (rc != 0) {
			if (first_pass) {
				pv_error("%s %u: %s", _("pid"), state->watchfd.pid[0], strerror(errno));
				state->status.exit_status |= PV_ERROREXIT_ACCESS;
				if (NULL != info_array)
					free(info_array);
				return PV_ERROREXIT_ACCESS;
			}
			break;
		}

		first_pass = false;
		displayed_lines = 0;

		for (fd = 0; fd < FD_SETSIZE && NULL != info_array; fd++) {
			off_t position_now;
			struct timespec init_time, transfer_elapsed;

			if (displayed_lines >= (int) (state->control.height))
				break;

			idx = fd_to_idx[fd];

			if (idx < 0)
				continue;

			if (info_array[idx].unused) {
				debug("%s %d: %s", "fd", fd, "unused array entry - skipping");
				continue;
			}

			if (!info_array[idx].displayable) {
				/*
				 * Non-displayable fd - just remove if
				 * changed
				 */
				if (pv_watchfd_changed(&(info_array[idx]))) {
					fd_to_idx[fd] = -1;
					info_array[idx].unused = true;
					info_array[idx].displayable = false;
					pv_freecontents_watchfd(&(info_array[idx]));
					debug("%s %d: %s", "fd", fd, "removing");
				}
				continue;
			}

			if (info_array[idx].watch_fd < 0) {
				debug("%s %d: %s", "fd", fd, "negative fd - skipping");
				continue;
			}

			/*
			 * Displayable fd - display, or remove if changed
			 */

			position_now = pv_watchfd_position(&(info_array[idx]));

			if (position_now < 0) {
				fd_to_idx[fd] = -1;
				info_array[idx].unused = true;
				info_array[idx].displayable = false;
				pv_freecontents_watchfd(&(info_array[idx]));
				debug("%s %d: %s", "fd", fd, "removing");
				continue;
			}

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

			info_array[idx].transfer.elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

			if (displayed_lines > 0) {
				debug("%s", "adding newline");
				pv_tty_write(&(state->flags), "\n", 1);
			}

			debug("%s %d [%d]: %Lf / %Ld", "fd", fd, idx,
			      info_array[idx].transfer.elapsed_seconds, position_now);

			if (info_array[idx].watch_fd >= 0) {
				info_array[idx].transfer.transferred = position_now;
				info_array[idx].transfer.total_written = position_now;
				state->control.name = info_array[idx].display_name;
				state->control.size = info_array[idx].size;
				pv_display(&(state->status),
					   &(state->control), &(info_array[idx].flags),
					   &(info_array[idx].transfer), &(info_array[idx].calc),
					   &(state->cursor), &(info_array[idx].display), NULL, false);
				/*@-mustfreeonly@ */
				state->control.name = NULL;
				/*
				 * splint warns of a memory leak, but we'd
				 * set name to be an alias of display_name,
				 * so nothing is lost here.
				 */
				/*@+mustfreeonly@ */
				displayed_lines++;
			}
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
			pvdisplay_width_t blank_count;
			if (displayed_lines > 0)
				pv_tty_write(&(state->flags), "\n", 1);
			for (blank_count = 0; blank_count < state->control.width; blank_count++)
				pv_tty_write(&(state->flags), " ", 1);
			pv_tty_write(&(state->flags), "\r", 1);
			blank_lines--;
			displayed_lines++;
		}

		debug("%s: %d", "displayed lines", displayed_lines);

		while (displayed_lines > 1) {
			pv_tty_write(&(state->flags), "\033[A", 3);
			displayed_lines--;
		}
	}

	/*
	 * Clean up our displayed lines on exit.
	 */
	blank_lines = prev_displayed_lines;
	while (blank_lines > 0) {
		pvdisplay_width_t blank_count;
		for (blank_count = 0; blank_count < state->control.width; blank_count++)
			pv_tty_write(&(state->flags), " ", 1);
		pv_tty_write(&(state->flags), "\r", 1);
		blank_lines--;
		if (blank_lines > 0)
			pv_tty_write(&(state->flags), "\n", 1);
	}
	while (prev_displayed_lines > 1) {
		pv_tty_write(&(state->flags), "\033[A", 3);
		prev_displayed_lines--;
	}

	/*
	 * Free the per-fd state.
	 */
	for (idx = 0; NULL != info_array && idx < array_length; idx++) {
		pv_freecontents_watchfd(&(info_array[idx]));
		info_array[idx].unused = true;
		info_array[idx].displayable = false;
	}

	if (NULL != info_array)
		free(info_array);

	return 0;
}
