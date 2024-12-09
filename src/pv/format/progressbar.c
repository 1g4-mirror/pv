/*
 * Formatter functions for progress bars.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <string.h>


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
 * This is only called by pv_formatter_progress().
 */
static size_t pv_formatter_progress_knownsize(pvstate_t state, pvdisplay_t display, char *buffer, size_t buffer_size,
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
			pv_describe_amount(after_bar, sizeof(after_bar), "/%s",
				    8.0 * state->calc.rate_max, "", _("b/s"), display->count_type);
		} else {
			/* bytes or lines per second */
			pv_describe_amount(after_bar, sizeof(after_bar),
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
 * This is only called by pv_formatter_progress().
 */
static size_t pv_formatter_progress_unknownsize(pvstate_t state,	/*@unused@ */
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
size_t pv_formatter_progress(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
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
		    pv_formatter_progress_knownsize(state, display, content, sizeof(content), segment->width, true, true,
						  true);
	} else {
		/* Unknown size - back-and-forth moving indicator. */
		bytes = pv_formatter_progress_unknownsize(state, display, content, sizeof(content), segment->width, true);
	}

	content[bytes] = '\0';

	return pv_formatter_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * Progress bar, without sides and without a number afterwards.
 */
size_t pv_formatter_progress_bar_only(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment,
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
		    pv_formatter_progress_knownsize(state, display, content, sizeof(content), segment->width, false, true,
						  false);
	} else {
		/* Unknown size - back-and-forth moving indicator. */
		bytes =
		    pv_formatter_progress_unknownsize(state, display, content, sizeof(content), segment->width, false);
	}

	content[bytes] = '\0';

	return pv_formatter_segmentcontent(content, segment, buffer, buffer_size, offset);
}


/*
 * The number after the progress bar.
 */
size_t pv_formatter_progress_amount_only(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment,
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
		    pv_formatter_progress_knownsize(state, display, content, sizeof(content), segment->width, false,
						  false, true);
	} else {
		/* Unknown size - no number. */
		return 0;
	}

	content[bytes] = '\0';

	return pv_formatter_segmentcontent(content, segment, buffer, buffer_size, offset);
}
