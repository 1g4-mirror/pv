/*
 * Formatter function for average rate.
 *
 * Copyright 2024-2025 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Average transfer rate.
 *
 * TODO: show exact number without suffix if --numeric is active.
 */
pvdisplay_bytecount_t pv_formatter_average_rate(pvformatter_args_t args)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	content[0] = '\0';

	if (0 == args->buffer_size)
		return 0;

	/*@-mustfreefresh@ */
	if (args->state->control.bits && !args->state->control.linemode) {
		/* bits per second */
		pv_describe_amount(content, sizeof(content),
				   "(%s)", 8 * args->state->calc.average_rate, "", _("b/s"), args->display->count_type);
	} else {
		/* bytes or lines per second */
		pv_describe_amount(content,
				   sizeof(content), "(%s)", args->state->calc.average_rate, _("/s"), _("B/s"),
				   args->display->count_type);
	}
	/*@+mustfreefresh@ *//* splint: see above. */

	return pv_formatter_segmentcontent(content, args);
}
