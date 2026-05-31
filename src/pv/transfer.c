/*
 * Functions for transferring data between file descriptors.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023-2026 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>

/*
 * splint note: In a few places, "#if SPLINT" is used to substitute other
 * code while analysing with splint, to work around the issues it has with
 * FD_ZERO, FD_SET, FD_ISSET - these macros expand to code it does not like,
 * such as using << with an fd which may be negative, or comparing an
 * unsigned integer with a size_t, and turning off those specific warnings
 * where these macros are used does not work.
 */

/*
 * Return >0 if data is ready to read on fd_in, or write on fd_out, before
 * "usec" microseconds have elapsed, 0 if not, or negative on error.
 *
 * Either or both of "fd_in" and "fd_out" may be negative to ignore that
 * side.
 *
 * If fd_in_ready and/or fd_out_ready are not NULL, they will be populated
 * with true or false depending on whether data is ready on those sides.
 */
static int is_data_ready(int fd_in, /*@null@ */ bool *fd_in_ready, int fd_out, /*@null@ */ bool *fd_out_ready,
			 long usec)
{
	struct timeval tv;
	fd_set readfds;
	fd_set writefds;
	fd_set exceptfds;
	int max_fd;
	int result;

	max_fd = -1;
	if (fd_in > max_fd)
		max_fd = fd_in;
	if (fd_out > max_fd)
		max_fd = fd_out;

	memset(&tv, 0, sizeof(tv));

#if SPLINT
	/* splint doesn't like FD_ZERO and FD_SET. */
	memset(&readfds, 0, sizeof(readfds));
	memset(&writefds, 0, sizeof(writefds));
	memset(&exceptfds, 0, sizeof(exceptfds));
#else				/* !SPLINT */
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_ZERO(&exceptfds);
	if (fd_in >= 0)
		FD_SET(fd_in, &readfds);
	if (fd_out >= 0)
		FD_SET(fd_out, &writefds);
#endif				/* !SPLINT */

	tv.tv_sec = usec / 1000000;
	tv.tv_usec = usec % 1000000;

	if (NULL != fd_in_ready)
		*fd_in_ready = false;
	if (NULL != fd_out_ready)
		*fd_out_ready = false;

	result = select(max_fd + 1, &readfds, &writefds, &exceptfds, &tv);

	if (result > 0) {
		if ((fd_in >= 0) && (NULL != fd_in_ready)
#ifndef SPLINT
		    && (FD_ISSET(fd_in, &readfds))
#endif
		    ) {
			*fd_in_ready = true;
		}
		if ((fd_out >= 0) && (NULL != fd_out_ready)
#ifndef SPLINT
		    && (FD_ISSET(fd_out, &writefds))
#endif
		    ) {
			*fd_out_ready = true;
		}
	}

	return result;
}


/*
 * Read up to "count" bytes from file descriptor "fd" into the buffer "buf",
 * in chunks of no more than MAX_READ_AT_ONCE bytes at a time.
 *
 * Keeps reading while fewer than "count" bytes were read,
 * TRANSFER_READ_TIMEOUT seconds have not yet elapsed, and more data is
 * available to read according to is_data_ready().
 *
 * Sets state->transfer.method to PV_TRANSFERMETHOD_READWRITE as a side
 * effect, so that if this is called as a fallback from another transfer
 * method, the original caller can find out.
 *
 * Returns the total number of bytes read, or negative on error.
 */
static ssize_t pv__transfer__read_repeated(pvstate_t state, int fd, char *buf, size_t count)
{
	struct timespec start_time;
	ssize_t total_read;

	state->transfer.method = PV_TRANSFERMETHOD_READWRITE;

	memset(&start_time, 0, sizeof(start_time));

	pv_elapsedtime_read(&start_time);

	total_read = 0;

	while (count > 0) {
		ssize_t nread;
		struct timespec cur_time, transfer_elapsed;
		long double elapsed_seconds;

		nread = read(fd, buf, (size_t) (count > MAX_READ_AT_ONCE ? MAX_READ_AT_ONCE : count));	/* flawfinder: ignore */

		/*
		 * flawfinder rationale: reads stop after "count" bytes,
		 * negative and zero results from read() are handled, so it
		 * is bounded to the buffer size supplied by the caller.
		 */

		if (nread < 0)
			return nread;

		total_read += nread;
		buf += nread;

		/* Unsigned type - guard against underflow. */
		if (count > (size_t) nread) {
			count -= nread;
		} else {
			count = 0;
		}

		if (0 == nread) {
			debug("%s %d: %s", "fd", fd, "nread=0");
			return total_read;
		}

		memset(&cur_time, 0, sizeof(cur_time));
		memset(&transfer_elapsed, 0, sizeof(transfer_elapsed));
		elapsed_seconds = 0.0;

		pv_elapsedtime_read(&cur_time);
		pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &start_time);
		elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		if (elapsed_seconds > TRANSFER_READ_TIMEOUT) {
			debug("%s %d: %s (%f %s)", "fd", fd,
			      "stopping read - timer expired", (double) elapsed_seconds, "sec elapsed");
			return total_read;
		}

		if (count > 0) {
			debug("%s %d: %s (%ld %s, %lu %s)", "fd", fd,
			      "trying another read after partial buffer fill", nread, "read", count, "remaining");
			if (is_data_ready(fd, NULL, -1, NULL, 0) < 1)
				break;
		}
	}

	return total_read;
}


/*
 * Write up to "count" bytes to file descriptor "fd" from the buffer "buf",
 * in chunks of no more than MAX_WRITE_AT_ONCE bytes at a time.
 *
 * Keeps writing while fewer than "count" bytes were written, and
 * TRANSFER_WRITE_TIMEOUT seconds have not yet elapsed.
 *
 * Although this function is called after a successful write-possible
 * select(), write() is not guaranteed to succeed for _all_ sizes; this
 * function can return 0 if this occurs.  (The first write() may return -1 /
 * EINTR if the consumer doesn't read any data before the timeout and the
 * buffer of whatever stdout is is near-full.) (see
 * https://codeberg.org/ivarch/pv/pulls/93)
 *
 * If "sync_after_write" is true, fdatasync() is called after each write()
 * (or fsync() if _POSIX_SYNCHRONIZED_IO is not > 0).
 *
 * Returns the total number of bytes written, or negative on error.
 */
static ssize_t pv__transfer__write_repeated(int fd, char *buf, size_t count, bool sync_after_write)
{
	struct timespec start_time;
	ssize_t total_written;

	memset(&start_time, 0, sizeof(start_time));

	pv_elapsedtime_read(&start_time);

	total_written = 0;

	while (count > 0) {
		ssize_t nwritten;
		struct timespec cur_time, transfer_elapsed;
		long double elapsed_seconds;
		size_t asked_to_write;

		asked_to_write = count > MAX_WRITE_AT_ONCE ? MAX_WRITE_AT_ONCE : count;

		nwritten = write(fd, buf, asked_to_write);

		if (sync_after_write && nwritten >= 0) {
			/*
			 * Ignore non I/O errors, such as EBADFD (bad file
			 * descriptor), EINVAL (non syncable fd, such as a
			 * pipe), etc - only return an error on EIO.
			 */
#if defined(HAVE_FDATASYNC) && defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
			if ((fdatasync(fd) < 0) && (EIO == errno)) {
				return -1;
			}
#else				/* !HAVE_FDATASYNC et al */
			if ((fsync(fd) < 0) && (EIO == errno)) {
				return -1;
			}
#endif				/* HAVE_FDATASYNC et al */
		}

		if (nwritten < 0) {
			if ((EINTR == errno) || (EAGAIN == errno)) {
				/*
				 * Interrupted by a signal - probably the
				 * alarm or interval timer - so return the
				 * amount written so far.
				 */
				return total_written;
			} else {
				/*
				 * Legitimate error - return negative.
				 */
				return nwritten;
			}
		}

		total_written += nwritten;
		buf += nwritten;

		/* Unsigned type - guard against underflow. */
		if (count > (size_t) nwritten) {
			count -= nwritten;
		} else {
			count = 0;
		}

		if (0 == nwritten)
			return total_written;

		memset(&cur_time, 0, sizeof(cur_time));
		memset(&transfer_elapsed, 0, sizeof(transfer_elapsed));
		elapsed_seconds = 0.0;

		pv_elapsedtime_read(&cur_time);
		pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &start_time);
		elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		if (elapsed_seconds > TRANSFER_WRITE_TIMEOUT) {
			debug("%s %d: %s (%f %s)", "fd", fd,
			      "stopping write - timer expired", (double) elapsed_seconds, "sec elapsed");
			return total_written;
		}

		/*
		 * Running select() here can make PV eat a lot of CPU in
		 * some cases, so instead of using is_data_ready(), go round
		 * the loop again and rely on the alarm or interval timer to
		 * cause EINTR/EAGAIN, and on the elapsed time check, to
		 * prevent endless retries.
		 */
		if (count > 0) {
			debug("%s %d: %s (%ld %s, %lu %s)", "fd", fd,
			      "trying another write after partial buffer flush",
			      nwritten, "written", count, "remaining");

#if 0					    /* removed after 1.6.0 - see comment above */
			if (is_data_ready(-1, NULL, fd, NULL, 0) < 1) {
				break;
			}
#endif				/* end of removed section */
		}
	}

	return total_written;
}


