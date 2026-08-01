/*
 * Formatter function for OSC 9;4 - ConEmu progress bar - codes.
 *
 * Copyright 2024-2026 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <string.h>

#if HAVE_MATH_H
#include <math.h>
#endif


/*
 * Populate a buffer with OSC 9;4 progress bar codes for terminal title
 * tabs, returning the length of the string.
 */
size_t pv_osc94_format(char *buffer, size_t bufsize, readonly_pvcontrol_t control, readonly_pvtransfercalc_t calc)
{
	int formatted_size = 0;

	if (NULL == buffer)
		return 0;
	if (bufsize < 1)
		return 0;
	buffer[0] = '\0';

	if (control->size > 0 || control->rate_gauge) {
		/* Known size or rate gauge - percentage progress. */
		formatted_size = pv_snprintf(buffer, bufsize, "\033]9;4;1;%.0f\033\\", calc->percentage);
	} else {
		/* Unknown size - indeterminate progress. */
		/* See pv_formatter_progress_unknownsize() in progressbar.c. */
		double indicator_position;

		indicator_position = calc->percentage;
		if (indicator_position > 200.0)
#if HAVE_FMOD
			indicator_position = fmod(indicator_position, 200.0);
#else
		{
			while (indicator_position > 200.0)
				indicator_position -= 200.0;
		}
#endif
		if (indicator_position > 100.0) {
			indicator_position = 200.0 - indicator_position;
		}
		if (indicator_position < 0.0) {
			indicator_position = 0.0;
		}

		formatted_size = pv_snprintf(buffer, bufsize, "\033]9;4;3;%.0f\033\\", indicator_position);
	}

	if (formatted_size < 0)
		return 0;
	if (formatted_size >= (int) bufsize)
		return bufsize - 1;
	return (size_t) formatted_size;
}


/*
 * Produce OSC 9;4 progress bar codes for terminal title tabs.
 */
pvdisplay_bytecount_t pv_formatter_progress_conemu(pvformatter_args_t args)
{
	char content[128];		 /* flawfinder: ignore */

	/* flawfinder - null-terminated and bounded with pv_snprintf(). */

	memset(content, 0, sizeof(content));

	(void) pv_osc94_format(content, sizeof(content), args->control, args->calc);

	args->display->using_osc94 = true;

	return pv_formatter_segmentcontent(content, args);
}
