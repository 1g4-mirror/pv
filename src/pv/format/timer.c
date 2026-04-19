/*
 * Formatter function for the elapsed transfer time.
 *
 * Copyright 2024-2026 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Elapsed time.
 */
pvdisplay_bytecount_t pv_formatter_timer(pvformatter_args_t args)
{
	char content[128];		 /* flawfinder: ignore - bounded with pv_snprintf(). */
	long double elapsed_seconds;

	args->display->showing_timer = true;

	content[0] = '\0';

	if (0 == args->buffer_size)
		return 0;

	elapsed_seconds = args->transfer->elapsed_seconds;

	/* The timer must always be positive. */
	if (elapsed_seconds < 0.0L)
		elapsed_seconds = 0.0L;

	if (args->control->numeric) {
		/* Numeric mode - show the number of seconds, unformatted. */
		(void) pv_snprintf(content, sizeof(content), "%.4Lf", elapsed_seconds);
	} else if (elapsed_seconds > (long double) (999999.999999L * 86400.0L)) {
		/*
		 * At a million days, the timer would be too wide, so avoid
		 * an overflow.
		 */
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%s:%02ld:%02ld:%02ld",
				   ">=1e6",
				   (((long) (elapsed_seconds)) / 3600) %
				   24, (((long) (elapsed_seconds)) / 60) % 60, ((long) (elapsed_seconds)) % 60);
	} else if (elapsed_seconds > (long double) 86400.0L) {
		/*
		 * If the elapsed time is more than a day, include a day count as
		 * well as hours, minutes, and seconds.
		 */
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%ld:%02ld:%02ld:%02ld",
				   ((long) (elapsed_seconds)) / 86400,
				   (((long) (elapsed_seconds)) / 3600) %
				   24, (((long) (elapsed_seconds)) / 60) % 60, ((long) (elapsed_seconds)) % 60);
	} else {
		(void) pv_snprintf(content,
				   sizeof(content),
				   "%ld:%02ld:%02ld",
				   ((long) (elapsed_seconds)) / 3600,
				   (((long) (elapsed_seconds)) / 60) % 60, ((long) (elapsed_seconds)) % 60);
	}

	return pv_formatter_segmentcontent(content, args);
}
