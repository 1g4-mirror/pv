/*
 * Formatter function for transfer buffer percentage utilisation.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Percentage transfer buffer utilisation.
 */
size_t pv_formatter_buffer_percent(pvstate_t state, /*@unused@ */  __attribute__((unused)) pvdisplay_t display,
					pvdisplay_segment_t segment, char *buffer, size_t buffer_size, size_t offset)
{
	char content[16];		 /* flawfinder: ignore - always bounded */

	content[0] = '\0';

	if (0 == buffer_size)
		return 0;

	if (state->transfer.buffer_size > 0) {
		int pct_used = pv_percentage((off_t)
					     (state->transfer.read_position - state->transfer.write_position),
					     (off_t)
					     (state->transfer.buffer_size));
		(void) pv_snprintf(content, sizeof(content), "{%3d%%}", pct_used);
	}
#ifdef HAVE_SPLICE
	if (state->transfer.splice_used)
		(void) pv_snprintf(content, sizeof(content), "{%s}", "----");
#endif

	return pv_formatter_segmentcontent(content, segment, buffer, buffer_size, offset);
}