#ifdef HAVE_SPLICE
/*
 * Splice up to "bytes_to_splice" bytes from file descriptor "input_fd" to
 * "output_fd", via the intermediate pipe in state->transfer.
 *
 * Returns the total number of bytes transferred to output_fd, or negative
 * on error.
 *
 * If a negative value is returned and the error had occurred in the second
 * splice from the intermediate pipe to the output_fd, then
 * *post_splice_fail_rewind will have been populated with the amount that
 * was just now spliced in from input_fd, so the caller can rewind
 * input_fd's position if the error was not EINTR/EAGAIN.
 */
static ssize_t pv__transfer__splice_via_intermediate(pvstate_t state, int input_fd, int output_fd,
						     size_t bytes_to_splice, off_t *post_splice_fail_rewind)
{
	int room_in_pipe_buffer;
	size_t bytes_to_splice_in, bytes_to_splice_out;
	ssize_t spliced_out;
	ssize_t spliced_in;

	*post_splice_fail_rewind = 0;

	room_in_pipe_buffer =
	    state->transfer.intermediate_pipe_buffer_size - state->transfer.intermediate_pipe_buffer_used;
	if (room_in_pipe_buffer < 0)
		room_in_pipe_buffer = 0;

	bytes_to_splice_in = bytes_to_splice;
	if (bytes_to_splice_in > (size_t) room_in_pipe_buffer)
		bytes_to_splice_in = (size_t) room_in_pipe_buffer;

	/* Read into the intermediate pipe if it has room in its buffer. */
	spliced_in = 0;
	if (bytes_to_splice_in > 0) {
		/*@-nullpass@ *//*@-type@ *//* splint doesn't know about splice. */
		spliced_in = splice(input_fd, NULL, state->transfer.intermediate_pipe[1], NULL,
				    bytes_to_splice_in, SPLICE_F_MORE);
		/*@+type@ *//*@+nullpass@ */
	}

	/* Early return on error. */
	if (spliced_in < 0) {
		debug("splice(in:%d->%d, %lu): %s", input_fd, state->transfer.intermediate_pipe[1],
		      (unsigned long) bytes_to_splice_in, strerror(errno));
		return spliced_in;
	}

	state->transfer.intermediate_pipe_buffer_used += (int) spliced_in;

	debug("%s(%d->%d): %ld", "spliced_in", input_fd, state->transfer.intermediate_pipe[1], spliced_in);

	/* If the intermediate pipe has nothing in its buffer, return 0. */
	if (state->transfer.intermediate_pipe_buffer_used <= 0) {
		debug("%s", "intermediate pipe buffer empty, returning 0");
		return 0;
	}

	/* Transfer out whatever's in the intermediate pipe buffer. */
	bytes_to_splice_out = (size_t) (state->transfer.intermediate_pipe_buffer_used);
	if (bytes_to_splice_out > bytes_to_splice)
		bytes_to_splice_out = bytes_to_splice;

	/*@-nullpass@ *//*@-type@ *//* splint doesn't know about splice. */
	spliced_out =
	    splice(state->transfer.intermediate_pipe[0], NULL, output_fd, NULL, bytes_to_splice_out, SPLICE_F_MORE);
	/*@+type@ *//*@+nullpass@ */

	if (spliced_out > 0) {
		state->transfer.intermediate_pipe_buffer_used -= (int) spliced_out;
		debug("%s(%d->%d): %ld", "spliced_out", state->transfer.intermediate_pipe[0], output_fd, spliced_out);
	}

	if (spliced_out < 0) {
		debug("splice(out:%d->%d, %lu): %s", state->transfer.intermediate_pipe[0], output_fd,
		      (unsigned long) bytes_to_splice_out, strerror(errno));
		debug("%s=%d", "intermediate_pipe_buffer_used", state->transfer.intermediate_pipe_buffer_used);
		if (spliced_in > 0) {
			*post_splice_fail_rewind = (off_t) spliced_in;
		}
	}

	return spliced_out;
}


/*
 * Splice bytes from file descriptor "input_fd" to "output_fd", possibly via
 * an intermediate input pipe if one is active.
 *
 * The number of bytes spliced is capped to "max_to_read", or if
 * "max_to_write" is greater than zero and less than "max_to_read", capped
 * to "max_to_write".  A "max_to_read" of less than zero indicates no
 * maximum.
 *
 * If state->control.rate_limit_active is true and "max_to_write" is zero,
 * performs no action and returns zero.  Since this looks the same as EOF,
 * the caller must trap for this condition.
 *
 * Keeps reading until the target amount has been spliced or until
 * TRANSFER_READ_TIMEOUT seconds have elapsed, as long as more data is
 * available to read according to is_data_ready().
 *
 * If splice() could not be used, sets state->transfer.splice_failed_fd to
 * input_fd so splice() won't be tried again until the next input file, and
 * then calls pv__transfer__read_repeated() to read up to
 * "fallback_read_amount" bytes into the buffer, returning its result.
 *
 * Sets state->transfer.method to either PV_TRANSFERMETHOD_SPLICE or
 * PV_TRANSFERMETHOD_SPLICE_INTERMEDIATE as a side effect; if it falls back
 * to pv__transfer__read_repeated(), that function then sets
 * state->transfer.method to PV_TRANSFERMETHOD_READWRITE, so the caller can
 * check which transfer method was actually used.
 *
 * Returns the total number of bytes transferred, or negative on error.
 */
