/*
 * Formatter function for bytes or lines transferred.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Number of bytes or lines transferred.
 */
size_t pv_formatter_bytes(pvstate_t state, pvdisplay_t display, pvdisplay_segment_t segment, char *buffer,
			       size_t buffer_size, size_t offset)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	display->showing_bytes = true;

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	/*@-mustfreefresh@ */
	if (state->control.bits && !state->control.linemode) {
		pv_describe_amount(content, sizeof(content), "%s",
			    (long double) (state->transfer.transferred * 8), "", _("b"), display->count_type);
	} else {
		pv_describe_amount(content, sizeof(content), "%s",
			    (long double) (state->transfer.transferred), "", _("B"), display->count_type);
	}
	/*@+mustfreefresh@ *//* splint - false positive from gettext(). */

	return pv_formatter_segmentcontent(content, segment, buffer, buffer_size, offset);
}
