/*
 * Formatter function for the current rate of transfer.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Transfer rate.
 */
size_t pv_formatter_rate(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
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
		pv_describe_amount(content, sizeof(content), "[%s]",
			    8 * state->calc.transfer_rate, "", _("b/s"), display->count_type);
	} else {
		/* bytes or lines per second */
		pv_describe_amount(content, sizeof(content),
			    "[%s]", state->calc.transfer_rate, _("/s"), _("B/s"), display->count_type);
	}
	/*@+mustfreefresh@ *//* splint: see above. */

	return pv_formatter_segmentcontent(content, segment, buffer, buffer_size, offset);
}
