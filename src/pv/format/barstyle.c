/*
 * Formatter functions for styled progress bars.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <string.h>
#if defined(ENABLE_NLS) && defined(HAVE_WCHAR_H)
#include <wchar.h>
#if defined(HAVE_WCTYPE_H)
#include <wctype.h>
#endif
#endif


/*
 * Populate "style" with the named bar style, falling back to the default if
 * the name was not recognised.  Returns true if the named style was found.
 *
 * Note that strings are copied into the structure, rather than just
 * updating pointers, to maintain separation of concern between different
 * parts of the code - and because on 64-bit architectures, the sizes of the
 * data types and their associated alignment padding tend to outweigh the
 * memory savings from using pointers and shared strings.
 */
static bool pv_barstyle(pvformatter_args_t args, pvbarstyle_t style, const char *name)
{
#define populate_string(item, str, w) { \
  item.width = w; \
  item.bytes = strlen(str); /* flawfinder: ignore */ \
  if (item.bytes > 0 && item.bytes <= PV_BARSTYLE_SIZEOF_STRING) \
	  memcpy(item.string, str, item.bytes); /* flawfinder: ignore */ \
}
	/*
	 * flawfinder - strlen() on null-terminated static strings is OK,
	 * and with the memcpy(), we check the buffer is big enough.
	 */

	memset(style, 0, sizeof(*style));

	if (args->state->control.can_display_utf8 && 0 == strcmp(name, "block")) {

		style->style_id = 2;

		populate_string(style->indicator, "◀▶", 2);
		populate_string(style->tip, "", 0);

		populate_string(style->filler[0], " ", 1);
		populate_string(style->filler[1], "█", 1);

		style->filler_entries = 2;

		return true;

	} else if (args->state->control.can_display_utf8 && 0 == strcmp(name, "granular")) {

		style->style_id = 3;

		populate_string(style->indicator, "◀▶", 2);
		populate_string(style->tip, "", 0);

		populate_string(style->filler[0], " ", 1);
		populate_string(style->filler[1], "▏", 1);
		populate_string(style->filler[2], "▎", 1);
		populate_string(style->filler[3], "▍", 1);
		populate_string(style->filler[4], "▌", 1);
		populate_string(style->filler[5], "▋", 1);
		populate_string(style->filler[6], "▊", 1);
		populate_string(style->filler[7], "▉", 1);
		populate_string(style->filler[8], "█", 1);

		style->filler_entries = 9;

		return true;

	} else if (args->state->control.can_display_utf8 && 0 == strcmp(name, "shaded")) {

		style->style_id = 4;

		populate_string(style->indicator, "▒▓▒", 3);
		populate_string(style->tip, "", 0);

		populate_string(style->filler[0], "░", 1);
		populate_string(style->filler[1], "▒", 1);
		populate_string(style->filler[2], "▓", 1);
		populate_string(style->filler[3], "█", 1);

		style->filler_entries = 4;

		return true;
	}

	/* Default style. */

	style->style_id = 1;

	populate_string(style->indicator, "<=>", 3);
	populate_string(style->tip, ">", 1);

	populate_string(style->filler[0], " ", 1);
	populate_string(style->filler[1], "=", 1);

	style->filler_entries = 2;

	if (0 == strcmp(name, "default"))
		return true;

	return false;
}


/*
 * Return the index into args->display->barstyle for the style with the
 * given name, adding that style to the array if it's not there already and
 * there's room.
 *
 * If there is no room, returns zero, so the first style is re-used.
 */
int pv_display_barstyle_index(pvformatter_args_t args, const char *name)
{
	struct pvbarstyle_s style;
	int barstyle_index;
#ifdef ENABLE_DEBUGGING
	bool found;
#endif

	memset(&style, 0, sizeof(style));
#ifdef ENABLE_DEBUGGING
	found = pv_barstyle(args, &style, name);
	if (!found)
		debug("%s: %s", name, "bar style not found, using default");
#else
	(void) pv_barstyle(args, &style, name);
#endif

	for (barstyle_index = 0;
	     barstyle_index < PV_BARSTYLE_MAX && args->display->barstyle[barstyle_index].style_id > 0;
	     barstyle_index++) {
		if (args->display->barstyle[barstyle_index].style_id == style.style_id) {
			debug("%s: %s: %d", name, "found in bar style array", barstyle_index);
			return barstyle_index;
		}
	}

	if (barstyle_index >= PV_BARSTYLE_MAX) {
		debug("%s: %s", name, "no room to add another bar style - returning 0");
		return 0;
	}

	memcpy(&(args->display->barstyle[barstyle_index]), &style, sizeof(style));	/* flawfinder: ignore */
	/* flawfinder - the destination is an array element of the right size. */
	debug("%s: %s: %d", name, "added to bar style array", barstyle_index);
	return barstyle_index;
}


size_t pv_formatter_bar_default(pvformatter_args_t args)
{
	if (0 == args->segment->parameter)
		args->segment->parameter = 1 + pv_display_barstyle_index(args, "default");
	return pv_formatter_progress_bar_only(args);
}

size_t pv_formatter_bar_block(pvformatter_args_t args)
{
	if (0 == args->segment->parameter)
		args->segment->parameter = 1 + pv_display_barstyle_index(args, "block");
	return pv_formatter_progress_bar_only(args);
}

size_t pv_formatter_bar_granular(pvformatter_args_t args)
{
	if (0 == args->segment->parameter)
		args->segment->parameter = 1 + pv_display_barstyle_index(args, "granular");
	return pv_formatter_progress_bar_only(args);
}

size_t pv_formatter_bar_shaded(pvformatter_args_t args)
{
	if (0 == args->segment->parameter)
		args->segment->parameter = 1 + pv_display_barstyle_index(args, "shaded");
	return pv_formatter_progress_bar_only(args);
}
