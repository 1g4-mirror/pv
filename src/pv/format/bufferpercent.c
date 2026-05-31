/*
 * Formatter function for transfer buffer percentage utilisation.
 *
 * Copyright 2024-2026 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Percentage transfer buffer utilisation.
 */
pvdisplay_bytecount_t pv_formatter_buffer_percent(pvformatter_args_t args)
{
	char content[16];		 /* flawfinder: ignore - always bounded */

	content[0] = '\0';

	if (0 == args->buffer_size)
		return 0;

	if (args->transfer->buffer_size > 0) {
		double pct_used = pv_percentage((off_t)
						(args->transfer->read_position - args->transfer->write_position),
						(off_t)
						(args->transfer->buffer_size));
		(void) pv_snprintf(content, sizeof(content), "{%3.0f%%}", pct_used);
	}
	switch (args->transfer->method) {
	case PV_TRANSFERMETHOD_READWRITE:
		break;
	case PV_TRANSFERMETHOD_SPLICE:
		(void) pv_snprintf(content, sizeof(content), "{%s}", "----");
		break;
	case PV_TRANSFERMETHOD_SPLICE_INTERMEDIATE:
		(void) pv_snprintf(content, sizeof(content), "{%s}", "-||-");
		break;
	case PV_TRANSFERMETHOD_COPY_FILE_RANGE:
		(void) pv_snprintf(content, sizeof(content), "{%s}", "--->");
		break;
	}

	return pv_formatter_segmentcontent(content, args);
}