static ssize_t pv__transfer__splice_repeated(pvstate_t state, int input_fd, int output_fd, char *buf,
					     size_t fallback_read_amount, off_t max_to_read, off_t max_to_write)
{
	struct timespec start_time;
	size_t bytes_to_splice;
	ssize_t total_spliced;
	bool use_intermediate_pipe;

	debug("%d->%d, fallback_read_amount=%lu, max_to_read=%lld, max_to_write=%lld", input_fd, output_fd,
	      fallback_read_amount, max_to_read, max_to_write);

	/*
	 * Early return via pv__transfer__read_repeated() if splice() is
	 * turned off, or if line mode is active, or if there's no output
	 * fd, or if splice() already failed on this input file descriptor,
	 * or if there's anything waiting in the transfer buffer.
	 */
	if (state->control.no_splice || state->control.linemode || (output_fd < 0)
	    || (input_fd == state->transfer.splice_failed_fd)
	    || (state->transfer.to_write > 0)) {
		return pv__transfer__read_repeated(state, input_fd, buf, fallback_read_amount);
	}

	/*
	 * Cap the transfer amount if applicable.
	 *
	 * Note that max_to_write is an off_t (file size / offset), which
	 * may not fit into a size_t (byte count), so check it against
	 * SIZE_MAX before trying a comparison otherwise on 32-bit systems
	 * it might appear to be 0.
	 */
	/*@-unrecog@ */
	bytes_to_splice = SIZE_MAX;
	if ((max_to_read >= 0) && ((unsigned long long) max_to_read <= (unsigned long long) SIZE_MAX)) {
		bytes_to_splice = (size_t) max_to_read;
	}
	if ((state->control.rate_limit_active || max_to_write > 0)
	    && ((unsigned long long) max_to_write <= (unsigned long long) SIZE_MAX)
	    && ((max_to_read < 0) || ((unsigned long long) max_to_read >= (unsigned long long) SIZE_MAX)
		|| (max_to_read > max_to_write))
	    ) {
		bytes_to_splice = (size_t) max_to_write;
	}
	/*@+unrecog@ *//* splint doesn't know of SIZE_MAX. */

	/* Early return with 0 if the transfer cap is zero. */
	if (0 == bytes_to_splice) {
		debug("bytes_to_splice=%lu", bytes_to_splice);
		return 0;
	}

	/*
	 * Use an intermediate pipe if one is available and that transfer
	 * method is selected.  Either way, update state->transfer.method to
	 * the method being used.
	 */
	use_intermediate_pipe = false;
	if ((PV_TRANSFERMETHOD_SPLICE_INTERMEDIATE == state->transfer.method)
	    && (-1 != state->transfer.intermediate_pipe[0]) && (-1 != state->transfer.intermediate_pipe[1])) {
		use_intermediate_pipe = true;
		state->transfer.method = PV_TRANSFERMETHOD_SPLICE_INTERMEDIATE;
	} else {
		state->transfer.method = PV_TRANSFERMETHOD_SPLICE;
	}

	memset(&start_time, 0, sizeof(start_time));

	pv_elapsedtime_read(&start_time);

	total_spliced = 0;

	debug("bytes_to_splice=%lu", bytes_to_splice);

	while (bytes_to_splice > 0) {
		ssize_t nspliced;
		struct timespec cur_time, transfer_elapsed;
		long double elapsed_seconds;
		off_t post_splice_fail_rewind;

		post_splice_fail_rewind = 0;
		if (use_intermediate_pipe) {
			nspliced =
			    pv__transfer__splice_via_intermediate(state, input_fd, output_fd, bytes_to_splice,
								  &post_splice_fail_rewind);
		} else {
			/*@-nullpass@ *//*@-type@ *//* splint doesn't know about splice. */
			nspliced = splice(input_fd, NULL, output_fd, NULL, bytes_to_splice, SPLICE_F_MORE);
			/*@+type@ *//*@+nullpass@ */
		}

		debug("bytes_to_splice=%lu, nspliced=%ld, use_intermediate_pipe=%s", bytes_to_splice, nspliced,
		      use_intermediate_pipe ? "true" : "false");

		/*
		 * Early return on signal interrupt, returning -1 if nothing
		 * was transferred yet, or the amount transferred otherwise.
		 */
		if (nspliced < 0 && ((EINTR == errno) || (EAGAIN == errno))) {
			if (0 == total_spliced)
				return -1;
			return total_spliced;
		}

		/*
		 * If the splice failed, turn it off for this input file
		 * descriptor.  Then, if nothing has been spliced so far,
		 * return the result of an ordinary read, otherwise return
		 * the amount spliced.
		 */
		if (nspliced < 0) {
			debug("%s %d: %s: %s", "fd", input_fd, "disabling splice after failure", strerror(errno));
			if (post_splice_fail_rewind > 0) {
				debug("%s %d: %s: %ld", "fd", input_fd, "rewinding position by amount spliced in",
				      (long) post_splice_fail_rewind);
				/*
				 * If data was spliced into the intermediate
				 * pipe but not out again, rewind by the
				 * amount spliced in, so that subsequent
				 * reads are coming from the right position.
				 * If the rewind fails, report the error.
				 */
				/*@+longintegral@ *//* splice has issues with __off_t. */
				if (lseek(input_fd, (off_t) (0 - post_splice_fail_rewind), SEEK_CUR) < 0) {
					debug("%s: %s", "lseek", strerror(errno));
					pv_perror("%s", pv_current_file_name(state));
					state->status.exit_status |= PV_ERROREXIT_TRANSFER;
				}
				/*@-longintegral@ */
			}
			state->transfer.splice_failed_fd = input_fd;
			if (0 == total_spliced) {
				return pv__transfer__read_repeated(state, input_fd, buf, fallback_read_amount);
			} else {
				return total_spliced;
			}
		}

		total_spliced += nspliced;
		/* Unsigned type - guard against underflow. */
		if (bytes_to_splice > (size_t) nspliced) {
			bytes_to_splice -= nspliced;
		} else {
			bytes_to_splice = 0;
		}

		/* Early return on EOF. */
		if (0 == nspliced) {
			debug("%s %d: %s (%lu/%lu)", "fd", input_fd, "reached EOF", total_spliced, bytes_to_splice);
			return total_spliced;
		}

		/* Keep trying while data is ready. */
		memset(&cur_time, 0, sizeof(cur_time));
		memset(&transfer_elapsed, 0, sizeof(transfer_elapsed));
		elapsed_seconds = 0.0;

		pv_elapsedtime_read(&cur_time);
		pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &start_time);
		elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		if (elapsed_seconds > TRANSFER_READ_TIMEOUT) {
			debug("%s %d: %s (%f %s)", "fd", input_fd,
			      "stopping splice - timer expired", (double) elapsed_seconds, "sec elapsed");
			return total_spliced;
		}

		if (bytes_to_splice > 0) {
			debug("%s %d: %s (%ld %s, %lu %s)", "fd", input_fd,
			      "trying another splice", nspliced, "transferred this time", bytes_to_splice, "remaining");
			if (is_data_ready(input_fd, NULL, -1, NULL, 0) < 1)
				break;
		}
	}

	return total_spliced;
}
#endif				/* HAVE_SPLICE */


#ifdef HAVE_COPY_FILE_RANGE
/*
 * Copy bytes from file descriptor "input_fd" to "output_fd" using the
 * in-kernel copy function copy_file_range().
 *
 * The number of bytes copied is capped to "max_to_read", or if
 * "max_to_write" is greater than zero and less than "max_to_read", capped
 * to "max_to_write".  A "max_to_read" of less than zero indicates no
 * maximum.
 *
 * If state->control.rate_limit_active is true and "max_to_write" is zero,
 * performs no action and returns zero.  Since this looks the same as EOF,
 * the caller must trap for this condition.
 *
 * Keeps reading until the target amount has been copied or until
 * TRANSFER_READ_TIMEOUT seconds have elapsed, as long as more data is
 * available to read according to is_data_ready().
 *
 * If copy_file_range() could not be used, sets
 * state->transfer.copy_file_range_failed_fd to input_fd so it won't be
 * tried again until the next input file, and then calls
 * pv__transfer__read_repeated() to read up to "fallback_read_amount" bytes
 * into the buffer, returning its result.
 *
 * Sets state->transfer.method to PV_TRANSFERMETHOD_COPY_FILE_RANGE as a
 * side effect; if it falls back to pv__transfer__read_repeated(), that
 * function then sets state->transfer.method to PV_TRANSFERMETHOD_READWRITE,
 * so the caller can check which transfer method was actually used.
 *
 * Returns the total number of bytes transferred, or negative on error.
 */
static ssize_t pv__transfer__copy_file_range_repeated(pvstate_t state, int input_fd, int output_fd, char *buf,
						      size_t fallback_read_amount, off_t max_to_read,
						      off_t max_to_write)
{
	struct timespec start_time;
	size_t bytes_to_copy;
	ssize_t total_copied;

	debug("%d->%d, fallback_read_amount=%lu, max_to_read=%ld, max_to_write=%ld", input_fd, output_fd,
	      fallback_read_amount, max_to_read, max_to_write);

	/*
	 * Early return via pv__transfer__read_repeated() if this feature is
	 * turned off, or if line mode is active, or if there's no output
	 * fd, or if it already failed on this input file descriptor, or if
	 * there's anything waiting in the transfer buffer.
	 */
	if (state->control.no_splice || state->control.linemode || (output_fd < 0)
	    || (input_fd == state->transfer.copy_file_range_failed_fd)
	    || (state->transfer.to_write > 0)) {
		return pv__transfer__read_repeated(state, input_fd, buf, fallback_read_amount);
	}

	/*
	 * Cap the transfer amount if applicable.
	 *
	 * Note that max_to_write is an off_t (file size / offset), which
	 * may not fit into a size_t (byte count), so check it against
	 * SIZE_MAX before trying a comparison otherwise on 32-bit systems
	 * it might appear to be 0.
	 */
	/*@-unrecog@ */
	bytes_to_copy = SIZE_MAX;
	if ((max_to_read >= 0) && ((unsigned long) max_to_read <= (unsigned long) SIZE_MAX)) {
		bytes_to_copy = (size_t) max_to_read;
	}
	if ((state->control.rate_limit_active || max_to_write > 0)
	    && ((unsigned long) max_to_write <= (unsigned long) SIZE_MAX)
	    && ((max_to_read < 0) || ((unsigned long) max_to_read >= (unsigned long) SIZE_MAX)
		|| (max_to_read > max_to_write))
	    ) {
		bytes_to_copy = (size_t) max_to_write;
	}
	/*@+unrecog@ *//* splint doesn't know of SIZE_MAX. */

	/* Early return with 0 if the transfer cap is zero. */
	if (0 == bytes_to_copy) {
		debug("bytes_to_copy=%lu", bytes_to_copy);
		return 0;
	}

	/* Explicitly set the transfer method being used. */
	state->transfer.method = PV_TRANSFERMETHOD_COPY_FILE_RANGE;

	memset(&start_time, 0, sizeof(start_time));

	pv_elapsedtime_read(&start_time);

	total_copied = 0;

	debug("bytes_to_copy=%lu", bytes_to_copy);

	while (bytes_to_copy > 0) {
		ssize_t ncopied;
		struct timespec cur_time, transfer_elapsed;
		long double elapsed_seconds;

		/*@-nullpass@ *//*@-type@ *//* splint doesn't know about copy_file_range. */
		ncopied = copy_file_range(input_fd, NULL, output_fd, NULL, bytes_to_copy, 0);
		/*@+type@ *//*@+nullpass@ */

		debug("bytes_to_copy=%lu, ncopied=%ld", bytes_to_copy, ncopied);

		/*
		 * Early return on signal interrupt, returning -1 if nothing
		 * was transferred yet, or the amount transferred otherwise.
		 */
		if (ncopied < 0 && ((EINTR == errno) || (EAGAIN == errno))) {
			if (0 == total_copied)
				return -1;
			return total_copied;
		}

		/* TODO: possibly check for ENOSPC. */

		/*
		 * If the copy failed, turn it off for this input file
		 * descriptor.  Then, if nothing has been copied so far,
		 * return the result of an ordinary read, otherwise return
		 * the amount copied.
		 */
		if (ncopied < 0) {
			debug("%s %d: %s: %s", "fd", input_fd, "disabling copy_file_range after failure",
			      strerror(errno));
			state->transfer.copy_file_range_failed_fd = input_fd;
			if (0 == total_copied) {
				return pv__transfer__read_repeated(state, input_fd, buf, fallback_read_amount);
			} else {
				return total_copied;
			}
		}

		total_copied += ncopied;
		/* Unsigned type - guard against underflow. */
		if (bytes_to_copy > (size_t) ncopied) {
			bytes_to_copy -= ncopied;
		} else {
			bytes_to_copy = 0;
		}

		/* Early return on EOF. */
		if (0 == ncopied) {
			debug("%s %d: %s (%lu/%lu)", "fd", input_fd, "reached EOF", total_copied, bytes_to_copy);
			return total_copied;
		}

		/* Keep trying while data is ready. */
		memset(&cur_time, 0, sizeof(cur_time));
		memset(&transfer_elapsed, 0, sizeof(transfer_elapsed));
		elapsed_seconds = 0.0;

		pv_elapsedtime_read(&cur_time);
		pv_elapsedtime_subtract(&transfer_elapsed, &cur_time, &start_time);
		elapsed_seconds = pv_elapsedtime_seconds(&transfer_elapsed);

		if (elapsed_seconds > TRANSFER_READ_TIMEOUT) {
			debug("%s %d: %s (%f %s)", "fd", input_fd,
			      "stopping copy_file_range - timer expired", (double) elapsed_seconds, "sec elapsed");
			return total_copied;
		}

		if (bytes_to_copy > 0) {
			debug("%s %d: %s (%ld %s, %lu %s)", "fd", input_fd,
			      "trying another copy_file_range", ncopied, "transferred this time", bytes_to_copy,
			      "remaining");
			if (is_data_ready(input_fd, NULL, -1, NULL, 0) < 1)
				break;
		}
	}

	return total_copied;
}
#endif				/* HAVE_COPY_FILE_RANGE */


