/*
 * Display functions.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023-2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

/*
 * We need sys/ioctl.h for ioctl() regardless of whether TIOCGWINSZ is
 * defined in termios.h, so we no longer use AC_HEADER_TIOCGWINSZ in
 * configure.in, and just include both header files if they are available.
 * (GH#74, 2023-08-06)
 */
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

/*
 * Output an error message.  If we've displayed anything to the terminal
 * already, then put a newline before our error so we don't write over what
 * we've written.
 */
void pv_error(pvstate_t state, char *format, ...)
{
	va_list ap;
	if (state->display.display_visible)
		fprintf(stderr, "\n");
	fprintf(stderr, "%s: ", state->status.program_name);
	va_start(ap, format);
	(void) vfprintf(stderr, format, ap);	/* flawfinder: ignore */
	va_end(ap);
	fprintf(stderr, "\n");
	/*
	 * flawfinder: this function relies on callers always having a
	 * static format string, not directly subject to outside influences.
	 */
}


/*
 * Return true if we are the foreground process on the terminal, or if we
 * aren't outputting to a terminal; false otherwise.
 */
bool pv_in_foreground(void)
{
	pid_t our_process_group;
	pid_t tty_process_group;

	if (0 == isatty(STDERR_FILENO)) {
		debug("true: %s", "not a tty");
		return true;
	}

	/*@-type@ *//* __pid_t vs pid_t, not significant */
	our_process_group = getpgrp();
	tty_process_group = tcgetpgrp(STDERR_FILENO);
	/*@+type@ */

	if (tty_process_group == -1 && errno == ENOTTY) {
		debug("true: %s", "tty_process_group is -1, errno is ENOTTY");
		return true;
	}

	if (our_process_group == tty_process_group) {
		debug("true: %s (%d)", "our_process_group == tty_process_group", our_process_group);
		return true;
	}

	/*
	 * If the terminal process group ID doesn't match our own, assume
	 * we're in the background.
	 */
	debug("false: our_process_group=%d, tty_process_group=%d", our_process_group, tty_process_group);

	return false;
}


/*
 * Write the given buffer to the given file descriptor, retrying until all
 * bytes have been written or an error has occurred.
 */
void pv_write_retry(int fd, const char *buf, size_t count)
{
	while (count > 0) {
		ssize_t nwritten;

		nwritten = write(fd, buf, count);

		if (nwritten < 0) {
			if ((EINTR == errno) || (EAGAIN == errno)) {
				continue;
			}
			return;
		}
		if (nwritten < 1)
			return;

		count -= nwritten;
		buf += nwritten;
	}
}


/*
 * Write the given buffer to the terminal, like pv_write_retry(), unless
 * stderr is suspended.
 */
void pv_tty_write(pvstate_t state, const char *buf, size_t count)
{
	while (0 == state->flag.suspend_stderr && count > 0) {
		ssize_t nwritten;

		nwritten = write(STDERR_FILENO, buf, count);

		if (nwritten < 0) {
			if ((EINTR == errno) || (EAGAIN == errno)) {
				continue;
			}
			return;
		}
		if (nwritten < 1)
			return;

		count -= nwritten;
		buf += nwritten;
	}
}


/*
 * Fill in *width and *height with the current terminal size,
 * if possible.
 */
void pv_screensize(unsigned int *width, unsigned int *height)
{
#ifdef TIOCGWINSZ
	struct winsize wsz;

	memset(&wsz, 0, sizeof(wsz));

	if (0 != isatty(STDERR_FILENO)) {
		if (0 == ioctl(STDERR_FILENO, TIOCGWINSZ, &wsz)) {
			*width = wsz.ws_col;
			*height = wsz.ws_row;
		}
	}
#endif
}


/*
 * Return the original value x so that it has been clamped between
 * [min..max]
 */
static long bound_long(long x, long min, long max)
{
	return x < min ? min : x > max ? max : x;
}


/*
 * Given how many bytes have been transferred, the total byte count to
 * transfer, and the current average transfer rate, return the estimated
 * number of seconds until completion.
 */
static long pv__seconds_remaining(const off_t so_far, const off_t total, const long double rate)
{
	long double amount_left;

	if ((so_far < 1) || (rate < 0.001))
		return 0;

	amount_left = (long double) (total - so_far) / rate;

	return (long) amount_left;
}

/*
 * Given a long double value, it is divided or multiplied by the ratio until
 * a value in the range 1.0 to 999.999... is found.  The string "prefix" to
 * is updated to the corresponding SI prefix.
 *
 * If the count type is PV_TRANSFERCOUNT_BYTES, then the second byte of
 * "prefix" is set to "i" to denote MiB etc (IEEE1541).  Thus "prefix"
 * should be at least 3 bytes long (to include the terminating null).
 */
