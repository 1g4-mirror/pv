/*
 * Formatter function for the elapsed transfer time.
 *
 * Copyright 2024-2025 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Elapsed time.
 *
 * TODO: show as a number if --numeric is active.
 */
pvdisplay_bytecount_t pv_formatter_timer(pvformatter_args_t args)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	args->display->showing_timer = true;

	content[0] = '\0';

	if (0 == args->buffer_size)
		return 0;

	/*
	 * Bounds check, so we don't overrun the prefix buffer.  This does
	 * mean that the timer will stop at a 100,000 hours, but since
	 * that's 11 years, it shouldn't be a problem.
	 */
	if (args->state->transfer.elapsed_seconds > (long double) 360000000.0L)
		args->state->transfer.elapsed_seconds = (long double) 360000000.0L;

	/* Also check it's not negative. */
	if (args->state->transfer.elapsed_seconds < 0.0)
		args->state->transfer.elapsed_seconds = 0.0;

	/*
	 * If the elapsed time is more than a day, include a day count as
	 * well as hours, minutes, and seconds.
	 */
	if (args->state->transfer.elapsed_seconds > (long double) 86400.0L) {
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%ld:%02ld:%02ld:%02ld",
				   ((long) (args->state->transfer.elapsed_seconds)) / 86400,
				   (((long) (args->state->transfer.elapsed_seconds)) / 3600) %
				   24, (((long) (args->state->transfer.elapsed_seconds)) / 60) % 60,
				   ((long) (args->state->transfer.elapsed_seconds)) % 60);
	} else {
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%ld:%02ld:%02ld",
				   ((long) (args->state->transfer.elapsed_seconds)) / 3600,
				   (((long) (args->state->transfer.elapsed_seconds)) / 60) % 60,
				   ((long) (args->state->transfer.elapsed_seconds)) % 60);
	}

	return pv_formatter_segmentcontent(content, args);
}