/*
 * Read some data from the given file descriptor, updating the state.
 *
 * At most, the number of bytes read will be the number of bytes remaining
 * in the input buffer, capped to the number of bytes left until
 * state->control.size is reached if state->control.stop_at_size is true. 
 * If state->control.rate_limit_active is true, and/or "max_to_write" is >0,
 * and a non-read/write transfer method (splice or copy_file_range) is used,
 * then the maximum number of bytes read will be further capped to the value
 * of "max_to_write", since those functions write as well as read.
 *
 * If a read/write transfer method was used, updates
 * state->transfer.read_position by the number of bytes read.  If not, the
 * amount read by splice/copy_file_range is placed in
 * state->transfer.written since that amount will have been written to the
 * output.
 *
 * Also increases state->transfer.total_bytes_read by the number of bytes
 * read, regardless of the transfer method, since total_bytes_read is what
 * it says it is, rather than being a buffer indicator.
 *
 * If there is a non-transient read error, updates
 * state->status.exit_status, and tries to skip past the problem if
 * state->control.skip_errors is non-zero.
 *
 * If the end of the input file is reached or the error is unrecoverable,
 * sets *eof_in to true.  If all data in the buffer has been written at this
 * point, then also sets *eof_out to true.
 *
 * Returns true if the transfer can continue normally (meaning some data was
 * transferred, or there was an error or an EOF that has caused the state to
 * be updated).
 *
 * Returns false if there was a transient error and so the transfer should
 * not continue, but be retried shortly instead.
 */
