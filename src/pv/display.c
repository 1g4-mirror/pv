/*
 * Display functions.
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
		debug("%s: true: %s", "pv_in_foreground", "not a tty");
		return true;
	}

	/*@-type@ *//* __pid_t vs pid_t, not significant */
	our_process_group = getpgrp();
	tty_process_group = tcgetpgrp(STDERR_FILENO);
	/*@+type@ */

	if (tty_process_group == -1 && errno == ENOTTY) {
		debug("%s: true: %s", "pv_in_foreground", "tty_process_group is -1, errno is ENOTTY");
		return true;
	}

	if (our_process_group == tty_process_group) {
		debug("%s: true: %s", "pv_in_foreground", "our_process_group == tty_process_group");
		return true;
	}

	debug("%s: false: our_process_group=%d, tty_process_group=%d",
	      "pv_in_foreground", our_process_group, tty_process_group);

	return false;
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
 * Calculate the percentage transferred so far and return it.
 */
static int pv__calc_percentage(off_t so_far, const off_t total)
{
	if (total < 1)
		return 0;

	so_far *= 100;
	so_far /= total;

	return (int) so_far;
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
 * Types of transfer count - bytes or lines.
 */
typedef enum {
	PV_TRANSFERCOUNT_BYTES,
	PV_TRANSFERCOUNT_LINES
} pv__transfercount_t;

/*
 * Given a long double value, it is divided or multiplied by the ratio until
 * a value in the range 1.0 to 999.999... is found.  The string "prefix" to
 * is updated to the corresponding SI prefix.
 *
 * If the count type is PV_TRANSFERCOUNT_BYTES, then the second byte of
 * "prefix" is set to "i" to denote MiB etc (IEEE1541).  Thus "prefix"
 * should be at least 3 bytes long (to include the terminating null).
 */
static void pv__si_prefix(long double *value, char *prefix, const long double ratio, pv__transfercount_t count_type)
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
			long double amount, char *suffix_basic, char *suffix_bytes, pv__transfercount_t count_type)
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
 * Initialise the output format structure, based on the current options.
 */