static void pv__si_prefix(long double *value, char *prefix, const long double ratio, pvtransfercount_t count_type)
{
	static char *pfx_000 = NULL;	 /* kilo, mega, etc */
	static char *pfx_024 = NULL;	 /* kibi, mibi, etc */
	static char const *pfx_middle_000 = NULL;
	static char const *pfx_middle_024 = NULL;
	char *pfx;
	char const *pfx_middle;
	char const *pfx_ptr;
	long double cutoff;

	prefix[0] = ' ';		    /* Make the prefix start blank. */
	prefix[1] = '\0';

	/*
	 * The prefix list strings have a space (no prefix) in the middle;
	 * moving right from the space gives the prefix letter for each
	 * increasing multiple of 1000 or 1024 - such as kilo, mega, giga -
	 * and moving left from the space gives the prefix letter for each
	 * decreasing multiple - such as milli, micro, nano.
	 */

	/*
	 * Prefix list for multiples of 1000.
	 */
	if (NULL == pfx_000) {
		/*@-onlytrans@ */
		pfx_000 = _("yzafpnum kMGTPEZY");
		/*
		 * splint: this is only looked up once in the program's run,
		 * so the memory leak is negligible.
		 */
		/*@+onlytrans@ */
		if (NULL == pfx_000) {
			debug("%s", "prefix list was NULL");
			return;
		}
		pfx_middle_000 = strchr(pfx_000, ' ');
	}

	/*
	 * Prefix list for multiples of 1024.
	 */
	if (NULL == pfx_024) {
		/*@-onlytrans@ */
		pfx_024 = _("yzafpnum KMGTPEZY");
		/*@+onlytrans@ *//* splint: see above. */
		if (NULL == pfx_024) {
			debug("%s", "prefix list was NULL");
			return;
		}
		pfx_middle_024 = strchr(pfx_024, ' ');
	}

	pfx = pfx_000;
	pfx_middle = pfx_middle_000;
	if (count_type == PV_TRANSFERCOUNT_BYTES) {
		/* bytes - multiples of 1024 */
		pfx = pfx_024;
		pfx_middle = pfx_middle_024;
	}

	pfx_ptr = pfx_middle;
	if (NULL == pfx_ptr) {
		debug("%s", "prefix middle was NULL");
		return;
	}

	/*
	 * Force an empty prefix if the value is almost zero, to avoid
	 * "0yB".  NB we don't compare directly with zero because of
	 * potential floating-point inaccuracies.
	 *
	 * See the "count_type" check below for the reason we add another
	 * space in bytes mode.
	 */
	if ((*value > -0.00000001) && (*value < 0.00000001)) {
		if (count_type == PV_TRANSFERCOUNT_BYTES) {
			prefix[1] = ' ';
			prefix[2] = '\0';
		}
		return;
	}

	/*
	 * Cut-off for moving to the next prefix - a little less than the
	 * ratio (970 for ratio=1000, 993 for ratio=1024).
	 */
	cutoff = ratio * 0.97;

	/*
	 * Divide by the ratio until the value is a little below the ratio,
	 * moving along the prefix list with each division to get the
	 * associated prefix letter, so that for example 20000 becomes 20
	 * with a "k" (kilo) prefix.
	 */

	if (*value > 0) {
		/* Positive values */

		while ((*value > cutoff) && (*(pfx_ptr += 1) != '\0')) {
			*value /= ratio;
			prefix[0] = *pfx_ptr;
		}
	} else {
		/* Negative values */

		cutoff = 0 - cutoff;
		while ((*value < cutoff) && (*(pfx_ptr += 1) != '\0')) {
			*value /= ratio;
			prefix[0] = *pfx_ptr;
		}
	}

	/*
	 * Multiply by the ratio until the value is at least 1, moving in
	 * the other direction along the prefix list to get the associated
	 * prefix letter - so for example a value of 0.5 becomes 500 with a
	 * "m" (milli) prefix.
	 */

	if (*value > 0) {
		/* Positive values */
		while ((*value < 1.0) && ((pfx_ptr -= 1) != (pfx - 1))) {
			*value *= ratio;
			prefix[0] = *pfx_ptr;
		}
	} else {
		/* Negative values */
		while ((*value > -1.0) && ((pfx_ptr -= 1) != (pfx - 1))) {
			*value *= ratio;
			prefix[0] = *pfx_ptr;
		}
	}

	/*
	 * Byte prefixes (kibi, mebi, etc) are of the form "KiB" rather than
	 * "KB", so that's two characters, not one - meaning that for just
	 * "B", the prefix is two spaces, not one.
	 */
	if (count_type == PV_TRANSFERCOUNT_BYTES) {
		prefix[1] = (prefix[0] == ' ' ? ' ' : 'i');
		prefix[2] = '\0';
	}
}


/*
 * Put a string in "buffer" (max length "bufsize") containing "amount"
 * formatted such that it's 3 or 4 digits followed by an SI suffix and then
 * whichever of "suffix_basic" or "suffix_bytes" is appropriate (whether
 * "count_type" is PV_TRANSFERTYPE_LINES for non-byte amounts or
 * PV_TRANSFERTYPE_BYTES for byte amounts).  If "count_type" is
 * PV_TRANSFERTYPE_BYTES then the SI units are KiB, MiB etc and the divisor
 * is 1024 instead of 1000.
 *
 * The "format" string is in sprintf format and must contain exactly one %
 * parameter (a %s) which will expand to the string described above.
 */
static void pv__sizestr(char *buffer, size_t bufsize, char *format,
			long double amount, char *suffix_basic, char *suffix_bytes, pvtransfercount_t count_type)
{
	char sizestr_buffer[256];	 /* flawfinder: ignore */
	char si_prefix[8];		 /* flawfinder: ignore */
	long double divider;
	long double display_amount;
	char *suffix;

	/*
	 * flawfinder: sizestr_buffer and si_prefix are explicitly zeroed;
	 * sizestr_buffer is only ever used with pv_snprintf() along with
	 * its buffer size; si_prefix is only populated by pv_snprintf()
	 * along with its size, and by pv__si_prefix() which explicitly only
	 * needs 3 bytes.
	 */

	memset(sizestr_buffer, 0, sizeof(sizestr_buffer));
	memset(si_prefix, 0, sizeof(si_prefix));

	(void) pv_snprintf(si_prefix, sizeof(si_prefix), "%s", "  ");

	if (count_type == PV_TRANSFERCOUNT_BYTES) {
		suffix = suffix_bytes;
		divider = 1024.0;
	} else if (count_type == PV_TRANSFERCOUNT_DECBYTES) {
		suffix = suffix_bytes;
		divider = 1000.0;
	} else {
		suffix = suffix_basic;
		divider = 1000.0;
	}

	display_amount = amount;

	pv__si_prefix(&display_amount, si_prefix, divider, count_type);

	/* Make sure we don't overrun our buffer. */
	if (display_amount > 100000)
		display_amount = 100000;
	if (display_amount < -100000)
		display_amount = -100000;

	/* Fix for display of "1.01e+03" instead of "1010" */
	if ((display_amount > 99.9) || (display_amount < -99.9)) {
		(void) pv_snprintf(sizestr_buffer, sizeof(sizestr_buffer),
				   "%4ld%.2s%.16s", (long) display_amount, si_prefix, suffix);
	} else {
		/*
		 * AIX blows up with %4.3Lg%.2s%.16s for some reason, so we
		 * write display_amount separately first.
		 */
		char str_disp[64];	 /* flawfinder: ignore - only used with pv_snprintf(). */
		memset(str_disp, 0, sizeof(str_disp));
		/* # to get 13.0GB instead of 13GB (#1477) */
		(void) pv_snprintf(str_disp, sizeof(str_disp), "%#4.3Lg", display_amount);
		(void) pv_snprintf(sizestr_buffer, sizeof(sizestr_buffer), "%s%.2s%.16s", str_disp, si_prefix, suffix);
	}

	(void) pv_snprintf(buffer, bufsize, format, sizestr_buffer);
}


/*
 * Formatting functions.
 *
 * Each formatting function takes a state, the current display, and the
 * segment it's for; it also takes a buffer, with a particular size, and an
 * offset at which to start writing to the buffer.
 *
 * If the component is dynamically sized (such as a progress bar with no
 * chosen_size constraint), the segment's "width" is expected to have
 * already been populated by the caller, with the target width.
 *
 * The function writes the appropriate string to the buffer at the offset,
 * and updates the segment's "offset" and "bytes".  The number of bytes
 * written ("bytes") is also returned; it will be 0 if the string would not
 * fit into the buffer.
 *
 * The caller is expected to update the segment's "width".
 *
 * If called with a buffer size of 0, only the side effects occur (such as
 * setting flags like display->showing_timer).
 */

/*
 * Add a null-terminated string to the buffer if there is room for it,
 * updating the segment's offset and bytes values and returning the bytes
 * value, or treating the byte count as zero if there's insufficient space.
 */
