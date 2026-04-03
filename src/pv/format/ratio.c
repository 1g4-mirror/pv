/*
 * Formatter function for ratio of bytes transferred by the other monitored
 * side to bytes transferred by this side.
 *
 * Copyright 2026 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Ratio of bytes or lines transferred by the other monitored side ("-M
 * both") to the bytes or lines transferred by this side.
 */
pvdisplay_bytecount_t pv_formatter_ratio(pvformatter_args_t args)
{
	char content[128];		 /* flawfinder: ignore - always bounded */

	args->display->showing_ratio = true;

	if (0 == args->buffer_size)
		return 0;

	content[0] = '\0';

	if (0 == args->transfer->otherside_transferred || 0 == args->transfer->transferred) {
		(void) pv_snprintf(content, sizeof(content), "%s:%s", "-", "-");
	} else if (args->transfer->otherside_transferred == args->transfer->transferred) {
		(void) pv_snprintf(content, sizeof(content), "%d:%d", 1, 1);
	} else {
		long double side1, side2, ratio;

		side1 = (long double) (args->transfer->otherside_transferred);
		side2 = (long double) (args->transfer->transferred);

		if (side1 > side2) {
			ratio = side1 / side2;
			(void) pv_snprintf(content, sizeof(content), "%.4Lg:%d", ratio, 1);
		} else {
			ratio = side2 / side1;
			(void) pv_snprintf(content, sizeof(content), "%d:%.4Lg", 1, ratio);
		}
	}

	return pv_formatter_segmentcontent(content, args);
}