static void pv__format_init(pvstate_t state)
{
	const char *formatstr;
	size_t strpos;
	size_t segment;

	if (NULL == state)
		return;

	state->display.format_segment_count = 0;
	memset(state->display.format, 0, PV_FORMAT_ARRAY_MAX * sizeof(state->display.format[0]));
	memset(state->display.component, 0, PV_COMPONENT__MAX * sizeof(state->display.component[0]));

	if (state->control.name) {
		(void) pv_snprintf(state->display.component[PV_COMPONENT_NAME].content, PV_SIZEOF_COMPONENT_STR,
				   "%9.500s:", state->control.name);
		state->display.component[PV_COMPONENT_NAME].length = strlen(state->display.component[PV_COMPONENT_NAME].content);	/* flawfinder: ignore */
		/* flawfinder: content always bounded thanks to pv_snprintf(). */
	}

	formatstr = state->control.format_string ? state->control.format_string : state->control.default_format;

	if (NULL == formatstr)
		return;

	/*
	 * Split the format string into segments.  Each segment consists
	 * of a type and some string information.
	 *
	 * A type of PV_COMPONENT_STRING indicates that the segment is a
	 * constant string starting at a position in the format string and
	 * with a particular length.
	 *
	 * A type other than PV_COMPONENT_STRING indicates that the segment
	 * is a string updated by pv__format(), whose contents will be in
	 * component[type].
	 *
	 * In pv__format(), the content of a PV_COMPONENT_PROGRESS component
	 * is calculated after first populating all the other components
	 * referenced by the format segments.
	 *
	 * Then, pv_format() generates the output string by sticking all of
	 * these segments together.
	 */
	segment = 0;
	for (strpos = 0; formatstr[strpos] != '\0' && segment < PV_FORMAT_ARRAY_MAX; strpos++, segment++) {
		pv_display_component seg_type;
		size_t str_start, str_length;

		if ('%' == formatstr[strpos]) {
			unsigned long number_prefix;
#if HAVE_STRTOUL
			char *number_end_ptr;
#endif

			strpos++;

			/*
			 * Check for a numeric prefix between the % and the
			 * format character - currently only used with "%A".
			 */
#if HAVE_STRTOUL
			number_end_ptr = NULL;
			number_prefix = strtoul(&(formatstr[strpos]), &number_end_ptr, 10);
			if ((NULL == number_end_ptr) || (number_end_ptr[0] == '\0'))
				break;
			if (number_end_ptr > &(formatstr[strpos]))
				strpos += (number_end_ptr - &(formatstr[strpos]));
#else				/* !HAVE_STRTOUL */
			while (isdigit((int) (formatstr[strpos]))) {
				number_prefix = number_prefix * 10;
				number_prefix += formatstr[strpos] - '0';
				strpos++;
			}
#endif				/* !HAVE_STRTOUL */

			seg_type = PV_COMPONENT_STRING;
			str_start = 0;
			str_length = 0;

			switch (formatstr[strpos]) {
			case 'p':
				seg_type = PV_COMPONENT_PROGRESS;
				break;
			case 't':
				seg_type = PV_COMPONENT_TIMER;
				break;
			case 'e':
				seg_type = PV_COMPONENT_ETA;
				break;
			case 'I':
				seg_type = PV_COMPONENT_FINETA;
				break;
			case 'A':
				seg_type = PV_COMPONENT_OUTPUTBUF;
				if (number_prefix > PV_SIZEOF_LASTOUTPUT_BUFFER)
					number_prefix = PV_SIZEOF_LASTOUTPUT_BUFFER;
				if (number_prefix < 1)
					number_prefix = 1;
				state->display.lastoutput_length = (size_t) number_prefix;
				break;
			case 'r':
				seg_type = PV_COMPONENT_RATE;
				break;
			case 'a':
				seg_type = PV_COMPONENT_AVERAGERATE;
				break;
			case 'b':
				seg_type = PV_COMPONENT_BYTES;
				break;
			case 'T':
				seg_type = PV_COMPONENT_BUFPERCENT;
				break;
			case 'N':
				seg_type = PV_COMPONENT_NAME;
				break;
			case '%':
				/* %% => % */
				seg_type = PV_COMPONENT_STRING;
				str_start = strpos;
				str_length = 1;
				break;
			case '\0':
				/* % at end => just % */
				seg_type = PV_COMPONENT_STRING;
				str_start = strpos - 1;
				str_length = 1;
				break;
			default:
				/* %z (unknown) => %z */
				seg_type = PV_COMPONENT_STRING;
				str_start = strpos - 1;
				str_length = 2;
				break;
			}
		} else {
			const char *searchptr;
			int foundlength;

			searchptr = strchr(&(formatstr[strpos]), '%');
			if (NULL == searchptr) {
				foundlength = (int) strlen(&(formatstr[strpos]));	/* flawfinder: ignore */
				/* flawfinder: formatstr is explicitly \0-terminated. */
			} else {
				foundlength = searchptr - &(formatstr[strpos]);
			}

			seg_type = PV_COMPONENT_STRING;
			str_start = strpos;
			str_length = (size_t) foundlength;

			strpos += foundlength - 1;
		}

		if (seg_type != PV_COMPONENT_STRING)
			state->display.component[seg_type].required = true;

		state->display.format[segment].type = seg_type;
		state->display.format[segment].str_start = str_start;
		state->display.format[segment].str_length = str_length;
		state->display.format_segment_count++;
	}
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
 * Update the current average rate, using a ring buffer of past transfer
 * positions - if this is the first entry, use the provided instantaneous
 * rate, otherwise calulate the average rate from the difference between the
 * current position + elapsed time pair, and the oldest pair in the buffer.
 */
static void pv__update_average_rate_history(pvstate_t state, off_t total_bytes, long double elapsed_sec,
					    long double rate)
{
	size_t first = state->display.history_first;
	size_t last = state->display.history_last;
	long double last_elapsed;

	if (NULL == state->display.history)
		return;

	last_elapsed = state->display.history[last].elapsed_sec;

	/*
	 * Do nothing if this is not the first call but not enough time has
	 * elapsed since the previous call yet.
	 */
	if ((last_elapsed > 0.0)
	    && (elapsed_sec < (last_elapsed + state->display.history_interval)))
		return;

	/*
	 * If this is not the first call, add a new entry to the circular
	 * buffer.
	 */
	if (last_elapsed > 0.0) {
		size_t len = state->display.history_len;
		last = (last + 1) % len;
		state->display.history_last = last;
		if (last == first) {
			first = (first + 1) % len;
			state->display.history_first = first;
		}
	}

	state->display.history[last].elapsed_sec = elapsed_sec;
	state->display.history[last].total_bytes = total_bytes;

	if (first == last) {
		state->display.current_avg_rate = rate;
	} else {
		off_t bytes = (state->display.history[last].total_bytes - state->display.history[first].total_bytes);
		long double sec =
		    (state->display.history[last].elapsed_sec - state->display.history[first].elapsed_sec);
		state->display.current_avg_rate = (long double) bytes / sec;
	}
}

/*
 * Update state->display.display_buffer with status information formatted
 * according to the state held within the given structure, where
 * "elapsed_sec" is the seconds elapsed since the transfer started,
 * "bytes_since_last" is the number of bytes transferred since the last
 * update, and "total_bytes" is the total number of bytes transferred so
 * far.
 *
 * If "bytes_since_last" is negative, this is the final update so the rate
 * is given as an an average over the whole transfer; otherwise the current
 * rate is shown.
 *
 * In line mode, "bytes_since_last" and "total_bytes" are in lines, not bytes.
 *
 * Returns true if the display buffer can be used, false if not.
 *
 * When returning true, this function will have also set
 * state->display.display_string_len to the length of the string in
 * state->display.display_buffer, in bytes.
 *
 * If "total_bytes" is negative, then free the display buffer and return
 * false.
 */
static bool pv__format(pvstate_t state, long double elapsed_sec, off_t bytes_since_last, off_t total_bytes)
{
	long double time_since_last, rate, average_rate;
	long eta;
	int static_portion_size;
	pv_display_component component_type;
	size_t segment;
	size_t new_display_string_len;
	const char *formatstr;

	/* Quick sanity check - state must exist. */
	if (NULL == state)
		return false;

	/* Negative total transfer - free memory and return false. */
	if (total_bytes < 0) {
		if (NULL != state->display.display_buffer)
			free(state->display.display_buffer);
		state->display.display_buffer = NULL;
		state->display.display_buffer_size = 0;
		return false;
	}

	/* The format string is needed for the static segments. */
	formatstr = state->control.format_string ? state->control.format_string : state->control.default_format;
	if (NULL == formatstr)
		return false;

	/*
	 * In case the time since the last update is very small, we keep
	 * track of amount transferred since the last update, and just keep
	 * adding to that until a reasonable amount of time has passed to
	 * avoid rate spikes or division by zero.
	 */
	time_since_last = elapsed_sec - state->display.prev_elapsed_sec;
	if (time_since_last <= 0.01) {
		rate = state->display.prev_rate;
		state->display.prev_trans += bytes_since_last;
	} else {
		rate = ((long double) bytes_since_last + state->display.prev_trans) / time_since_last;
		state->display.prev_elapsed_sec = elapsed_sec;
		state->display.prev_trans = 0;
	}
	state->display.prev_rate = rate;

	/* Update history and current average rate for ETA. */
	pv__update_average_rate_history(state, total_bytes, elapsed_sec, rate);
	average_rate = state->display.current_avg_rate;

	/*
	 * If this is the final update at the end of the transfer, we
	 * recalculate the rate - and the average rate - across the whole
	 * period of the transfer.
	 */
	if (bytes_since_last < 0) {
		/* Sanity check to avoid division by zero */
		if (elapsed_sec < 0.000001)
			elapsed_sec = 0.000001;
		average_rate =
		    (((long double) total_bytes) -
		     ((long double) state->display.initial_offset)) / (long double) elapsed_sec;
		rate = average_rate;
	}

	if (state->control.size <= 0) {
		/*
		 * If we don't know the total size of the incoming data,
		 * then for a percentage, we gradually increase the
		 * percentage completion as data arrives, to a maximum of
		 * 200, then reset it - we use this if we can't calculate
		 * it, so that the numeric percentage output will go
		 * 0%-100%, 100%-0%, 0%-100%, and so on.
		 */
		if (rate > 0)
			state->display.percentage += 2;
		if (state->display.percentage > 199)
			state->display.percentage = 0;
	} else if (state->control.numeric || state->display.component[PV_COMPONENT_PROGRESS].required) {
		/*
		 * If we do know the total size, and we're going to show
		 * the percentage (numeric mode or a progress bar),
		 * calculate the percentage completion.
		 */
		state->display.percentage = pv__calc_percentage(total_bytes, state->control.size);
	}

	/*
	 * Reallocate output buffer if width changes.
	 */
	if (state->display.display_buffer != NULL
	    && state->display.display_buffer_size < (size_t) ((state->control.width * 2))) {
		free(state->display.display_buffer);
		state->display.display_buffer = NULL;
		state->display.display_buffer_size = 0;
	}

	/*
	 * Allocate output buffer if there isn't one.
	 */
	if (NULL == state->display.display_buffer) {
		char *new_buffer;
		size_t new_size;

		new_size = (size_t) ((2 * state->control.width) + 80);
		if (NULL != state->control.name)
			new_size += strlen(state->control.name);	/* flawfinder: ignore */
		/* flawfinder: name is always set by pv_strdup(), which bounds with a \0. */

		new_buffer = malloc(new_size + 16);
		if (NULL == new_buffer) {
			pv_error(state, "%s: %s", _("buffer allocation failed"), strerror(errno));
			state->status.exit_status |= 64;
			state->display.display_buffer = NULL;
			return false;
		}

		state->display.display_buffer = new_buffer;
		state->display.display_buffer_size = new_size;
		state->display.display_buffer[0] = '\0';
	}

	/*
	 * In numeric output mode, our output is just a number.
	 *
	 * Patch from Sami Liedes:
	 * With --timer we prefix the output with the elapsed time.
	 * With --bytes we output the bytes transferred so far instead
	 * of the percentage. (Or lines, if --lines was given with --bytes).
	 */
	if (state->control.numeric) {
		char numericprefix[128]; /* flawfinder: ignore - only populated by pv_snprintf(). */

		numericprefix[0] = '\0';

		if (state->display.component[PV_COMPONENT_TIMER].required)
			(void) pv_snprintf(numericprefix, sizeof(numericprefix), "%.4Lf ", elapsed_sec);

		if (state->display.component[PV_COMPONENT_BYTES].required) {
			if (state->control.bits) {
				(void) pv_snprintf(state->display.display_buffer,
						   state->display.display_buffer_size,
						   "%.99s%lld\n", numericprefix, (long long) (8 * total_bytes));
			} else {
				(void) pv_snprintf(state->display.display_buffer,
						   state->display.display_buffer_size,
						   "%.99s%lld\n", numericprefix, (long long) total_bytes);
			}
		} else {
			(void) pv_snprintf(state->display.display_buffer,
					   state->display.display_buffer_size, "%.99s%ld\n", numericprefix,
					   (long) (state->display.percentage));
		}

		state->display.display_string_len = strlen(state->display.display_buffer);	/* flawfinder: ignore */
		/* flawfinder: always \0 terminated by pv_snprintf(). */

		return true;
	}

	/*
	 * First, work out what components we will be putting in the output
	 * buffer, and for those that don't depend on the total width
	 * available (i.e. all but the progress bar), prepare their strings
	 * to be placed in the output buffer.
	 */

	for (component_type = 0; component_type < PV_COMPONENT__MAX; component_type++) {
		char *component_content;
		size_t component_buf_size;
		size_t buf_idx;
		bool show_eta;
		time_t now;
		time_t then;
		struct tm *time_ptr;
		char *time_format;

		if (!state->display.component[component_type].required)
			continue;

		/*
		 * Don't try to calculate ETA if the size is not known.  We
		 * check here to avoid big indented blocks if we check
		 * later.
		 */
		if (state->control.size < 1
		    && ((component_type == PV_COMPONENT_ETA) || (component_type == PV_COMPONENT_FINETA))) {
			state->display.component[component_type].content[0] = '\0';
			state->display.component[component_type].length = 0;
			continue;
		}

		component_content = state->display.component[component_type].content;
		component_content[0] = '\0';
		component_buf_size = PV_SIZEOF_COMPONENT_STR;

		switch (component_type) {

		case PV_COMPONENT_STRING:
			break;

		case PV_COMPONENT_PROGRESS:
			/* Progress bar - variable width, so do this later. */
			break;

		case PV_COMPONENT_BYTES:
			/* Bytes / bits / lines transferred. */
			/*@-mustfreefresh @ */
			if (state->control.bits && !state->control.linemode) {
				pv__sizestr(component_content, component_buf_size, "%s",
					    (long double) total_bytes * 8, "", _("b"), PV_TRANSFERCOUNT_BYTES);
			} else {
				pv__sizestr(component_content, component_buf_size, "%s",
					    (long double) total_bytes, "", _("B"),
					    state->control.linemode ? PV_TRANSFERCOUNT_LINES : PV_TRANSFERCOUNT_BYTES);
			}
			/*@+mustfreefresh @ */
			/* splint: we trust gettext() not to really leak memory. */
			break;

		case PV_COMPONENT_TIMER:
			/* Elapsed time. */
			/*
			 * Bounds check, so we don't overrun the prefix buffer. This
			 * does mean that the timer will stop at a 100,000 hours,
			 * but since that's 11 years, it shouldn't be a problem.
			 */
			if (elapsed_sec > (long double) 360000000.0L)
				elapsed_sec = (long double) 360000000.0L;

			/*
			 * If the elapsed time is more than a day, include a day count as
			 * well as hours, minutes, and seconds.
			 */
			if (elapsed_sec > (long double) 86400.0L) {
				(void) pv_snprintf(component_content,
						   component_buf_size,
						   "%ld:%02ld:%02ld:%02ld",
						   ((long) elapsed_sec) / 86400,
						   (((long) elapsed_sec) / 3600) %
						   24, (((long) elapsed_sec) / 60) % 60, ((long) elapsed_sec) % 60);
			} else {
				(void) pv_snprintf(component_content,
						   component_buf_size,
						   "%ld:%02ld:%02ld",
						   ((long) elapsed_sec) / 3600,
						   (((long) elapsed_sec) / 60) % 60, ((long) elapsed_sec) % 60);
			}
			break;

		case PV_COMPONENT_RATE:
			/* Current transfer rate. */
			/*@-mustfreefresh @ */
			if (state->control.bits && !state->control.linemode) {
				/* bits per second */
				pv__sizestr(component_content, component_buf_size, "[%s]", 8 * rate, "", _("b/s"),
					    PV_TRANSFERCOUNT_BYTES);
			} else {
				/* bytes or lines per second */
				pv__sizestr(component_content, component_buf_size,
					    "[%s]", rate, _("/s"), _("B/s"),
					    state->control.linemode ? PV_TRANSFERCOUNT_LINES : PV_TRANSFERCOUNT_BYTES);
			}
			/*@+mustfreefresh @ *//* splint: see above. */
			break;

		case PV_COMPONENT_AVERAGERATE:
			/* Average transfer rate. */
			/*@-mustfreefresh @ */
			if (state->control.bits && !state->control.linemode) {
				/* bits per second */
				pv__sizestr(component_content, component_buf_size,
					    "[%s]", 8 * average_rate, "", _("b/s"), PV_TRANSFERCOUNT_BYTES);
			} else {
				/* bytes or lines per second */
				pv__sizestr(component_content,
					    component_buf_size,
					    "[%s]", average_rate, _("/s"), _("B/s"),
					    state->control.linemode ? PV_TRANSFERCOUNT_LINES : PV_TRANSFERCOUNT_BYTES);
			}
			/*@+mustfreefresh @ *//* splint: see above. */
			break;

		case PV_COMPONENT_ETA:
			/* Estimated time remaining until completion - if size is known. */
			eta =
			    pv__seconds_remaining(((off_t) total_bytes - state->display.initial_offset),
						  state->control.size - state->display.initial_offset,
						  state->display.current_avg_rate);

			/*
			 * Bounds check, so we don't overrun the suffix buffer. This
			 * means the ETA will always be less than 100,000 hours.
			 */
			eta = bound_long(eta, 0, (long) 360000000L);

			/*
			 * If the ETA is more than a day, include a day count as
			 * well as hours, minutes, and seconds.
			 */
			/*@-mustfreefresh @ */
			if (eta > 86400L) {
				(void) pv_snprintf(component_content,
						   component_buf_size,
						   "%.16s %ld:%02ld:%02ld:%02ld",
						   _("ETA"), eta / 86400, (eta / 3600) % 24, (eta / 60) % 60, eta % 60);
			} else {
				(void) pv_snprintf(component_content,
						   component_buf_size,
						   "%.16s %ld:%02ld:%02ld", _("ETA"), eta / 3600, (eta / 60) % 60,
						   eta % 60);
			}
			/*@+mustfreefresh @ *//* splint: see above. */

			/*
			 * If this is the final update, show a blank space where the
			 * ETA used to be.
			 */
			if (bytes_since_last < 0) {
				size_t erase_idx;
				for (erase_idx = 0;
				     erase_idx < component_buf_size && component_content[erase_idx] != '\0';
				     erase_idx++) {
					component_content[erase_idx] = ' ';
				}
			}
			break;

		case PV_COMPONENT_FINETA:
			/* Estimated time of completion - if size is known. */

			now = time(NULL);
			show_eta = true;
			time_format = NULL;

			/*
			 * The ETA may be hidden by a failed ETA string
			 * generation.
			 */

			eta =
			    pv__seconds_remaining((off_t) (total_bytes - state->display.initial_offset),
						  state->control.size - state->display.initial_offset,
						  state->display.current_avg_rate);

			/*
			 * Bounds check, so we don't overrun the suffix buffer. This
			 * means the ETA will always be less than 100,000 hours.
			 */
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
				show_eta = false;
			} else {
				/* Localtime keeps data stored in a static
				 * buffer that gets overwritten by time
				 * functions.
				 */
				struct tm time = *time_ptr;
				size_t component_content_length;

				/*@-mustfreefresh @ */
				(void) pv_snprintf(component_content, component_buf_size, "%.16s ", _("ETA"));
				/*@+mustfreefresh @ *//* splint: see above. */
				component_content_length = strlen(component_content);	/* flawfinder: ignore */
				/* flawfinder: always bounded with \0 by pv_snprintf(). */
				(void) strftime(component_content + component_content_length,
						component_buf_size - 1 - component_content_length, time_format, &time);
			}

			if (!show_eta) {
				size_t erase_idx;
				for (erase_idx = 0;
				     erase_idx < component_buf_size && component_content[erase_idx] != '\0';
				     erase_idx++) {
					component_content[erase_idx] = ' ';
				}
			}
			break;

		case PV_COMPONENT_NAME:
			/* Name prefix. */
			if (state->control.name) {
				(void) pv_snprintf(component_content, component_buf_size, "%9.500s:",
						   state->control.name);
			}
			break;

		case PV_COMPONENT_BUFPERCENT:
			/* Transfer buffer percentage utilisation. */
			if (state->transfer.buffer_size > 0) {
				int pct_used = pv__calc_percentage((off_t)
								   (state->transfer.read_position -
								    state->transfer.write_position),
								   (off_t)
								   (state->transfer.buffer_size));
				(void) pv_snprintf(component_content, component_buf_size, "{%3d%%}", pct_used);
			}
#ifdef HAVE_SPLICE
			if (state->transfer.splice_used)
				(void) pv_snprintf(component_content, component_buf_size, "{%s}", "----");
#endif
			break;

		case PV_COMPONENT_OUTPUTBUF:
			/* Recently transferred bytes. */
			for (buf_idx = 0; buf_idx < state->display.lastoutput_length; buf_idx++) {
				int display_char;
				display_char = (int) (state->display.lastoutput_buffer[buf_idx]);
				component_content[buf_idx] = isprint(display_char) ? (char) display_char : '.';
			}
			component_content[buf_idx] = '\0';
			break;

		default:
			break;
		}

		/* Record the string length for this component. */
		state->display.component[component_type].length = strlen(component_content);	/* flawfinder: ignore */
		/* flawfinder: always bounded by \0 either explicitly or by pv_snprintf(). */
	}


	/*
	 * Now go through all the static portions of the format to work
	 * out how much space will be left for any dynamic portions
	 * (i.e. the progress bar).
	 */
	static_portion_size = 0;
	for (segment = 0; segment < state->display.format_segment_count; segment++) {
		if (state->display.format[segment].type == PV_COMPONENT_STRING) {
			static_portion_size += state->display.format[segment].str_length;
		} else if (state->display.format[segment].type != PV_COMPONENT_PROGRESS) {
			static_portion_size += state->display.component[state->display.format[segment].type].length;
		}
	}

	debug("static_portion_size: %d", static_portion_size);

	/*
	 * Assemble the progress bar now we know how big it should be.
	 */
	if (state->display.component[PV_COMPONENT_PROGRESS].required) {
		char *component_content;
		size_t component_buf_size;
		char pct[16];		 /* flawfinder: ignore - only populated by pv_snprintf(). */
		int available_width, bar_length, pad_count;

		component_content = state->display.component[PV_COMPONENT_PROGRESS].content;
		component_content[0] = '\0';
		component_buf_size = PV_SIZEOF_COMPONENT_STR;

		memset(pct, 0, sizeof(pct));

		/* The opening of the bar area. */
		(void) pv_snprintf(component_content, component_buf_size, "[");

		if (state->control.size > 0) {
			/* Known size; show a bar and a percentage. */
			size_t pct_width;

			if (state->display.percentage < 0)
				state->display.percentage = 0;
			if (state->display.percentage > 100000)
				state->display.percentage = 100000;
			(void) pv_snprintf(pct, sizeof(pct), "%3ld%%", state->display.percentage);
			pct_width = strlen(pct);	/* flawfinder: ignore */
			/* flawfinder: always \0-terminated by pv_snprintf() and the earlier memset(). */

			available_width = (int) (state->control.width) - static_portion_size - (int) (pct_width) - 3;

			if (available_width < 0)
				available_width = 0;

			if (available_width > (int) (component_buf_size) - 16)
				available_width = (int) (component_buf_size - 16);

			/* The bar portion. */
			bar_length = (int) ((available_width * state->display.percentage) / 100 - 1);
			for (pad_count = 0; pad_count < bar_length; pad_count++) {
				if (pad_count < available_width)
					(void) pv_strlcat(component_content, "=", component_buf_size);
			}

			/* The tip of the bar, if not at 100%. */
			if (pad_count < available_width) {
				(void) pv_strlcat(component_content, ">", component_buf_size);
				pad_count++;
			}

			/* The spaces after the bar. */
			for (; pad_count < available_width; pad_count++) {
				(void) pv_strlcat(component_content, " ", component_buf_size);
			}

			/* The closure of the bar area, and the percentage. */
			(void) pv_strlcat(component_content, "] ", component_buf_size);
			(void) pv_strlcat(component_content, pct, component_buf_size);

		} else {
			/* Unknown size; show a moving indicator. */

			int indicator_position = state->display.percentage;

			available_width = (int) (state->control.width) - static_portion_size - 5;

			if (available_width < 0)
				available_width = 0;

			if (available_width > (int) (component_buf_size) - 16)
				available_width = (int) (component_buf_size) - 16;

			debug("available_width: %d", available_width);

			/*
			 * Earlier code in this function sets the percentage
			 * when the size is unknown to a value that goes 0 -
			 * 200 and resets, so here we make values above 100
			 * send the indicator back down again, so it moves
			 * back and forth.
			 */
			if (indicator_position > 100)
				indicator_position = 200 - indicator_position;

			/* The spaces before the indicator. */
			for (pad_count = 0; pad_count < (available_width * indicator_position) / 100; pad_count++) {
				if (pad_count < available_width)
					(void) pv_strlcat(component_content, " ", component_buf_size);
			}

			/* The indicator. */
			(void) pv_strlcat(component_content, "<=>", component_buf_size);

			/* The spaces after the indicator. */
			for (; pad_count < available_width; pad_count++) {
				(void) pv_strlcat(component_content, " ", component_buf_size);
			}

			/* The closure of the bar area. */
			(void) pv_strlcat(component_content, "]", component_buf_size);
		}

		/* Record the string length for this component. */
		state->display.component[PV_COMPONENT_PROGRESS].length = strlen(component_content);	/* flawfinder: ignore */
		/* flawfinder: always bounded with \0 by pv_strlcat(). */

		/*
		 * If the progress bar won't fit, drop it.
		 */
		if ((unsigned int) (state->display.component[PV_COMPONENT_PROGRESS].length + static_portion_size) >
		    state->control.width) {
			component_content[0] = '\0';
			state->display.component[PV_COMPONENT_PROGRESS].length = 0;
		}
	}

	/*
	 * We can now build the output string using the format structure.
	 */

	memset(state->display.display_buffer, 0, state->display.display_buffer_size);
	new_display_string_len = 0;
	for (segment = 0; segment < state->display.format_segment_count; segment++) {
		const char *segment_content;
		size_t segment_length;

		if (state->display.format[segment].type == PV_COMPONENT_STRING) {
			segment_content = &(formatstr[state->display.format[segment].str_start]);
			segment_length = state->display.format[segment].str_length;
		} else {
			segment_content = state->display.component[state->display.format[segment].type].content;
			segment_length = state->display.component[state->display.format[segment].type].length;
		}

		/* Skip empty segments. */
		if (segment_length == 0)
			continue;

		/*
		 * Truncate the segment if it would make the display string
		 * overflow the buffer.
		 */
		if (segment_length + new_display_string_len > state->display.display_buffer_size - 2)
			segment_length = state->display.display_buffer_size - new_display_string_len - 2;
		if (segment_length < 1)
			break;

		/* Skip the segment if it would make the display too wide. */
		if ((unsigned int) (segment_length + new_display_string_len) > state->control.width)
			break;

		/* Append the segment to the output string. */
		strncat(state->display.display_buffer, segment_content, segment_length);	/* flawfinder: ignore */
		/* flawfinder: length is checked above, and buffer is \0 terminated already. */

		new_display_string_len += segment_length;
	}

	debug("%s: %d", "display string length counted by format segments", (int) new_display_string_len);

	/* Recalculate display string length with strlen() in case of miscounting. */
	new_display_string_len = strlen(state->display.display_buffer);	/* flawfinder: ignore */
	/* flawfinder: \0 terminated by memset() above and then by segment bounds checking. */
	debug("%s: %d", "display string length from strlen()", (int) new_display_string_len);

	/*
	 * If the size of our output shrinks, we need to keep appending
	 * spaces at the end, so that we don't leave dangling bits behind.
	 */
	if ((new_display_string_len < state->display.display_string_len)
	    && (state->control.width >= state->display.prev_screen_width)) {
		char spaces[32];	 /* flawfinder: ignore - terminated, bounded */
		int spaces_to_add;

		spaces_to_add = (int) (state->display.display_string_len - new_display_string_len);
		/* Upper boundary on number of spaces */
		if (spaces_to_add > 15) {
			spaces_to_add = 15;
		}
		new_display_string_len += spaces_to_add;
		spaces[spaces_to_add] = '\0';
		while (--spaces_to_add >= 0) {
			spaces[spaces_to_add] = ' ';
		}
		(void) pv_strlcat(state->display.display_buffer, spaces, state->display.display_buffer_size);
	}

	state->display.display_string_len = new_display_string_len;
	state->display.prev_screen_width = state->control.width;

	return true;
}