static size_t pv__format_segmentcontent(char *content, pvdisplay_segment_t segment, char *buffer, size_t buffer_size,
					size_t offset)
{
	size_t bytes;

	bytes = strlen(content);	    /* flawfinder: ignore */
	/* flawfinder - caller is required to null-terminate the string. */

	if (offset >= buffer_size)
		bytes = 0;
	if ((offset + bytes) >= buffer_size)
		bytes = 0;

	segment->offset = offset;
	segment->bytes = bytes;

	if (0 == bytes)
		return 0;

	memmove(buffer + offset, content, bytes);

	return bytes;
}


/*
 * Write a progress bar to a buffer, in known-size or rate-gauge mode - a
 * bar, and a percentage (size) or max rate (gauge).  The total width of the
 * content is bounded to the given width.  Returns the number of bytes
 * written to the buffer.
 *
 * If "bar_sides" is false, only the bar itself is rendered, not the opening
 * and closing characters.
 *
 * If "include_bar" is false, the bar is omitted entirely.
 *
 * If "include_amount" is false, the percentage or rate after the bar is
 * omitted.
 *
 * This is only called by pv__format_progress().
 */
static size_t pv__format_progress_knownsize(pvstate_t state, pvdisplay_t display, char *buffer, size_t buffer_size,
					    size_t width, bool bar_sides, bool include_bar, bool include_amount)
{
	char after_bar[32];		 /* flawfinder: ignore - only populated by pv_snprintf(). */
	size_t after_bar_bytes, after_bar_width;
	size_t bar_area_width, filled_bar_width, buffer_offset, pad_count;
	int bar_percentage;

	buffer[0] = '\0';

	memset(after_bar, 0, sizeof(after_bar));

	if (state->control.size > 0) {
		/* Percentage of data transferred. */
		bar_percentage = state->calc.percentage;
		(void) pv_snprintf(after_bar, sizeof(after_bar), " %3ld%%", bar_percentage);
	} else {
		/* Current rate vs max rate. */
		bar_percentage = 0;
		if (state->calc.rate_max > 0) {
			bar_percentage = (int) (100.0 * state->calc.transfer_rate / state->calc.rate_max);
		}

		/*@-mustfreefresh@ */
		if (state->control.bits && !state->control.linemode) {
			/* bits per second */
			pv__sizestr(after_bar, sizeof(after_bar), "/%s",
				    8.0 * state->calc.rate_max, "", _("b/s"), display->count_type);
		} else {
			/* bytes or lines per second */
			pv__sizestr(after_bar, sizeof(after_bar),
				    "/%s", state->calc.rate_max, _("/s"), _("B/s"), display->count_type);
		}
		/*@+mustfreefresh@ *//* splint: see above about gettext(). */
	}

	if (!include_amount)
		after_bar[0] = '\0';

	after_bar_bytes = strlen(after_bar);	/* flawfinder: ignore */
	/* flawfinder: always \0-terminated by pv_snprintf() and the earlier memset(). */
	after_bar_width = pv_strwidth(after_bar, after_bar_bytes);

	if (!include_bar) {
		if (buffer_size < after_bar_bytes)
			return 0;
		if (after_bar_bytes > 1) {
			/* NB we skip the leading space. */
			memmove(buffer, after_bar + 1, after_bar_bytes - 1);
			buffer[after_bar_bytes - 1] = '\0';
			return after_bar_bytes - 1;
		}
		return 0;
	}

	if (bar_sides) {
		if (width < (after_bar_width + 2))
			return 0;
		bar_area_width = width - after_bar_width - 2;
	} else {
		if (width < after_bar_width)
			return 0;
		bar_area_width = width - after_bar_width;
	}

	if (bar_area_width > buffer_size - 16)
		bar_area_width = buffer_size - 16;

	filled_bar_width = (size_t) (bar_area_width * bar_percentage) / 100;
	/* Leave room for the tip of the bar. */
	if (filled_bar_width > 0)
		filled_bar_width--;

	debug("width=%d bar_area_width=%d filled_bar_width=%d", width, bar_area_width, filled_bar_width);

	buffer_offset = 0;

	if (bar_sides) {
		/* The opening of the bar area. */
		buffer[buffer_offset++] = '[';
	}

	/* The bar portion. */
	for (pad_count = 0; pad_count < filled_bar_width && buffer_offset < buffer_size - 1; pad_count++) {
		if (pad_count < bar_area_width)
			buffer[buffer_offset++] = '=';
	}

	/* The tip of the bar, if not at 100%. */
	if (pad_count < bar_area_width) {
		if (buffer_offset < buffer_size - 1)
			buffer[buffer_offset++] = '>';
		pad_count++;
	}

	/* The spaces after the bar. */
	for (; pad_count < bar_area_width; pad_count++) {
		if (buffer_offset < buffer_size - 1)
			buffer[buffer_offset++] = ' ';
	}

	if (bar_sides) {
		/* The closure of the bar area. */
		if (buffer_offset < buffer_size - 1)
			buffer[buffer_offset++] = ']';
	}

	/* The percentage. */
	if (after_bar_bytes > 0 && after_bar_bytes < (buffer_size - 1 - buffer_offset)) {
		memmove(buffer + buffer_offset, after_bar, after_bar_bytes);
		buffer_offset += after_bar_bytes;
	}

	buffer[buffer_offset] = '\0';

	return buffer_offset;
}


/*
 * Write a progress bar to a buffer, in unknown-size mode - just a moving
 * indicator.  The total width of the content is bounded to the given width. 
 * Returns the number of bytes written to the buffer.
 *
 * If "bar_sides" is false, only the bar itself is rendered, not the opening
 * and closing characters.
 *
 * This is only called by pv__format_progress().
 */
static size_t pv__format_progress_unknownsize(pvstate_t state,	/*@unused@ */
					      __attribute__((unused)) pvdisplay_t display, char *buffer,
					      size_t buffer_size, size_t width, bool bar_sides)
{
	size_t bar_area_width, buffer_offset, pad_count;
	size_t indicator_position;

	buffer[0] = '\0';

	if (bar_sides) {
		if (width < 6)
			return 0;
		bar_area_width = width - 5;
	} else {
		if (width < 5)
			return 0;
		bar_area_width = width - 3;
	}

	if (bar_area_width > buffer_size - 16)
		bar_area_width = buffer_size - 16;

	/*
	 * Note that pv_calculate_transfer_rate() sets the percentage when
	 * the size is unknown to a value that goes 0 - 200 and resets, so
	 * here we make values above 100 send the indicator back down again,
	 * so it moves back and forth.
	 */
	indicator_position = (size_t) (state->calc.percentage);
	if (indicator_position > 200)
		indicator_position = indicator_position % 200;
	if (indicator_position > 100 && indicator_position <= 200)
		indicator_position = 200 - indicator_position;

	buffer_offset = 0;

	if (bar_sides) {
		/* The opening of the bar area. */
		buffer[buffer_offset++] = '[';
	}

	/* The spaces before the indicator. */
	for (pad_count = 0; pad_count < (bar_area_width * indicator_position) / 100; pad_count++) {
		if (pad_count < bar_area_width && buffer_offset < buffer_size - 1)
			buffer[buffer_offset++] = ' ';
	}

	/* The indicator. */
	if (buffer_offset < buffer_size - 4) {
		buffer[buffer_offset++] = '<';
		buffer[buffer_offset++] = '=';
		buffer[buffer_offset++] = '>';
	}

	/* The spaces after the indicator. */
	for (; pad_count < bar_area_width; pad_count++) {
		if (pad_count < bar_area_width && buffer_offset < buffer_size - 1)
			buffer[buffer_offset++] = ' ';
	}

	if (bar_sides) {
		/* The closure of the bar area. */
		if (buffer_offset < buffer_size - 1)
			buffer[buffer_offset++] = ']';
	}

	buffer[buffer_offset] = '\0';

	return buffer_offset;
}