static bool pv__transfer_read(pvstate_t state, int input_fd, bool *eof_in, bool *eof_out, off_t max_to_write)
{
	bool do_not_skip_errors;
	bool zero_transfer_cap;
	size_t max_buffer_available;
	off_t max_to_read, max_to_read_into_buffer;
	off_t amount_to_skip, amount_skipped, orig_offset, skip_offset;
	ssize_t nread;
	int output_fd;

	output_fd = state->control.output_fd;
#ifdef HAVE_SPLICE
	if (state->control.discard_input && !state->control.no_splice)
		output_fd = state->transfer.discard_fd;
#endif				/* HAVE_SPLICE */

	do_not_skip_errors = false;
	if (0 == state->control.skip_errors)
		do_not_skip_errors = true;

	max_buffer_available = state->transfer.buffer_size - state->transfer.read_position;
	max_to_read = -1;

	/*
	 * Don't read past control.size if stop_at_size is true (issue
	 * #166).
	 *
	 * This isn't workable in line mode.
	 */
	if (state->control.stop_at_size && !state->control.linemode) {
		max_to_read = state->control.size - state->transfer.total_bytes_read;
	}

	nread = 0;

	/*
	 * Flag if the amount to read is going to be zero, so that a
	 * zero-sized read is not then mistakenly taken as EOF.
	 */
	zero_transfer_cap = false;
	if (0 == max_to_read)
		zero_transfer_cap = true;

	/*
	 * Restricted version of max_to_read limited to the available buffer
	 * space, to be used by the read/write transfer method.
	 */
	max_to_read_into_buffer = max_to_read;
	/* Clamp to a maximum of available buffer space. */
	if (max_to_read_into_buffer > 0 && max_to_read_into_buffer > (off_t) max_buffer_available)
		max_to_read_into_buffer = (off_t) max_buffer_available;
	/* If "unlimited" (-1), use the maximum buffer available. */
	if (max_to_read_into_buffer < 0)
		max_to_read_into_buffer = (off_t) max_buffer_available;
	/* Clamp to a minimum of 0 in case max_buffer_available underflowed. */
	if (max_to_read_into_buffer < 0)
		max_to_read_into_buffer = 0;

	/* Determine which transfer method to attempt. */
	state->transfer.method = PV_TRANSFERMETHOD_READWRITE;

#ifdef HAVE_SPLICE
	/*
	 * Attempt to use splice() only if splice() is not turned off, and
	 * line mode is inactive, and there's an output fd, and there's
	 * nothing waiting to be written from the transfer buffer, and
	 * splice() hasn't already failed on this input file descriptor.
	 */
	if (!(state->control.no_splice || state->control.linemode || (output_fd < 0)
	      || (state->transfer.to_write > 0)
	      || (input_fd == state->transfer.splice_failed_fd)
	    )) {
		state->transfer.method = PV_TRANSFERMETHOD_SPLICE;
		/*
		 * Splice through an intermediate input pipe if neither the
		 * input nor the output is a pipe, and an intermediate pipe
		 * is ready to use.
		 */
		if (!(state->status.output_is_pipe || state->status.current_input_is_pipe)
		    && (-1 != state->transfer.intermediate_pipe[0]) && (-1 != state->transfer.intermediate_pipe[1])) {
			state->transfer.method = PV_TRANSFERMETHOD_SPLICE_INTERMEDIATE;
		}
	}
#endif				/* HAVE_SPLICE */

#ifdef HAVE_COPY_FILE_RANGE
	/*
	 * Attempt to use copy_file_range() only if both the input and the
	 * output are regular files, and splice() is not turned off (since
	 * it's one option for both this and splice), and line mode is
	 * inactive, and there's an output fd, and there's nothing waiting
	 * to be written from the transfer buffer, and copy_file_range()
	 * hasn't already failed on this input file descriptor.
	 */
	if (state->status.current_input_is_file && state->status.output_is_file
	    && !(state->control.no_splice || state->control.linemode || (output_fd < 0)
		 || (state->transfer.to_write > 0)
		 || (input_fd == state->transfer.copy_file_range_failed_fd)
	    )) {
		state->transfer.method = PV_TRANSFERMETHOD_COPY_FILE_RANGE;
	}
#endif				/* HAVE_COPY_FILE_RANGE */

	debug
	    ("%d->%d, max_to_write=%lld, max_to_read=%lld, max_to_read_into_buffer=%lld, max_buffer_available=%lld, method=%d",
	     input_fd, output_fd, max_to_write, max_to_read, max_to_read_into_buffer, max_buffer_available,
	     state->transfer.method);

	/*
	 * Transfer data using the appropriate method.
	 *
	 * If one method falls back to another, it will update
	 * state->transfer.method, so that variable may have changed after
	 * this block.
	 */
	switch (state->transfer.method) {
	case PV_TRANSFERMETHOD_READWRITE:
		nread =
		    pv__transfer__read_repeated(state, input_fd,
						state->transfer.transfer_buffer + state->transfer.read_position,
						(size_t) max_to_read_into_buffer);
		break;
#ifdef HAVE_SPLICE
	case PV_TRANSFERMETHOD_SPLICE:
	case PV_TRANSFERMETHOD_SPLICE_INTERMEDIATE:
		nread =
		    pv__transfer__splice_repeated(state, input_fd, output_fd,
						  state->transfer.transfer_buffer + state->transfer.read_position,
						  (size_t) max_to_read_into_buffer, max_to_read, max_to_write);
		break;
#else				/* !HAVE_SPLICE */
	case PV_TRANSFERMETHOD_SPLICE:
	case PV_TRANSFERMETHOD_SPLICE_INTERMEDIATE:
		nread =
		    pv__transfer__read_repeated(state, input_fd,
						state->transfer.transfer_buffer + state->transfer.read_position,
						(size_t) max_to_read_into_buffer);
		break;
#endif				/* HAVE_SPLICE */
#ifdef HAVE_COPY_FILE_RANGE
	case PV_TRANSFERMETHOD_COPY_FILE_RANGE:
		nread =
		    pv__transfer__copy_file_range_repeated(state, input_fd, output_fd,
							   state->transfer.transfer_buffer +
							   state->transfer.read_position,
							   (size_t) max_to_read_into_buffer, max_to_read, max_to_write);
		break;
#else				/* !HAVE_COPY_FILE_RANGE */
	case PV_TRANSFERMETHOD_COPY_FILE_RANGE:
		nread =
		    pv__transfer__read_repeated(state, input_fd,
						state->transfer.transfer_buffer + state->transfer.read_position,
						(size_t) max_to_read_into_buffer);
		break;
#endif				/* HAVE_COPY_FILE_RANGE */
	}

	/*
	 * If the actual transfer method (after any fallbacks) was
	 * read/write, cap max_to_read to the amount that was left in the
	 * buffer.
	 */
	if (PV_TRANSFERMETHOD_READWRITE == state->transfer.method)
		max_to_read = max_to_read_into_buffer;

	/*
	 * If max_to_write is zero, state->control.rate_limit_active is
	 * true, and the read/write transfer method was not used, wait
	 * briefly to avoid a busy-wait, since non-read/write transfer
	 * methods may not have called select() to wait for input.
	 *
	 * Also set zero_transfer_cap, since a write limit of zero means
	 * nothing was read either, and this doesn't mean EOF.
	 */
	if (PV_TRANSFERMETHOD_READWRITE != state->transfer.method && state->control.rate_limit_active
	    && 0 == max_to_write) {
		zero_transfer_cap = true;
		(void) is_data_ready(-1, NULL, -1, NULL, 10000);
	}

	/*
	 * If the transfer wasn't a read/write type, then data has been
	 * written, so sync if sync_after_write is true.
	 *
	 * Ignore non I/O errors, such as EBADFD (bad file descriptor),
	 * EINVAL (non syncable fd, such as a pipe), etc - only treat EIO as
	 * a failure.
	 *
	 * Since this is a write error, not a read error, it can't be
	 * skipped, so set "do_not_skip_errors".
	 */
	if (PV_TRANSFERMETHOD_READWRITE != state->transfer.method && nread > 0 && state->control.sync_after_write) {
#if defined(HAVE_FDATASYNC) && defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
		if ((fdatasync(output_fd) < 0)
		    && (EIO == errno)) {
			nread = -1;
			do_not_skip_errors = true;
		}
#else				/* !HAVE_FDATASYNC et al */
		if ((fsync(output_fd) < 0)
		    && (EIO == errno)) {
			nread = -1;
			do_not_skip_errors = true;
		}
#endif				/* HAVE_FDATASYNC et al */
	}

	if (0 == nread) {
		/*
		 * If the read returned 0, the eof of the input fd has been
		 * reached (unless the transfer amount had been capped at
		 * zero due to rate limiting).
		 *
		 * If input is EOF and the transfer buffer has also all been
		 * written out, then set eof_out as well, so that the main
		 * loop can move on to the next input file.
		 */
		if (!zero_transfer_cap) {
			debug("%s %d: %s", "input_fd", input_fd, "reached EOF");
			*eof_in = true;
			if (state->transfer.write_position >= state->transfer.read_position) {
				*eof_out = true;
			}
		}
		return true;
	} else if (nread > 0) {
		/*
		 * Read returned >0, so data was successfully read - clear
		 * the error counter and update the record of how much data
		 * is in the buffer.
		 */
		state->transfer.read_errors_in_a_row = 0;
		/*
		 * If the transfer wasn't read/write, then the amount
		 * written was the amount read.
		 */
		if (PV_TRANSFERMETHOD_READWRITE != state->transfer.method) {
			state->transfer.written = nread;
		} else {
			/*
			 * The transfer was read/write, so the buffer
			 * now has "nread" more bytes in it.
			 */
			state->transfer.read_position += nread;
		}
		debug("%s %d: %s: %ld, %s=%d", "input_fd", input_fd, "transferred", nread, "transfer.method",
		      state->transfer.method);
		/* Update the counter of all bytes read so far. */
		state->transfer.total_bytes_read += nread;
		return true;
	}

	/*
	 * This point is reached when nread < 0, so there was an error.
	 */

	/*
	 * If a read error occurred but it was EINTR or EAGAIN, wait briefly
	 * and return false, since this was a transient error.
	 */
	if ((EINTR == errno) || (EAGAIN == errno)) {
		debug("%s %d: %s: %s", "input_fd", input_fd, "transient error - waiting briefly", strerror(errno));
		(void) is_data_ready(-1, NULL, -1, NULL, 10000);
		return false;
	}

	/*
	 * The read error is not transient, so update the program's final
	 * exit status, regardless of whether errors are being skipped, and
	 * increment the error counter.
	 */
	state->status.exit_status |= PV_ERROREXIT_TRANSFER;
	state->transfer.read_errors_in_a_row++;

	/*
	 * If errors aren't being skipped, show the error, and behave as if
	 * the end of the file was reached.
	 */
	if (do_not_skip_errors) {
		pv_perror("%s", pv_current_file_name(state));
		*eof_in = true;
		if (state->transfer.write_position >= state->transfer.read_position) {
			*eof_out = true;
		}
		return true;
	}

	/*
	 * Try to skip past the error.
	 */

	amount_skipped = -1;

	if (!state->transfer.read_error_warning_shown) {
		pv_perror("%s: %s", pv_current_file_name(state), _("warning: read errors detected"));
		state->transfer.read_error_warning_shown = true;
	}

	orig_offset = (off_t) lseek(input_fd, 0, SEEK_CUR);

	/*
	 * If the file is not seekable, the error can't be skipped, so
	 * report the error and behave as if the end of input had been
	 * reached.
	 */
	if (orig_offset < 0) {
		pv_perror("%s", pv_current_file_name(state));
		*eof_in = true;
		if (state->transfer.write_position >= state->transfer.read_position) {
			*eof_out = true;
		}
		return true;
	}

	/*
	 * If a non-zero error skip block size was given, use that,
	 * otherwise start small and ramp up based on the number of errors
	 * in a row.
	 */
	if (state->control.error_skip_block > 0) {
		amount_to_skip = state->control.error_skip_block;
	} else {
		if (state->transfer.read_errors_in_a_row < 10) {
			amount_to_skip = (off_t) (state->transfer.read_errors_in_a_row < 5 ? 1 : 2);
		} else if (state->transfer.read_errors_in_a_row < 20) {
			unsigned int shift_by = (unsigned int) (state->transfer.read_errors_in_a_row - 10);
			amount_to_skip = (off_t) (1 << shift_by);
		} else {
			amount_to_skip = 512;
		}
	}

	/*
	 * Round the skip amount down to the start of the next block of the
	 * skip amount size.  For instance if the skip amount is 512, but
	 * the file offset is 257, jump to 512 instead of 769.
	 */
	if (amount_to_skip > 1) {
		skip_offset = orig_offset + amount_to_skip;
		skip_offset -= (skip_offset % amount_to_skip);
		if (skip_offset > orig_offset) {
			amount_to_skip = skip_offset - orig_offset;
		}
	}

	/*
	 * Trim the skip amount to keep within max_to_read so as not to read
	 * more than permitted.
	 */
	if (max_to_read < 0) {
		max_to_read = (off_t) max_buffer_available;
	}
	if (max_to_read < 0)
		max_to_read = 0;
	if (amount_to_skip > (off_t) max_to_read)
		amount_to_skip = (off_t) max_to_read;

	/*@+longintegral@ */
	/* splint complains about __off_t vs off_t */
	skip_offset = (off_t) lseek(input_fd, (off_t) (orig_offset + amount_to_skip), SEEK_SET);
	/*@-longintegral@ */

	/*
	 * If the skip didn't work, try only skipping 1 byte, in case the
	 * attempt would have taken the file position past the end of the
	 * input file.
	 */
	if (skip_offset < 0) {
		amount_to_skip = 1;
		/*@+longintegral@ */
		/* see above */
		skip_offset = (off_t) lseek(input_fd, (off_t) (orig_offset + amount_to_skip), SEEK_SET);
		/*@-longintegral@ */
	}

	if (skip_offset < 0) {
		/*
		 * Failed to skip - lseek() returned an error, so mark the
		 * file as having ended.
		 */
		*eof_in = true;
		/*
		 * EINVAL means the file has ended due to attempting to go
		 * past the end of it, so in that case don't report it as a
		 * "failed to seek" error, as it just means the end of the
		 * file was reached.
		 */
		if (EINVAL != errno) {
			pv_perror("%s", pv_current_file_name(state));
		}
	} else {
		amount_skipped = skip_offset - orig_offset;
	}

	/*
	 * If some bytes were successfully skipped, zero the equivalent part
	 * of the transfer buffer, and update the buffer position.
	 */
	if (amount_skipped > 0) {
		memset(state->transfer.transfer_buffer + state->transfer.read_position, 0, (size_t) amount_skipped);
		state->transfer.read_position += amount_skipped;
		if (state->control.skip_errors < 2) {
			pv_error("%s: %s: %ld - %ld (%ld %s)",
				 pv_current_file_name(state),
				 _("skipped past read error"), (long) orig_offset, (long) skip_offset,
				 (long) amount_skipped, _("B"));
		}
	} else {
		/*
		 * Failed to skip - mark file as ended.
		 */
		*eof_in = true;
		if (state->transfer.write_position >= state->transfer.read_position) {
			*eof_out = true;
		}
	}

	return true;
}


