/*
 * Formatter function for ETA display.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Estimated time until completion.
 */
size_t pv_formatter_eta(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
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
	    pv_seconds_remaining((state->transfer.transferred - display->initial_offset),
				  state->control.size - display->initial_offset, state->calc.current_avg_rate);

	/*
	 * Bounds check, so we don't overrun the suffix buffer.  This means
	 * the ETA will always be less than 100,000 hours.
	 */
	eta = pv_bound_long(eta, 0, (long) 360000000L);

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

	return pv_formatter_segmentcontent(content, segment, buffer, buffer_size, offset);
}