/*
 * Progress bar.
 */
static size_t pv__format_progress(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
				  size_t buffer_size, size_t offset)
{
	char content[1024];		 /* flawfinder: ignore - always bounded */
	size_t bytes;

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	if (state->control.size > 0 || state->control.rate_gauge) {
		/* Known size or rate gauge - bar with percentage. */
		bytes =
		    pv__format_progress_knownsize(state, display, content, sizeof(content), segment->width, true, true,
						  true);
	} else {
		/* Unknown size - back-and-forth moving indicator. */
		bytes = pv__format_progress_unknownsize(state, display, content, sizeof(content), segment->width, true);
	}

	content[bytes] = '\0';

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Progress bar, without sides and without a number afterwards.
 */
static size_t pv__format_progress_bar_only(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment,
					   char *buffer, size_t buffer_size, size_t offset)
{
	char content[1024];		 /* flawfinder: ignore - always bounded */
	size_t bytes;

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	if (state->control.size > 0 || state->control.rate_gauge) {
		/* Known size or rate gauge - bar with percentage. */
		bytes =
		    pv__format_progress_knownsize(state, display, content, sizeof(content), segment->width, false, true,
						  false);
	} else {
		/* Unknown size - back-and-forth moving indicator. */
		bytes =
		    pv__format_progress_unknownsize(state, display, content, sizeof(content), segment->width, false);
	}

	content[bytes] = '\0';

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * The number after the progress bar.
 */
static size_t pv__format_progress_amount_only(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment,
					      char *buffer, size_t buffer_size, size_t offset)
{
	char content[256];		 /* flawfinder: ignore - always bounded */
	size_t bytes;

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	if (state->control.size > 0 || state->control.rate_gauge) {
		/* Known size or rate gauge - percentage or rate. */
		bytes =
		    pv__format_progress_knownsize(state, display, content, sizeof(content), segment->width, false,
						  false, true);
	} else {
		/* Unknown size - no number. */
		return 0;
	}

	content[bytes] = '\0';

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Elapsed time.
 */
static size_t pv__format_timer(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
			       size_t buffer_size, size_t offset)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	display->showing_timer = true;

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	/*
	 * Bounds check, so we don't overrun the prefix buffer.  This does
	 * mean that the timer will stop at a 100,000 hours, but since
	 * that's 11 years, it shouldn't be a problem.
	 */
	if (state->transfer.elapsed_seconds > (long double) 360000000.0L)
		state->transfer.elapsed_seconds = (long double) 360000000.0L;

	/*
	 * If the elapsed time is more than a day, include a day count as
	 * well as hours, minutes, and seconds.
	 */
	if (state->transfer.elapsed_seconds > (long double) 86400.0L) {
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%ld:%02ld:%02ld:%02ld",
				   ((long) (state->transfer.elapsed_seconds)) / 86400,
				   (((long) (state->transfer.elapsed_seconds)) / 3600) %
				   24, (((long) (state->transfer.elapsed_seconds)) / 60) % 60,
				   ((long) (state->transfer.elapsed_seconds)) % 60);
	} else {
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%ld:%02ld:%02ld",
				   ((long) (state->transfer.elapsed_seconds)) / 3600,
				   (((long) (state->transfer.elapsed_seconds)) / 60) % 60,
				   ((long) (state->transfer.elapsed_seconds)) % 60);
	}

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Estimated time until completion.
 */
static size_t pv__format_eta(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
			     size_t buffer_size, size_t offset)
{
	char content[128];		 /* flawfinder: ignore - always bounded */
	long eta;

	content[0] = '\0';

	/*
	 * Don't try to calculate this if the size is not known.
	 */
	if (state->control.size < 1)
		return 0;

	if (0 == buffer_size)
		return 0;

	eta =
	    pv__seconds_remaining((state->transfer.transferred - display->initial_offset),
				  state->control.size - display->initial_offset, state->calc.current_avg_rate);

	/*
	 * Bounds check, so we don't overrun the suffix buffer.  This means
	 * the ETA will always be less than 100,000 hours.
	 */
	eta = bound_long(eta, 0, (long) 360000000L);

	/*
	 * If the ETA is more than a day, include a day count as well as
	 * hours, minutes, and seconds.
	 */
	/*@-mustfreefresh@ */
	if (eta > 86400L) {
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%.16s %ld:%02ld:%02ld:%02ld",
				   _("ETA"), eta / 86400, (eta / 3600) % 24, (eta / 60) % 60, eta % 60);
	} else {
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%.16s %ld:%02ld:%02ld", _("ETA"), eta / 3600, (eta / 60) % 60, eta % 60);
	}
	/*@+mustfreefresh@ *//* splint: see above. */

	/*
	 * If this is the final update, show a blank space where the ETA
	 * used to be.
	 */
	if (display->final_update) {
		size_t erase_idx;
		for (erase_idx = 0; erase_idx < sizeof(content) && content[erase_idx] != '\0'; erase_idx++) {
			content[erase_idx] = ' ';
		}
	}

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Estimated local time of completion.
 */
static size_t pv__format_fineta(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
				size_t buffer_size, size_t offset)
{
	char content[128];		 /* flawfinder: ignore - always bounded */
	time_t now, then;
	struct tm *time_ptr;
	long eta;
	const char *time_format;
	bool show_fineta;

	content[0] = '\0';

	/*
	 * Don't try to calculate this if the size is not known.
	 */
	if (state->control.size < 1)
		return 0;

	if (0 == buffer_size)
		return 0;

	now = time(NULL);
	show_fineta = true;
	time_format = NULL;

	/*
	 * The completion clock time may be hidden by a failed localtime
	 * lookup.
	 */

	eta = pv__seconds_remaining(state->transfer.transferred - display->initial_offset,
				    state->control.size - display->initial_offset, state->calc.current_avg_rate);

	/* Bounds check - see pv__format_eta(). */
	eta = bound_long(eta, 0, (long) 360000000L);

	/*
	 * Only include the date if the ETA is more than 6 hours
	 * away.
	 */
	if (eta > (long) (6 * 3600)) {
		time_format = "%Y-%m-%d %H:%M:%S";
	} else {
		time_format = "%H:%M:%S";
	}

	then = now + eta;
	time_ptr = localtime(&then);

	if (NULL == time_ptr) {
		show_fineta = false;
	} else {
		/*
		 * The localtime() function keeps data stored in a static
		 * buffer that gets overwritten by time functions.
		 */
		struct tm time = *time_ptr;
		size_t content_bytes;

		/*@-mustfreefresh@ */
		(void) pv_snprintf(content, sizeof(content), "%.16s ", _("FIN"));
		/*@+mustfreefresh@ *//* splint: see above. */
		content_bytes = strlen(content);	/* flawfinder: ignore */
		/* flawfinder: always bounded with \0 by pv_snprintf(). */
		(void) strftime(content + content_bytes, sizeof(content) - 1 - content_bytes, time_format, &time);
	}

	if (!show_fineta) {
		size_t erase_idx;
		for (erase_idx = 0; erase_idx < sizeof(content) && content[erase_idx] != '\0'; erase_idx++) {
			content[erase_idx] = ' ';
		}
	}

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Transfer rate.
 */
static size_t pv__format_rate(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
			      size_t buffer_size, size_t offset)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	display->showing_rate = true;

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	/*@-mustfreefresh@ */
	if (state->control.bits && !state->control.linemode) {
		/* bits per second */
		pv__sizestr(content, sizeof(content), "[%s]",
			    8 * state->calc.transfer_rate, "", _("b/s"), display->count_type);
	} else {
		/* bytes or lines per second */
		pv__sizestr(content, sizeof(content),
			    "[%s]", state->calc.transfer_rate, _("/s"), _("B/s"), display->count_type);
	}
	/*@+mustfreefresh@ *//* splint: see above. */

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Average transfer rate.
 */
static size_t pv__format_average_rate(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
				      size_t buffer_size, size_t offset)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	/*@-mustfreefresh@ */
	if (state->control.bits && !state->control.linemode) {
		/* bits per second */
		pv__sizestr(content, sizeof(content),
			    "(%s)", 8 * state->calc.average_rate, "", _("b/s"), display->count_type);
	} else {
		/* bytes or lines per second */
		pv__sizestr(content,
			    sizeof(content), "(%s)", state->calc.average_rate, _("/s"), _("B/s"), display->count_type);
	}
	/*@+mustfreefresh@ *//* splint: see above. */

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Number of bytes or lines transferred.
 */
static size_t pv__format_bytes(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
			       size_t buffer_size, size_t offset)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	display->showing_bytes = true;

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	/*@-mustfreefresh@ */
	if (state->control.bits && !state->control.linemode) {
		pv__sizestr(content, sizeof(content), "%s",
			    (long double) (state->transfer.transferred * 8), "", _("b"), display->count_type);
	} else {
		pv__sizestr(content, sizeof(content), "%s",
			    (long double) (state->transfer.transferred), "", _("B"), display->count_type);
	}
	/*@+mustfreefresh@ *//* splint - false positive from gettext(). */

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Percentage transfer buffer utilisation.
 */
static size_t pv__format_buffer_percent(pvstate_t state, /*@unused@ */  __attribute__((unused)) pvdisplay_t display,
					pvdisplay_segment_t segment, char *buffer, size_t buffer_size, size_t offset)
{
	char content[16];		 /* flawfinder: ignore - always bounded */

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	if (state->transfer.buffer_size > 0) {
		int pct_used = pv_percentage((off_t)
					     (state->transfer.read_position - state->transfer.write_position),
					     (off_t)
					     (state->transfer.buffer_size));
		(void) pv_snprintf(content, sizeof(content), "{%3d%%}", pct_used);
	}
#ifdef HAVE_SPLICE
	if (state->transfer.splice_used)
		(void) pv_snprintf(content, sizeof(content), "{%s}", "----");
#endif

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Display the last few bytes written.
 *
 * As a side effect, this sets display->lastwritten_bytes to the segment's
 * chosen_size, if it was previously smaller than that.
 */
static size_t pv__format_last_written(	 /*@unused@ */
					     __attribute__((unused)) pvstate_t state, pvdisplay_t display,
					     pvdisplay_segment_t segment, char *buffer, size_t buffer_size,
					     size_t offset)
{
	size_t bytes_to_show, read_offset, remaining;

	display->showing_last_written = true;

	bytes_to_show = segment->chosen_size;
	if (0 == bytes_to_show)
		bytes_to_show = segment->width;
	if (0 == bytes_to_show)
		return 0;

	if (bytes_to_show > PV_SIZEOF_LASTWRITTEN_BUFFER)
		bytes_to_show = PV_SIZEOF_LASTWRITTEN_BUFFER;
	if (bytes_to_show > display->lastwritten_bytes)
		display->lastwritten_bytes = bytes_to_show;

	if (0 == buffer_size)
		return 0;

	if (offset + bytes_to_show >= buffer_size)
		return 0;

	segment->offset = offset;
	segment->bytes = bytes_to_show;

	read_offset = display->lastwritten_bytes - bytes_to_show;
	for (remaining = bytes_to_show; remaining > 0; remaining--) {
		int display_char = (int) (display->lastwritten_buffer[read_offset++]);
		buffer[offset++] = isprint(display_char) ? (char) display_char : '.';
	}

	return bytes_to_show;
}


/*
 * Display the previously written line.
 */
static size_t pv__format_previous_line(	 /*@unused@ */
					      __attribute__((unused)) pvstate_t state, pvdisplay_t display,
					      pvdisplay_segment_t segment, char *buffer, size_t buffer_size,
					      size_t offset)
{
	size_t bytes_to_show, read_offset, remaining;

	display->showing_previous_line = true;

	if (0 == buffer_size)
		return 0;

	bytes_to_show = segment->chosen_size;
	if (0 == bytes_to_show)
		bytes_to_show = segment->width;
	if (0 == bytes_to_show)
		return 0;

	if (bytes_to_show > PV_SIZEOF_PREVLINE_BUFFER)
		bytes_to_show = PV_SIZEOF_PREVLINE_BUFFER;

	if (offset + bytes_to_show >= buffer_size)
		return 0;

	segment->offset = offset;
	segment->bytes = bytes_to_show;

	read_offset = 0;
	for (remaining = bytes_to_show; remaining > 0; remaining--) {
		int display_char = (int) (display->previous_line[read_offset++]);
		buffer[offset++] = isprint(display_char) ? (char) display_char : ' ';
	}

	return bytes_to_show;
}


/*
 * Display the transfer's name.
 */
static size_t pv__format_name(pvstate_t state, /*@unused@ */  __attribute__((unused)) pvdisplay_t display,
			      pvdisplay_segment_t segment, char *buffer, size_t buffer_size, size_t offset)
{
	char string_format[32];		 /* flawfinder: ignore - always bounded */
	char content[512];		 /* flawfinder: ignore - always bounded */
	size_t field_width;

	if (0 == buffer_size)
		return 0;

	field_width = segment->chosen_size;
	if (field_width < 1)
		field_width = 9;
	if (field_width > 500)
		field_width = 500;

	memset(string_format, 0, sizeof(string_format));
	(void) pv_snprintf(string_format, sizeof(string_format), "%%%d.500s:", field_width);

	content[0] = '\0';
	if (state->control.name) {
		(void) pv_snprintf(content, sizeof(content), string_format, state->control.name);
	}

	return pv__format_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Populate the display buffer for numeric-output mode.
 *
 * Called by pv_format() and has the same semantics.
 *
 * In numeric output mode, our output is just the percentage completion, as
 * a number by itself.
 *
 * With --timer, we prefix the output with the elapsed time.
 *
 * With --bytes, we output the bytes transferred so far instead of the
 * percentage (or we output the number lines transferred, if --lines was
 * given with --bytes).
 *
 * If --rate was given, we output the current transfer rate instead of the
 * percentage.  With --bytes as well, the rate is given after the
 * bytes/lines.
 */
static bool pv__format_numeric(pvstate_t state, pvdisplay_t display)
{
	char msg_timer[128];		 /* flawfinder: ignore */
	char msg_bytes[128];		 /* flawfinder: ignore */
	char msg_rate[128];		 /* flawfinder: ignore */
	char msg_percent[128];		 /* flawfinder: ignore */
	bool first_item, show_percentage;

	/* flawfinder: each buffer is kept safe by pv_snprintf(). */

	if (NULL == display->display_buffer)
		return false;

	first_item = true;
	show_percentage = true;

	msg_timer[0] = '\0';
	if (display->showing_timer) {
		(void) pv_snprintf(msg_timer, sizeof(msg_timer), "%s%.4Lf", first_item ? "" : " ",
				   state->transfer.elapsed_seconds);
		first_item = false;
	}

	msg_bytes[0] = '\0';
	if (display->showing_bytes) {
		(void) pv_snprintf(msg_bytes, sizeof(msg_bytes),
				   "%s%lld", first_item ? "" : " ",
				   (long long) ((state->control.bits ? 8 : 1) * state->transfer.transferred));
		first_item = false;
		show_percentage = false;
	}

	msg_rate[0] = '\0';
	if (display->showing_rate) {
		(void) pv_snprintf(msg_rate, sizeof(msg_rate),
				   "%s%.4Lf", first_item ? "" : " ",
				   ((state->control.bits ? 8.0 : 1.0) * state->calc.transfer_rate));
		first_item = false;
		show_percentage = false;
	}

	msg_percent[0] = '\0';
	if (show_percentage) {
		(void) pv_snprintf(msg_percent, sizeof(msg_percent),
				   "%s%d", first_item ? "" : " ", state->calc.percentage);
		first_item = false;
	}

	(void) pv_snprintf(display->display_buffer,
			   display->display_buffer_size, "%.39s%.39s%.39s%.39s\n", msg_timer, msg_bytes,
			   msg_rate, msg_percent);

	display->display_string_bytes = strlen(display->display_buffer);	/* flawfinder: ignore */
	/* flawfinder: always \0 terminated by pv_snprintf(). */
	display->display_string_width = display->display_string_bytes;

	return true;
}



/*
 * Format sequence lookup table.
 */
typedef size_t (*pvdisplay_function_t)(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
static struct pvdisplay_component_s {
	/*@null@ */ const char *match;
	/* string to match */
	/*@null@ */ pvdisplay_function_t function;
	/* function to call */
	bool dynamic;			 /* whether it can scale with screen size */
} format_component[] = {
	{ "p", &pv__format_progress, true },
	{ "{progress}", &pv__format_progress, true },
	{ "{progress-bar-only}", &pv__format_progress_bar_only, true },
	{ "{progress-amount-only}", &pv__format_progress_amount_only, false },
	{ "t", &pv__format_timer, false },
	{ "{timer}", &pv__format_timer, false },
	{ "e", &pv__format_eta, false },
	{ "{eta}", &pv__format_eta, false },
	{ "I", &pv__format_fineta, false },
	{ "{fineta}", &pv__format_fineta, false },
	{ "r", &pv__format_rate, false },
	{ "{rate}", &pv__format_rate, false },
	{ "a", &pv__format_average_rate, false },
	{ "{average-rate}", &pv__format_average_rate, false },
	{ "b", &pv__format_bytes, false },
	{ "{bytes}", &pv__format_bytes, false },
	{ "{transferred}", &pv__format_bytes, false },
	{ "T", &pv__format_buffer_percent, false },
	{ "{buffer-percent}", &pv__format_buffer_percent, false },
	{ "A", &pv__format_last_written, false },
	{ "{last-written}", &pv__format_last_written, false },
	{ "L", &pv__format_previous_line, true },
	{ "{previous-line}", &pv__format_previous_line, true },
	{ "N", &pv__format_name, false },
	{ "{name}", &pv__format_name, false },
	{ NULL, NULL, false }
};


/*
 * Initialise the output format structure, based on the current options.
 */
static void pv__format_init(pvstate_t state, /*@null@ */ const char *format_supplied, pvdisplay_t display)
{
	const char *display_format;
	size_t strpos;
	size_t segment;

	if (NULL == state)
		return;
	if (NULL == display)
		return;

	display->format_segment_count = 0;
	memset(display->format, 0, PV_FORMAT_ARRAY_MAX * sizeof(display->format[0]));

	display->showing_timer = false;
	display->showing_bytes = false;
	display->showing_rate = false;
	display->showing_last_written = false;
	display->showing_previous_line = false;

	display_format = NULL == format_supplied ? state->control.default_format : format_supplied;

	if (NULL == display_format)
		return;

	/*
	 * Split the format string into static strings and calculated
	 * components - a calculated component is is what replaces a
	 * placeholder sequence like "%b".
	 *
	 * A "static string" is part of the original format string that is
	 * copied to the display verbatim.  Its width is calculated here.
	 *
	 * Each segment's contents are stored in either the format string
	 * (if a static string) or an internal temporary buffer, starting at
	 * "offset" and extending for "bytes" bytes.
	 *
	 * Later, in pv_format(), segments whose components are dynamic and
	 * which aren't constrained to a fixed size are calculated after
	 * first populating all the other components referenced by the
	 * format segments.
	 *
	 * Then, that function generates the output string by sticking all
	 * of these segments together.
	 */
	segment = 0;
	for (strpos = 0; display_format[strpos] != '\0' && segment < PV_FORMAT_ARRAY_MAX; strpos++, segment++) {
		int component_type, component_idx;
		size_t str_start, str_bytes, chosen_size;

		str_start = strpos;
		str_bytes = 0;

		chosen_size = 0;

		if ('%' == display_format[strpos]) {
			unsigned long number_prefix;
			size_t percent_sign_offset, sequence_start, sequence_length;
#if HAVE_STRTOUL
			char *number_end_ptr;
#endif

			percent_sign_offset = strpos;
			strpos++;

			/*
			 * Check for a numeric prefix between the % and the
			 * format character - currently only used with "%A"
			 * and "%L".
			 */
#if HAVE_STRTOUL
			number_end_ptr = NULL;
			number_prefix = strtoul(&(display_format[strpos]), &number_end_ptr, 10);
			if ((NULL == number_end_ptr) || (number_end_ptr[0] == '\0')) {
				number_prefix = 0;
			} else if (number_end_ptr > &(display_format[strpos])) {
				strpos += (number_end_ptr - &(display_format[strpos]));
			}
#else				/* !HAVE_STRTOUL */
			while (isdigit((int) (display_format[strpos]))) {
				number_prefix = number_prefix * 10;
				number_prefix += display_format[strpos] - '0';
				strpos++;
			}
#endif				/* !HAVE_STRTOUL */

			sequence_start = strpos;
			sequence_length = 0;
			if ('\0' != display_format[strpos])
				sequence_length = 1;
			if ('{' == display_format[strpos]) {
				while ('\0' != display_format[strpos] && '}' != display_format[strpos]
				       && '%' != display_format[strpos]) {
					strpos++;
					sequence_length++;
				}
			}

			component_type = -1;
			for (component_idx = 0; NULL != format_component[component_idx].match; component_idx++) {
				size_t component_sequence_length = strlen(format_component[component_idx].match);	/* flawfinder: ignore */
				/* flawfinder - static strings, guaranteed null-terminated. */
				if (component_sequence_length != sequence_length)
					continue;
				if (0 !=
				    strncmp(format_component[component_idx].match, &(display_format[sequence_start]),
					    sequence_length))
					continue;
				component_type = component_idx;
				break;
			}

			if (-1 == component_type) {
				/* Unknown sequence - pass it through verbatim. */
				str_start = percent_sign_offset;
				str_bytes = sequence_length + sequence_start - percent_sign_offset;

				if (2 == str_bytes && '%' == display_format[percent_sign_offset + 1]) {
					/* Special case: "%%" => "%". */
					str_bytes = 1;
				} else if (str_bytes > 1 && '%' == display_format[strpos]) {
					/* Special case: "%{foo%p" => "%{foo" and go back one. */
					str_bytes--;
					strpos--;
				} else if (str_bytes == 0 && '\0' == display_format[strpos]) {
					/* Special case: "%" at end of string = "%". */
					str_bytes = 1;
				}
			} else {
				chosen_size = (size_t) number_prefix;
			}

		} else {
			const char *searchptr;
			int foundlength;

			searchptr = strchr(&(display_format[strpos]), '%');
			if (NULL == searchptr) {
				foundlength = (int) strlen(&(display_format[strpos]));	/* flawfinder: ignore */
				/* flawfinder: display_format is explicitly \0-terminated. */
			} else {
				foundlength = searchptr - &(display_format[strpos]);
			}

			component_type = -1;
			str_start = strpos;
			str_bytes = (size_t) foundlength;

			strpos += foundlength - 1;
		}

		display->format[segment].type = component_type;
		display->format[segment].chosen_size = chosen_size;

		if (-1 == component_type) {
			if (0 == str_bytes)
				continue;

			display->format[segment].offset = str_start;
			display->format[segment].bytes = str_bytes;
			display->format[segment].width = pv_strwidth(&(display_format[str_start]), str_bytes);

		} else {
			char dummy_buffer[4];	/* flawfinder: ignore - unused. */

			display->format[segment].offset = 0;
			display->format[segment].bytes = 0;

			/*
			 * Run the formatter function with a zero-sized
			 * buffer, to invoke its side effects such as
			 * setting display->showing_timer.
			 *
			 * These side effects are required for other parts
			 * of the program to understand what is required,
			 * such as the transfer functions knowning to track
			 * the previous line, or numeric mode knowing which
			 * additional display options are enabled.
			 */
			dummy_buffer[0] = '\0';
			(void) format_component[component_type].function(state, display, &(display->format[segment]),
									 dummy_buffer, 0, 0);
		}

		display->format_segment_count++;
	}
}


/*
 * Update display->display_buffer with status information formatted
 * according to the state held within the given structure.
 *
 * If "reinitialise" is true, the format string is reparsed first.  This
 * should be true for the first call, and true whenever the format is
 * changed.
 *
 * If "final" is true, this is the final update so the rate is given as an
 * an average over the whole transfer; otherwise the current rate is shown.
 *
 * Returns true if the display buffer can be used, false if not.
 *
 * When returning true, this function will have also set
 * display->display_string_len to the length of the string in
 * display->display_buffer, in bytes.
 */
bool pv_format(pvstate_t state, /*@null@ */ const char *format_supplied, pvdisplay_t display, bool reinitialise,
	       bool final)
{
	char display_segments[1024];	 /* flawfinder: ignore - always bounded */
	size_t display_segment_offset;
	size_t segment_idx, dynamic_segment_count;
	const char *display_format;
	size_t static_portion_width, dynamic_segment_width;
	size_t display_buffer_offset, display_buffer_remaining;
	size_t new_display_string_bytes, new_display_string_width;

	display_segments[0] = '\0';

	/* Quick safety check - state and display must exist. */
	if (NULL == state)
		return false;
	if (NULL == display)
		return false;

	/* Populate the display's "final" flag, for formatters. */
	display->final_update = final;

	/* Reinitialise if we were asked to. */
	if (reinitialise)
		pv__format_init(state, format_supplied, display);

	/* The format string is needed for the static segments. */
	display_format = NULL == format_supplied ? state->control.default_format : format_supplied;
	if (NULL == display_format)
		return false;

	/* Determine the type of thing being counted for transfer, rate, etc. */
	display->count_type = PV_TRANSFERCOUNT_BYTES;
	if (state->control.linemode)
		display->count_type = PV_TRANSFERCOUNT_LINES;
	else if (state->control.decimal_units)
		display->count_type = PV_TRANSFERCOUNT_DECBYTES;

	/*
	 * Reallocate the output buffer if the display width changes.
	 */
	if (display->display_buffer != NULL && display->display_buffer_size < (size_t) ((state->control.width * 2))) {
		free(display->display_buffer);
		display->display_buffer = NULL;
		display->display_buffer_size = 0;
	}

	/*
	 * Allocate output buffer if there isn't one.
	 */
	if (NULL == display->display_buffer) {
		char *new_buffer;
		size_t new_size;

		new_size = (size_t) ((2 * state->control.width) + 80);
		if (NULL != state->control.name)
			new_size += strlen(state->control.name);	/* flawfinder: ignore */
		/* flawfinder: name is always set by pv_strdup(), which bounds with a \0. */

		new_buffer = malloc(new_size + 16);
		if (NULL == new_buffer) {
			pv_error(state, "%s: %s", _("buffer allocation failed"), strerror(errno));
			state->status.exit_status |= PV_ERROREXIT_MEMORY;
			display->display_buffer = NULL;
			return false;
		}

		display->display_buffer = new_buffer;
		display->display_buffer_size = new_size;
		display->display_buffer[0] = '\0';
	}

	/*
	 * Use the numeric mode function if we're in numeric mode.
	 */
	if (state->control.numeric) {
		return pv__format_numeric(state, display);
	}

	/*
	 * Populate the internal segments buffer with each component's
	 * output, in two passes.
	 */

	display_segment_offset = 0;

	/* First pass - all components with a fixed width. */

	static_portion_width = 0;
	dynamic_segment_count = 0;

	for (segment_idx = 0; segment_idx < display->format_segment_count; segment_idx++) {
		pvdisplay_segment_t segment;
		struct pvdisplay_component_s *component;
		size_t bytes_added;
		bool fixed_width;

		segment = &(display->format[segment_idx]);
		if (-1 == segment->type) {
			static_portion_width += segment->width;
			continue;
		}
		component = &(format_component[segment->type]);

		fixed_width = true;
		if (component->dynamic && 0 == segment->chosen_size)
			fixed_width = false;

		if (!fixed_width) {
			dynamic_segment_count++;
			continue;
		}

		segment->width = segment->chosen_size;

		bytes_added =
		    component->function(state, display, segment, display_segments, sizeof(display_segments),
					display_segment_offset);

		segment->width = 0;
		if (bytes_added > 0) {
			segment->width = pv_strwidth(&(display_segments[display_segment_offset]), bytes_added);
		}

		display_segment_offset += bytes_added;
		static_portion_width += segment->width;
	}

	/*
	 * Second pass, now the remaining width is known - all components
	 * with a dynamic width.
	 */

	dynamic_segment_width = state->control.width - static_portion_width;

	/*
	 * Divide the total remaining screen space by the number of dynamic
	 * segments, so that multiple dynamic segments will share the space.
	 */
	if (dynamic_segment_count > 1)
		dynamic_segment_width /= dynamic_segment_count;

	for (segment_idx = 0; segment_idx < display->format_segment_count; segment_idx++) {
		pvdisplay_segment_t segment;
		struct pvdisplay_component_s *component;
		size_t bytes_added;
		bool fixed_width;

		segment = &(display->format[segment_idx]);
		if (-1 == segment->type) {
			static_portion_width += segment->width;
			continue;
		}
		component = &(format_component[segment->type]);

		fixed_width = true;
		if (component->dynamic && 0 == segment->chosen_size)
			fixed_width = false;

		if (fixed_width)
			continue;

		segment->width = dynamic_segment_width;
		bytes_added =
		    component->function(state, display, segment, display_segments, sizeof(display_segments),
					display_segment_offset);

		display_segment_offset += bytes_added;
	}

	/*
	 * Populate the display buffer from the segments.
	 */

	memset(display->display_buffer, 0, display->display_buffer_size);
	display_buffer_offset = 0;
	display_buffer_remaining = display->display_buffer_size - 1;
	new_display_string_bytes = 0;
	new_display_string_width = 0;

	for (segment_idx = 0; segment_idx < display->format_segment_count; segment_idx++) {
		pvdisplay_segment_t segment;
		const char *content_buffer = display_format;

		segment = &(display->format[segment_idx]);
		if (0 == segment->bytes)
			continue;
		if (segment->bytes > display_buffer_remaining)
			continue;

		if (-1 == segment->type) {
			content_buffer = display_format;
		} else {
			content_buffer = display_segments;
		}

		memmove(display->display_buffer + display_buffer_offset, content_buffer + segment->offset,
			segment->bytes);
		display_buffer_offset += segment->bytes;
		display_buffer_remaining -= segment->bytes;

		new_display_string_bytes += segment->bytes;
		new_display_string_width += segment->width;
	}

	debug("%s: %d", "new display string length in bytes", (int) new_display_string_bytes);
	debug("%s: %d", "new display string width", (int) new_display_string_width);

	/*
	 * If the width of our output shrinks, we need to keep appending
	 * spaces at the end, so that we don't leave dangling bits behind.
	 */
	if ((new_display_string_width < display->display_string_width)
	    && (state->control.width >= display->prev_screen_width)) {
		char spaces[32];	 /* flawfinder: ignore - terminated, bounded */
		int spaces_to_add;

		spaces_to_add = (int) (display->display_string_width - new_display_string_width);
		/* Upper boundary on number of spaces */
		if (spaces_to_add > 15) {
			spaces_to_add = 15;
		}
		new_display_string_bytes += spaces_to_add;
		new_display_string_width += spaces_to_add;
		spaces[spaces_to_add] = '\0';
		while (--spaces_to_add >= 0) {
			spaces[spaces_to_add] = ' ';
		}
		(void) pv_strlcat(display->display_buffer, spaces, display->display_buffer_size);
	}

	display->display_string_bytes = new_display_string_bytes;
	display->display_string_width = new_display_string_width;
	display->prev_screen_width = state->control.width;

	return true;
}


/*
 * Output status information on standard error.
 *
 * If "final" is true, this is the final update, so the rate is given as an
 * an average over the whole transfer; otherwise the current rate is shown.
 */
void pv_display(pvstate_t state, bool final)
{
	bool reinitialise = false;

	if (NULL == state)
		return;

	pv_sig_checkbg();

	pv_calculate_transfer_rate(state, final);

	/*
	 * If the display options need reparsing, do so to generate new
	 * formatting parameters.
	 */
	if (0 != state->flag.reparse_display) {
		reinitialise = true;
		state->flag.reparse_display = 0;
	}

	if (!pv_format(state, state->control.format_string, &(state->display), reinitialise, final))
		return;

	if (0 != state->control.extra_displays) {
		if (!pv_format(state, state->control.extra_format_string, &(state->extra_display), reinitialise, final))
			return;
	}

	if (NULL == state->display.display_buffer)
		return;

	if (state->control.numeric) {
		pv_tty_write(state, state->display.display_buffer, state->display.display_string_bytes);
	} else if (state->control.cursor) {
		if (state->control.force || pv_in_foreground()) {
			pv_crs_update(state, state->display.display_buffer);
			state->display.display_visible = true;
		}
	} else {
		if (state->control.force || pv_in_foreground()) {
			pv_tty_write(state, state->display.display_buffer, state->display.display_string_bytes);
			pv_tty_write(state, "\r", 1);
			state->display.display_visible = true;
		}
	}

	debug("%s: [%s]", "display", state->display.display_buffer);

	if ((0 != (PV_DISPLAY_WINDOWTITLE & state->control.extra_displays))
	    && (state->control.force || pv_in_foreground())
	    && (NULL != state->extra_display.display_buffer)
	    ) {
		pv_tty_write(state, "\033]2;", 4);
		pv_tty_write(state, state->extra_display.display_buffer, state->extra_display.display_string_bytes);
		pv_tty_write(state, "\033\\", 2);
		state->extra_display.display_visible = true;
		debug("%s: [%s]", "windowtitle display", state->extra_display.display_buffer);
	}

	if ((0 != (PV_DISPLAY_PROCESSTITLE & state->control.extra_displays))
	    && (NULL != state->extra_display.display_buffer)
	    ) {
		setproctitle("%s", state->extra_display.display_buffer);
		state->extra_display.display_visible = true;
		debug("%s: [%s]", "processtitle display", state->extra_display.display_buffer);
	}
}