/*
 * Write state->transfer.to_write bytes of data from the transfer buffer to the output.
 *
 * Updates state->transfer.write_position by moving it on by the number of bytes
 * written; adds the number of bytes written to state->transfer.written; sets
 * *eof_out to true, on output EOF, or when the write position catches up
 * with the read position AND *eof_in is true (meaning the end of data was
 * reached).
 *
 * On error, sets *eof_out to true, sets state->transfer.written to -1, and updates
 * state->status.exit_status.
 *
 * If state->control.discard_input is true, does not actually write anything.
 *
 * Returns true if the transfer can continue normally (meaning some data was
 * transferred, or there was an error or an EOF that has caused the state to
 * be updated).
 *
 * Returns false if there was a transient error and so the transfer should
 * not continue, but be retried shortly instead.
 */
static bool pv__transfer_write(pvstate_t state, bool *eof_in, bool *eof_out, long *lineswritten)
{
	ssize_t nwritten;
	int write_errno;
	bool all_nulls;
	off_t output_offset;
	size_t write_check_position, write_end_position;

	if (NULL == state->transfer.transfer_buffer) {
		/*
		 * Report it as a generic allocation error, since this
		 * condition should never be reached due to checks made on
		 * the path to this function.
		 */
		pv_error("%s", _("memory allocation failure"));
		state->status.exit_status |= PV_ERROREXIT_MEMORY;
		*eof_out = true;
		state->transfer.written = -1;
		return true;
	}

	nwritten = 0;
	write_errno = 0;

	if (state->control.discard_input) {
		nwritten = state->transfer.to_write;
	} else if (state->transfer.to_write > 0) {

		/*
		 * In sparse output mode, check whether all of the bytes to
		 * be written are null, and if so, try to seek the output
		 * instead of writing the null bytes.
		 */
		if (state->control.sparse_output && !state->transfer.output_not_seekable) {
			write_check_position = state->transfer.write_position;
			write_end_position = write_check_position + (size_t) (state->transfer.to_write);
			all_nulls = true;
			while (all_nulls && write_check_position < write_end_position) {
				if ('\0' == state->transfer.transfer_buffer[write_check_position]) {
					write_check_position++;
				} else {
					all_nulls = false;
				}
			}
			if (all_nulls) {

				/*
				 * Use lseek() to move forward in the file,
				 * and then ftruncate() it to its new size.
				 *
				 * If any step fails, mark the output not
				 * seekable, to prevent future attempts.
				 */

				/*@+longintegral@ */
				/*
				 * splint has trouble with off_t / __off_t, in the lseek() call.
				 */
				output_offset =
				    lseek(state->control.output_fd, (off_t) (state->transfer.to_write), SEEK_CUR);
				if (output_offset == (off_t) - 1) {
					debug("%s: %s", "output lseek() failed", strerror(errno));
					state->transfer.output_not_seekable = true;
				} else {
					/* Seek successful - skip write. */
					debug("%s (%ld) -> %s: %ld", "skipped null writes",
					      (long) (state->transfer.to_write), "new position", (long) output_offset);
					nwritten = state->transfer.to_write;
					goto pv__transfer_write_completed;
				}
				/*@-longintegral@ */
			}
		}

		/*
		 * Set an interval timer or an alarm to interrupt the write
		 * with a signal if the write takes too long, so progress
		 * information can continue to be produced.
		 */
#if HAVE_SETITIMER
		struct itimerval new_timer;

		/*@-unrecog@ */
		/* splint doesn't know setitimer or ITIMER_REAL. */
		memset(&new_timer, 0, sizeof(new_timer));
		new_timer.it_value.tv_sec = (time_t) (state->control.interval);
		new_timer.it_value.tv_usec = (suseconds_t) (((long) (state->control.interval * 1000000.0)) % 1000000);

		/*
		 * The interval has to be set so that the timer continues to
		 * repeat while writes are attempted, especially as it's
		 * possible that the initial timer run will expire
		 * immediately if the period is less than 1 second.
		 */

		new_timer.it_interval.tv_sec = new_timer.it_value.tv_sec;
		new_timer.it_interval.tv_usec = new_timer.it_value.tv_usec;

		debug("%s: [%lds,%ldus]", "setting interval timer", (long) (new_timer.it_value.tv_sec),
		      (long) (new_timer.it_value.tv_usec));

		if (0 != setitimer(ITIMER_REAL, &new_timer, NULL)) {
			/*
			 * Record failure only as debugging information,
			 * since if this call failed, it's not actionable by
			 * the user and would only clutter the display.
			 */
			debug("%s: %s", "setitimer (set) failed", strerror(errno));
		}

#else				/* ! HAVE_SETITIMER */
		(void) alarm(1);
		debug("%s", "setting alarm");
#endif				/* HAVE_SETITIMER */
		debug("%s: %ld %s", "beginning write attempt", (long) (state->transfer.to_write), "bytes");
		nwritten = pv__transfer__write_repeated(state->control.output_fd,
							state->transfer.transfer_buffer +
							state->transfer.write_position,
							(size_t) (state->transfer.to_write),
							state->control.sync_after_write);
		if (nwritten < 0) {
			write_errno = (int) errno;
			debug("%s: %ld: %s", "bytes written", (long) nwritten, strerror(errno));
		} else {
			debug("%s: %ld", "bytes written", (long) nwritten);
		}
#if HAVE_SETITIMER
		memset(&new_timer, 0, sizeof(new_timer));
		new_timer.it_interval.tv_sec = 0;
		new_timer.it_interval.tv_usec = 0;
		new_timer.it_value.tv_sec = 0;
		new_timer.it_value.tv_usec = 0;
		if (0 != setitimer(ITIMER_REAL, &new_timer, NULL)) {
			/* Debug output only, as above. */
			debug("%s: %s", "setitimer (clear) failed", strerror(errno));
		}

		/*@+unrecog@ */
#else				/* ! HAVE_SETITIMER */
		debug("%s", "cancelling alarm");
		(void) alarm(0);
#endif				/* HAVE_SETITIMER */
	}

      pv__transfer_write_completed:
	/* If lseek() worked for sparse output, it jumps down here. */

	if (nwritten > 0) {
		bool tracking_lines = false;

		if ((state->control.linemode) && (lineswritten != NULL))
			tracking_lines = true;
		else if (state->display.showing_previous_line)
			tracking_lines = true;

		/*
		 * Write returned >0 - data successfully written.
		 */
		if (tracking_lines) {
			char separator;
			char *ptr;
			long lines = 0;

			/*
			 * Tracking lines - either line mode is enabled, or
			 * the display includes the "previous-line" format
			 * segment ("%L"), or both.
			 *
			 * Look through what was just written to either
			 * count how many lines there were, or get the
			 * content of the most recent complete line, or
			 * both.
			 */

			/* Allocate buffer to remember line positions. */
			if (NULL == state->transfer.line_positions && NULL != lineswritten) {
				state->transfer.line_positions_capacity = MAX_LINE_POSITIONS;
				/*@-mustfreeonly@ */
				state->transfer.line_positions =
				    calloc((size_t) (state->transfer.line_positions_capacity), sizeof(off_t));
				if (NULL == state->transfer.line_positions) {
					pv_perror("%s", _("memory allocation failure"));
				}
				/*@+mustfreeonly@ */
				/*
				 * splint doesn't see that calloc() is only
				 * called when line_positions is NULL.
				 */
			}

			if (state->control.null_terminated_lines) {
				separator = '\0';
			} else {
				separator = '\n';
			}

			ptr = (char *) (state->transfer.transfer_buffer + state->transfer.write_position - 1);
			for (ptr++;
			     ptr - (char *) state->transfer.transfer_buffer - state->transfer.write_position <
			     (size_t) nwritten; ptr++, state->transfer.last_output_position++) {
				if (*ptr != separator) {
					/*
					 * If displaying the previous line
					 * ("%L"), add to the line buffer.
					 */
					if (state->display.showing_previous_line
					    && state->display.next_line_len < PV_SIZEOF_PREVLINE_BUFFER - 1) {
						state->display.next_line[state->display.next_line_len] = *ptr;
						state->display.next_line_len++;
					}
					continue;
				}

				/* Separator found - increment line count. */
				++lines;

				/*
				 * If displaying the previous line ("%L"),
				 * update the previous-line buffer with the
				 * line that was just completed, and start a
				 * new one.
				 */
				if (state->display.showing_previous_line) {
					/* Clear the previous_line buffer. */
					memset(state->display.previous_line, 0, PV_SIZEOF_PREVLINE_BUFFER);
					/*
					 * Limit next_line_len to the buffer
					 * size, minus 1 for the terminating
					 * \0.
					 */
					if (state->display.next_line_len > PV_SIZEOF_PREVLINE_BUFFER - 1)
						state->display.next_line_len = PV_SIZEOF_PREVLINE_BUFFER - 1;
					/* Update the previous_line buffer. */
					if (state->display.next_line_len > 0) {
						memcpy(state->display.previous_line, state->display.next_line,	/* flawfinder: ignore */
						       state->display.next_line_len);
						debug("%s: [%s]", "updated previous_line",
						      state->display.previous_line);
					}
					state->display.next_line_len = 0;
					/*
					 * flawfinder - next_line_len is
					 * guaranteed to be less than the
					 * size of the previous_line buffer
					 * since it's checked just before
					 * memcpy(), and last byte in the
					 * buffer is set to \0 by memset().
					 */
				}

				if (NULL == state->transfer.line_positions)
					continue;

				/* Store the position of the separator. */
				state->transfer.line_positions[state->transfer.line_positions_head] =
				    state->transfer.last_output_position;
				state->transfer.line_positions_head++;

				/* Circular buffer - wrap around. */
				if (state->transfer.line_positions_head >= state->transfer.line_positions_capacity) {
					state->transfer.line_positions_head = 0;
				}

				/* Increment count of line positions, if below capacity. */
				if (state->transfer.line_positions_length < state->transfer.line_positions_capacity) {
					state->transfer.line_positions_length++;
				}
			}

			if (NULL != lineswritten)
				*lineswritten += lines;
		}

		state->transfer.write_position += nwritten;
		state->transfer.written += nwritten;

		/*
		 * If displaying the bytes last written ("%A"), update the
		 * copy of the last few bytes that were written.
		 */
		if (state->display.showing_last_written && (nwritten > 0)) {
			size_t new_portion_size, old_portion_size;

			new_portion_size = (size_t) nwritten;
			if (new_portion_size > state->display.lastwritten_bytes)
				new_portion_size = state->display.lastwritten_bytes;

			old_portion_size = state->display.lastwritten_bytes - new_portion_size;

			/*
			 * Make room for the new portion.
			 */
			if (old_portion_size > 0) {
				memmove(state->display.lastwritten_buffer,
					state->display.lastwritten_buffer + new_portion_size, old_portion_size);
			}

			/*
			 * Copy the new data in.
			 */
			memcpy(state->display.lastwritten_buffer +	/* flawfinder: ignore */
			       old_portion_size,
			       state->transfer.transfer_buffer + state->transfer.write_position - new_portion_size,
			       new_portion_size);
			/*
			 * flawfinder rationale: calculations above ensure
			 * that old_portion_size + new_portion_size is
			 * always <= lastwritten_bytes, and
			 * lastwritten_bytes is guaranteed by
			 * pv__format_init() to be no more than
			 * PV_SIZEOF_LASTWRITTEN_BUFFER, which is the size
			 * of lastwritten_buffer, so the memcpy() will
			 * always fit into the buffer.
			 */
		}

		/*
		 * If all the data in the buffer was written, reset the read
		 * pointer to the start, and if the input file is at EOF,
		 * set eof_out as well to indicate that everything for this
		 * input file has been written.
		 */
		if (state->transfer.write_position >= state->transfer.read_position) {
			state->transfer.write_position = 0;
			state->transfer.read_position = 0;
			if (*eof_in)
				*eof_out = true;
		}

		return true;
	}

	/*
	 * This point is reached when nwritten <= 0, so there may be an
	 * error.
	 */

	/*
	 * If a write error occurred but it was EINTR or EAGAIN, or write(2)
	 * blocked on first write such that nwritten == 0, wait briefly and
	 * then return zero, since this was a transient error.
	 */
	if ((0 == nwritten) || (EINTR == write_errno) || (EAGAIN == write_errno)) {
		if (0 == nwritten) {
			debug("%s", "attempted write blocked - waiting briefly");
		} else {
			debug("%s: %s", "transient write error - waiting briefly", strerror(write_errno));
		}
		(void) is_data_ready(-1, NULL, -1, NULL, 10000);
		return false;
	}

	/*
	 * SIGPIPE means that no more output can be written, so behave as if
	 * EOF was reached on input and output.  Don't output an error
	 * because it's not an error in PV.
	 */
	if (EPIPE == write_errno) {
		*eof_in = true;
		*eof_out = true;
		state->flags.pipe_closed = 1;
		debug("%s", "SIGPIPE received - setting pipe_closed");
		return false;
	}

	/*
	 * Anything else should be treated as an error.  Report the error,
	 * adjust the exit status, and mark the output as EOF.
	 */

	errno = write_errno;
	pv_perror("%s: %s", NULL == state->control.output_name ? "(null)" : state->control.output_name,
		  _("write error"));
	state->status.exit_status |= PV_ERROREXIT_TRANSFER;
	*eof_out = true;
	state->transfer.written = -1;

	return true;
}


