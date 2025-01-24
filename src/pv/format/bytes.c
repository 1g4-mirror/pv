/*
 * Formatter function for bytes or lines transferred.
 *
 * Copyright 2024-2025 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Number of bytes or lines transferred.
 */
pvdisplay_bytecount_t pv_formatter_bytes(pvformatter_args_t args)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	args->display->showing_bytes = true;

	if (0 == args->buffer_size)
		return 0;

	content[0] = '\0';

	/*@-mustfreefresh@ */
	if (args->state->control.numeric) {
		/* Numeric mode - raw values only, no suffix. */
		(void) pv_snprintf(content, sizeof(content),
				   "%lld",
				   (long long) ((args->state->control.bits ? 8 : 1) * args->state->transfer.transferred));
	} else if (args->state->control.bits && !args->state->control.linemode) {
		pv_describe_amount(content, sizeof(content), "%s",
				   (long double) (args->state->transfer.transferred * 8), "", _("b"),
				   args->display->count_type);
	} else {
		pv_describe_amount(content, sizeof(content), "%s",
				   (long double) (args->state->transfer.transferred), "", _("B"),
				   args->display->count_type);
	}
	/*@+mustfreefresh@ *//* splint - false positive from gettext(). */

	return pv_formatter_segmentcontent(content, args);
}
