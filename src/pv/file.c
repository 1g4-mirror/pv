/*
 * Functions for opening and closing files.
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
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>


/*
 * Calculate the total number of bytes to be transferred by adding up the
 * sizes of all input files.  If any of the input files are of indeterminate
 * size (such as if they are a pipe), the total size is set to zero.
 *
 * Any files that cannot be stat()ed or that access() says we can't read
 * will cause a warning to be output and will be removed from the list.
 *
 * Returns the total size, or 0 if it is unknown.
 */
static unsigned long long pv_calc_total_bytes(pvstate_t state)
{
	unsigned long long total;
	struct stat sb;
	int rc, file_idx, move_idx, fd;

	total = 0;
	rc = 0;
	memset(&sb, 0, sizeof(sb));

	/*
	 * No files specified - check stdin.
	 */
	if (state->input_file_count < 1) {
		if (0 == fstat(STDIN_FILENO, &sb))
			total = sb.st_size;
		return total;
	}

	for (file_idx = 0; file_idx < state->input_file_count; file_idx++) {
		if (0 == strcmp(state->input_files[file_idx], "-")) {
			rc = fstat(STDIN_FILENO, &sb);
			if (rc != 0) {
				total = 0;
				return total;
			}
		} else {
			rc = stat(state->input_files[file_idx], &sb);
			if (0 == rc)
				rc = access(state->input_files[file_idx], R_OK);
		}

		if (rc != 0) {
			pv_error(state, "%s: %s", state->input_files[file_idx], strerror(errno));
			for (move_idx = file_idx; move_idx < state->input_file_count - 1; move_idx++) {
				state->input_files[move_idx] = state->input_files[move_idx + 1];
			}
			state->input_file_count--;
			file_idx--;
			state->exit_status |= 2;
			continue;
		}

		if (S_ISBLK(sb.st_mode)) {
			/*
			 * Get the size of block devices by opening
			 * them and seeking to the end.
			 */
			if (0 == strcmp(state->input_files[file_idx], "-")) {
				fd = open("/dev/stdin", O_RDONLY);
			} else {
				fd = open(state->input_files[file_idx], O_RDONLY);
			}
			if (fd >= 0) {
				total += lseek(fd, 0, SEEK_END);
				close(fd);
			} else {
				pv_error(state, "%s: %s", state->input_files[file_idx], strerror(errno));
				state->exit_status |= 2;
			}
		} else if (S_ISREG(sb.st_mode)) {
			total += sb.st_size;
		} else {
			total = 0;
		}
	}

	/*
	 * Patch from Peter Samuelson: if we cannot work out the size of the
	 * input, but we are writing to a block device, then use the size of
	 * the output block device.
	 *
	 * Further modified to check that stdout is not in append-only mode
	 * and that we can seek back to the start after getting the size.
	 */
	if (total <= 0) {
		rc = fstat(STDOUT_FILENO, &sb);
		if ((0 == rc) && S_ISBLK(sb.st_mode)
		    && (0 == (fcntl(STDOUT_FILENO, F_GETFL) & O_APPEND))) {
			total = lseek(STDOUT_FILENO, 0, SEEK_END);
			if (lseek(STDOUT_FILENO, 0, SEEK_SET) != 0) {
				pv_error(state, "%s: %s: %s", "(stdout)",
					 _("failed to seek to start of output"), strerror(errno));
				state->exit_status |= 2;
			}
			/*
			 * If we worked out a size, then set the
			 * stop-at-size flag to prevent a "no space left on
			 * device" error when we reach the end of the output
			 * device.
			 */
			if (total > 0) {
				state->stop_at_size = 1;
			}
		}
	}

	return total;
}


/*
 * Count the total number of lines to be transferred by reading through all
 * input files.  If any of the inputs are not regular files (such as if they
 * are a pipe or a block device), the total size is set to zero.
 *
 * Any files that cannot be stat()ed or that access() says we can't read
 * will cause a warning to be output and will be removed from the list.
 *
 * Returns the total size, or 0 if it is unknown.
 */