/*
 * Return a pointer to a newly allocated buffer of the given size, aligned
 * appropriately for the current input and output file descriptors
 * (important if using O_DIRECT).
 *
 * Falls back to unaligned allocation if it was not possible to get an
 * aligned buffer, or if the relevant operating system features were not
 * available.  With O_DIRECT, this means that transfers could fail with an
 * "Invalid argument" error (EINVAL).
 *
 * Returns NULL on complete allocation failure.
 */
/*@null@*/
/*@only@*/
static char *pv__allocate_aligned_buffer(int outfd, int infd, size_t target_size)
{
	void *newptr;

#if defined(HAVE_FPATHCONF) && defined(HAVE_POSIX_MEMALIGN) && defined(_PC_REC_XFER_ALIGN)
	long input_alignment, output_alignment, min_alignment;
	long required_alignment;

	input_alignment = infd >= 0 ? fpathconf(infd, _PC_REC_XFER_ALIGN) : -1;
	output_alignment = fpathconf(outfd, _PC_REC_XFER_ALIGN);
#if defined(HAVE_SYSCONF) && defined(_SC_PAGESIZE)
	min_alignment = sysconf(_SC_PAGESIZE);
#else				/* ! defined(HAVE_SYSCONF) && defined(_SC_PAGESIZE) */
	min_alignment = 8192;
#endif				/* defined(HAVE_SYSCONF) && defined(_SC_PAGESIZE) */

	if (input_alignment > output_alignment) {
		required_alignment = input_alignment;
	} else if (output_alignment > input_alignment) {
		required_alignment = output_alignment;
	} else if (input_alignment < min_alignment) {
		required_alignment = min_alignment;
	} else {
		required_alignment = input_alignment;
	}

	/* Ensure the alignment is at least the page size. */
	if (required_alignment < min_alignment) {
		required_alignment = min_alignment;
	}

	newptr = NULL;

	/*@-unrecog@ */
	/* splice doesn't know of posix_memalign(). */
	if (0 != posix_memalign((void **) (&newptr), (size_t) required_alignment, target_size)) {
		newptr = malloc(target_size);
	}
	/*@+unrecog@ */
#else				/* ! defined(HAVE_FPATHCONF) && defined(HAVE_POSIX_MEMALIGN) && defined(_PC_REC_XFER_ALIGN) */
	newptr = malloc(target_size);
#endif				/* defined(HAVE_FPATHCONF) && defined(HAVE_POSIX_MEMALIGN) && defined(_PC_REC_XFER_ALIGN) */

	/* Initialise the buffer with zeroes. */
	if (NULL != newptr)
		memset(newptr, 0, target_size);

	return newptr;
}


