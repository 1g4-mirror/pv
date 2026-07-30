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
 * Produce OSC 9;4 progress bar codes for terminal title tabs.
 */
pvdisplay_bytecount_t pv_formatter_progress_conemu(pvformatter_args_t args)
{
	char content[128];		 /* flawfinder: ignore */

	/* flawfinder - null-terminated and bounded with pv_snprintf(). */

	memset(content, 0, sizeof(content));

	if (args->control->size > 0 || args->control->rate_gauge) {
		/* Known size or rate gauge - percentage progress. */
		(void) pv_snprintf(content, sizeof(content), "\033]9;4;1;%.0f\033\\", args->calc->percentage);
	} else {
		/* Unknown size - indeterminate progress. */
		/* See pv_formatter_progress_unknownsize() in progressbar.c. */
		double indicator_position;

		indicator_position = args->calc->percentage;
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

		(void) pv_snprintf(content, sizeof(content), "\033]9;4;3;%.0f\033\\", indicator_position);
	}

	args->display->using_osc94 = true;

	return pv_formatter_segmentcontent(content, args);
}
