/*
 * Formatter function for showing the name of the transfer.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <string.h>


/*
 * Display the transfer's name.
 */
size_t pv_formatter_name(pvstate_t state, /*@unused@ */  __attribute__((unused)) pvdisplay_t display,
			      pvdisplay_segment_t segment, char *buffer, size_t buffer_size, size_t offset)
{
	char string_format[32];		 /* flawfinder: ignore - always bounded */
	char content[512];		 /* flawfinder: ignore - always bounded */
	size_t field_width;

	if (0 == buffer_size)
		return 0;

	field_width = segment->chosen_size;
	if (field_width < 1)
		field_width = 9;
	if (field_width > 500)
		field_width = 500;

	memset(string_format, 0, sizeof(string_format));
	(void) pv_snprintf(string_format, sizeof(string_format), "%%%d.500s:", field_width);

	content[0] = '\0';
	if (state->control.name) {
		(void) pv_snprintf(content, sizeof(content), string_format, state->control.name);
	}

	return pv_formatter_segmentcontent(content, segment, buffer, buffer_size, offset);
}