/*
 * Transfer some data from "fd" to standard output, timing out after 9/100
 * of a second.  If state->control.rate_limit_active is true, and/or
 * "allowed" is >0, only up to "allowed" bytes can be written.  The
 * variables that "eof_in" and "eof_out" point to are used to flag that
 * we've finished reading and writing respectively.
 *
 * Returns the number of bytes written, or negative on error (in which case
 * state->status.exit_status is updated).  In line mode, the number of lines
 * written will be put into *lineswritten.
 */
ssize_t pv_transfer(pvstate_t state, int fd, bool *eof_in, bool *eof_out, off_t allowed, long *lineswritten)
{
	bool ready_to_read, ready_to_write;
	int check_read_fd, check_write_fd;
	int n;

	if (NULL == state)
		return 0;

#ifdef O_DIRECT
	/*
	 * Set or clear O_DIRECT on the input and output file descriptors,
	 * if the setting has changed.
	 */
	if (state->control.direct_io_changed) {
		if (!(*eof_in)) {
			if (0 != fcntl(fd, F_SETFL, (state->control.direct_io ? O_DIRECT : 0) | fcntl(fd, F_GETFL))) {
				debug("%s: %s: %s", pv_current_file_name(state), "fcntl", strerror(errno));
			}
		}
		if (!(*eof_out)) {
			if (0 != fcntl(state->control.output_fd, F_SETFL,
				       (state->control.direct_io ? O_DIRECT : 0) |
				       fcntl(state->control.output_fd, F_GETFL))) {
				debug("%s: %s: %s",
				      NULL == state->control.output_name ? "(null)" : state->control.output_name,
				      "fcntl", strerror(errno));
			}
		}
		state->control.direct_io_changed = false;
	}
#endif				/* O_DIRECT */

	/*
	 * Reinitialise the error skipping variables if the file descriptor
	 * has changed since the last time this function was called.
	 */
	if (fd != state->transfer.last_read_skip_fd) {
		state->transfer.last_read_skip_fd = fd;
		state->transfer.read_errors_in_a_row = 0;
		state->transfer.read_error_warning_shown = false;
	}

	/*
	 * Allocate a new buffer, aligned appropriately for the input file
	 * (important if using O_DIRECT).
	 */
	if (NULL == state->transfer.transfer_buffer) {
		state->transfer.transfer_buffer =
		    pv__allocate_aligned_buffer(state->control.output_fd, fd, state->control.target_buffer_size + 32);
		if (NULL == state->transfer.transfer_buffer) {
			pv_perror("%s", _("memory allocation failure"));
			state->status.exit_status |= PV_ERROREXIT_MEMORY;
			return -1;
		}
		state->transfer.buffer_size = state->control.target_buffer_size;
	}

	/*
	 * Reallocate the buffer if the buffer size has changed
	 * mid-transfer.  This has to be done by allocating a new buffer,
	 * copying to it, and freeing the old one (potentially fragmenting
	 * memory) because the buffer may need to be aligned for O_DIRECT,
	 * and realloc() can't guarantee the same alignment.
	 */
	if (state->transfer.buffer_size < state->control.target_buffer_size) {
		char *newptr;
		newptr =
		    pv__allocate_aligned_buffer(state->control.output_fd, fd, state->control.target_buffer_size + 32);
		if (NULL == newptr) {
			/*
			 * Reset the target buffer size to the current
			 * buffer size if the allocation failed, to avoid
			 * trying to reallocate repeatedly.
			 */
			debug("allocate aligned buffer: %s", strerror(errno));
			state->control.target_buffer_size = state->transfer.buffer_size;
		} else {
			debug("%s: %ld", "buffer resized", state->transfer.buffer_size);
			/*
			 * Copy the old buffer contents into the new buffer,
			 * and free the old one.
			 */
			if (state->transfer.buffer_size > 0) {
				memcpy(newptr, state->transfer.transfer_buffer, state->transfer.buffer_size);	/* flawfinder: ignore */
			}
			/*
			 * flawfinder rationale: the number of bytes copied
			 * is definitely always within the new buffer size.
			 */
			free(state->transfer.transfer_buffer);
			state->transfer.transfer_buffer = newptr;
			state->transfer.buffer_size = state->control.target_buffer_size;
		}
	}

	if ((state->control.linemode) && (lineswritten != NULL))
		*lineswritten = 0;

	if ((*eof_in) && (*eof_out)) {
		debug("%s %d: %s", "fd", fd, "early return 0 - EOF in and out");
		return 0;
	}

	check_read_fd = -1;
	check_write_fd = -1;

	/*
	 * If the input file is not at EOF and there's room in the buffer,
	 * look for incoming data from it.
	 */
	if ((!(*eof_in)) && (state->transfer.read_position < state->transfer.buffer_size)) {
		check_read_fd = fd;
	}

	/*
	 * Calculate how much can be written this time, based on the amount
	 * of data left in the buffer and capped based on whether rate
	 * limiting is active or if "allowed" is > 0.
	 */
	state->transfer.to_write = (ssize_t) (state->transfer.read_position - state->transfer.write_position);
	if ((state->control.rate_limit_active) || (allowed > 0)) {
		if ((off_t) (state->transfer.to_write) > allowed) {
			state->transfer.to_write = (ssize_t) allowed;
		}
	}

	/*
	 * If there is anything waiting to be written, look for the output
	 * becoming writable.
	 */
	if ((!(*eof_out)) && (state->transfer.to_write > 0)) {
		check_write_fd = state->control.output_fd;
	}

	ready_to_read = false;
	ready_to_write = false;
	n = is_data_ready(check_read_fd, &ready_to_read, check_write_fd, &ready_to_write, 90000);

	if (n < 0) {
		/*
		 * Ignore transient errors by returning 0 immediately.
		 */
		if (EINTR == errno) {
			debug("%s %d: %s", "fd", fd, "early return 0 - is_data_ready < 0");
			return 0;
		}

		/*
		 * Any other error is reported and causes an early return.
		 */
		pv_perror("%s", pv_current_file_name(state));

		state->status.exit_status |= PV_ERROREXIT_TRANSFER;

		return -1;
	}

	state->transfer.written = 0;

	/*
	 * If there is data to read, try to read some in. Return early if
	 * there was a transient read error.
	 *
	 * NB this can update state->transfer.written because of
	 * non-read/write transfer methods such as splice() or
	 * copy_file_range().
	 */
	if (ready_to_read) {
		if (!pv__transfer_read(state, fd, eof_in, eof_out, allowed)) {
			debug("%s %d: %s (%s=%s, %s=%s, %s=%lu)", "fd", fd,
			      "early return 0 - pv__transfer_read returned false", "eof_in", *eof_in ? "true" : "false",
			      "eof_out", *eof_out ? "true" : "false", "allowed", (unsigned long) allowed);
			return 0;
		}
	}

	/*
	 * In line mode, only write up to and including the last newline, so
	 * that output is written line-by-line.
	 */
	if ((state->transfer.to_write > 0) && (state->control.linemode) && !(state->control.null_terminated_lines)) {
		const char *start;
		const char *end;

		start = (char *) (state->transfer.transfer_buffer + state->transfer.write_position);
		end = pv_memrchr(start, (int) '\n', (size_t) (state->transfer.to_write));

		if (NULL != end) {
			state->transfer.to_write = (ssize_t) ((end - start) + 1);
		}
	}

	/*
	 * If there is data to write, and the output is ready to receive it,
	 * and the transfer method is read/write rather than splice() or
	 * copy_file_range(), write some data.
	 *
	 * Return early if there was a transient write error.
	 */
	if (ready_to_write && (PV_TRANSFERMETHOD_READWRITE == state->transfer.method)
	    && (state->transfer.read_position > state->transfer.write_position)
	    && (state->transfer.to_write > 0)
	    && (NULL != lineswritten)) {
		if (!pv__transfer_write(state, eof_in, eof_out, lineswritten)) {
			debug("%s %d: %s (%s=%s, %s=%s, %s=%lu)", "fd", fd,
			      "early return 0 - pv__transfer_write returned false", "eof_in",
			      *eof_in ? "true" : "false", "eof_out", *eof_out ? "true" : "false", "lineswritten",
			      (unsigned long) lineswritten);
			return 0;
		}
	}
#ifdef MAXIMISE_BUFFER_FILL
	/*
	 * Rotate the written bytes out of the buffer so that it can be
	 * filled up completely by the next read.
	 */
	if (state->transfer.write_position > 0) {
		if (state->transfer.write_position < state->transfer.read_position) {
			memmove(state->transfer.transfer_buffer,
				state->transfer.transfer_buffer +
				state->transfer.write_position,
				state->transfer.read_position - state->transfer.write_position);
			state->transfer.read_position -= state->transfer.write_position;
			state->transfer.write_position = 0;
		} else {
			state->transfer.write_position = 0;
			state->transfer.read_position = 0;
		}
	}
#endif				/* MAXIMISE_BUFFER_FILL */

	if (0 == state->transfer.written) {
		debug("%s %d: %s", "fd", fd, "end-of-function return 0 - transfer.written is zero");
	}

	return state->transfer.written;
}