/*
 * Output status information on standard error, where "esec" is the seconds
 * elapsed since the transfer started, "sl" is the number of bytes transferred
 * since the last update, and "tot" is the total number of bytes transferred
 * so far.
 *
 * If "sl" is negative, this is the final update so the rate is given as an
 * an average over the whole transfer; otherwise the current rate is shown.
 *
 * In line mode, "sl" and "tot" are in lines, not bytes.
 */
void pv_display(pvstate_t state, long double esec, off_t sl, off_t tot)
{
	if (NULL == state)
		return;

	/*
	 * If the display options need reparsing, do so to generate new
	 * formatting parameters.
	 */
	if (0 != state->flag.reparse_display) {
		pv__format_init(state);
		state->flag.reparse_display = 0;
	}

	pv_sig_checkbg();

	if (!pv__format(state, esec, sl, tot))
		return;

	if (NULL == state->display.display_buffer)
		return;

	if (state->control.numeric) {
		pv_write_retry(STDERR_FILENO, state->display.display_buffer, state->display.display_string_len);
	} else if (state->control.cursor) {
		if (state->control.force || pv_in_foreground()) {
			pv_crs_update(state, state->display.display_buffer);
			state->display.display_visible = true;
		}
	} else {
		if (state->control.force || pv_in_foreground()) {
			pv_write_retry(STDERR_FILENO, state->display.display_buffer, state->display.display_string_len);
			pv_write_retry(STDERR_FILENO, "\r", 1);
			state->display.display_visible = true;
		}
	}

	debug("%s: [%s]", "display", state->display.display_buffer);
}

/* EOF */
