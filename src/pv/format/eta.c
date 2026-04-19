/*
 * Formatter function for ETA display.
 *
 * Copyright 2024-2026 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <limits.h>


/*
 * Estimated time until completion.
 */
pvdisplay_bytecount_t pv_formatter_eta(pvformatter_args_t args)
{
	char content[128];		 /* flawfinder: ignore - bounded in pv_snprintf(). */
	long eta, max_days;

	content[0] = '\0';

	/*
	 * Don't try to calculate this if the size is not known.
	 */
	if (args->control->size < 1)
		return 0;

	if (0 == args->buffer_size)
		return 0;

	eta =
	    pv_seconds_remaining((args->transfer->transferred - args->display->initial_offset),
				 args->control->size - args->display->initial_offset, args->calc->current_avg_rate);

	/* The ETA must always be positive. */
	if (eta < 0)
		eta = 0;

	/* The ETA must be under 1,000,000 days so it's not too wide. */
	/* If a "long" is 32 bits, only 24,855 days will fit in it. */
	max_days = 24855L;
#ifdef LONG_MAX
	if (LONG_MAX > 2147483647L)
		max_days = 1000000;
#endif

	if ((eta / 86400L) >= max_days)
		eta = max_days * 86400L - 1L;

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
	/*@+mustfreefresh@ *//* splint: false positives from gettext(). */

	/*
	 * If this is the final update, show a blank space where the ETA
	 * used to be.
	 */
	if (args->display->final_update) {
		size_t erase_idx;
		for (erase_idx = 0; erase_idx < sizeof(content) && content[erase_idx] != '\0'; erase_idx++) {
			content[erase_idx] = ' ';
		}
	}

	return pv_formatter_segmentcontent(content, args);
}