static unsigned long long pv_calc_total_lines(pvstate_t state)
{
	unsigned long long total;
	struct stat sb;
	int rc, file_idx, move_idx, fd;

	/*
	 * In line mode, we count input lines to work out the total size.
	 */
	total = 0;

	for (file_idx = 0; file_idx < state->input_file_count; file_idx++) {
		fd = -1;

		if (0 == strcmp(state->input_files[file_idx], "-")) {
			rc = fstat(STDIN_FILENO, &sb);
			if ((rc != 0) || (!S_ISREG(sb.st_mode))) {
				total = 0;
				return total;
			}
			fd = dup(STDIN_FILENO);
		} else {
			rc = stat(state->input_files[file_idx], &sb);
			if ((rc != 0) || (!S_ISREG(sb.st_mode))) {
				total = 0;
				return total;
			}
			fd = open(state->input_files[file_idx], O_RDONLY);
		}

		if (fd < 0) {
			pv_error(state, "%s: %s", state->input_files[file_idx], strerror(errno));
			for (move_idx = file_idx; move_idx < state->input_file_count - 1; move_idx++) {
				state->input_files[move_idx] = state->input_files[move_idx + 1];
			}
			state->input_file_count--;
			file_idx--;
			state->exit_status |= 2;
			continue;
		}
#if HAVE_POSIX_FADVISE
		/* Advise the OS that we will only be reading sequentially. */
		(void) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

		while (1) {
			unsigned char scanbuf[1024];
			int numread, buf_idx;

			numread = read(fd, scanbuf, sizeof(scanbuf));
			if (numread < 0) {
				pv_error(state, "%s: %s", state->input_files[file_idx], strerror(errno));
				state->exit_status |= 2;
				break;
			} else if (0 == numread) {
				break;
			}
			for (buf_idx = 0; buf_idx < numread; buf_idx++) {
				if (state->null) {
					if ('\0' == scanbuf[buf_idx])
						total++;
				} else {
					if ('\n' == scanbuf[buf_idx])
						total++;
				}
			}
		}

		if (0 != lseek(fd, 0, SEEK_SET)) {
			pv_error(state, "%s: %s", state->input_files[file_idx], strerror(errno));
			state->exit_status |= 2;
		}

		(void) close(fd);
	}

	return total;
}


/*
 * Work out the total size of all data by adding up the sizes of all input
 * files, using either pv_calc_total_bytes() or pv_calc_total_lines()
 * depending on whether state->linemode is true.
 *
 * Returns the total size, or 0 if it is unknown.
 */
unsigned long long pv_calc_total_size(pvstate_t state)
{
	if (state->linemode) {
		return pv_calc_total_lines(state);
	} else {
		return pv_calc_total_bytes(state);
	}
}


/*
 * Close the given file descriptor and open the next one, whose number in
 * the list is "filenum", returning the new file descriptor (or negative on
 * error). It is an error if the next input file is the same as the file
 * stdout is pointing to.
 *
 * Updates state->current_file in the process.
 */
int pv_next_file(pvstate_t state, int filenum, int oldfd)
{
	struct stat isb;
	struct stat osb;
	int fd, input_file_is_stdout;

	if (oldfd > 0) {
		if (close(oldfd)) {
			pv_error(state, "%s: %s", _("failed to close file"), strerror(errno));
			state->exit_status |= 8;
			return -1;
		}
	}

	if (filenum >= state->input_file_count) {
		state->exit_status |= 8;
		return -1;
	}

	if (filenum < 0) {
		state->exit_status |= 8;
		return -1;
	}

	if (0 == strcmp(state->input_files[filenum], "-")) {
		fd = STDIN_FILENO;
	} else {
		fd = open(state->input_files[filenum], O_RDONLY);
		if (fd < 0) {
			pv_error(state, "%s: %s: %s",
				 _("failed to read file"), state->input_files[filenum], strerror(errno));
			state->exit_status |= 2;
			return -1;
		}
	}

	if (fstat(fd, &isb)) {
		pv_error(state, "%s: %s: %s", _("failed to stat file"), state->input_files[filenum], strerror(errno));
		close(fd);
		state->exit_status |= 2;
		return -1;
	}

	if (fstat(STDOUT_FILENO, &osb)) {
		pv_error(state, "%s: %s", _("failed to stat output file"), strerror(errno));
		close(fd);
		state->exit_status |= 2;
		return -1;
	}

	/*
	 * Check that this new input file is not the same as stdout's
	 * destination. This restriction is ignored for anything other
	 * than a regular file or block device.
	 */
	input_file_is_stdout = 1;
	if (isb.st_dev != osb.st_dev)
		input_file_is_stdout = 0;
	if (isb.st_ino != osb.st_ino)
		input_file_is_stdout = 0;
	if (isatty(fd))
		input_file_is_stdout = 0;
	if ((!S_ISREG(isb.st_mode)) && (!S_ISBLK(isb.st_mode)))
		input_file_is_stdout = 0;

	if (input_file_is_stdout) {
		pv_error(state, "%s: %s", _("input file is output file"), state->input_files[filenum]);
		close(fd);
		state->exit_status |= 4;
		return -1;
	}

	state->current_file = state->input_files[filenum];
	if (0 == strcmp(state->input_files[filenum], "-")) {
		state->current_file = "(stdin)";
	}
#ifdef O_DIRECT
	/*
	 * Set or clear O_DIRECT on the file descriptor.
	 */
	fcntl(fd, F_SETFL, (state->direct_io ? O_DIRECT : 0) | fcntl(fd, F_GETFL));
	/*
	 * We don't clear direct_io_changed here, to avoid race conditions
	 * that could cause the input and output settings to differ.
	 */
#endif				/* O_DIRECT */

	return fd;
}

/* EOF */
