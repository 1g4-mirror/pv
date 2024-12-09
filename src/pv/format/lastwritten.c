/*
 * Formatter function for showing the last bytes written.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Display the last few bytes written.
 *
 * As a side effect, this sets display->lastwritten_bytes to the segment's
 * chosen_size, if it was previously smaller than that.
 */
size_t pv_formatter_last_written(	 /*@unused@ */
					     __attribute__((unused)) pvstate_t state, pvdisplay_t display,
					     pvdisplay_segment_t segment, char *buffer, size_t buffer_size,
					     size_t offset)
{
	size_t bytes_to_show, read_offset, remaining;

	display->showing_last_written = true;

	bytes_to_show = segment->chosen_size;
	if (0 == bytes_to_show)
		bytes_to_show = segment->width;
	if (0 == bytes_to_show)
		return 0;

	if (bytes_to_show > PV_SIZEOF_LASTWRITTEN_BUFFER)
		bytes_to_show = PV_SIZEOF_LASTWRITTEN_BUFFER;
	if (bytes_to_show > display->lastwritten_bytes)
		display->lastwritten_bytes = bytes_to_show;

	if (0 == buffer_size)
		return 0;

	if (offset + bytes_to_show >= buffer_size)
		return 0;

	segment->offset = offset;
	segment->bytes = bytes_to_show;

	read_offset = display->lastwritten_bytes - bytes_to_show;
	for (remaining = bytes_to_show; remaining > 0; remaining--) {
		char display_char = display->lastwritten_buffer[read_offset++];
		buffer[offset++] = pv_isprint(display_char) ? display_char : '.';
	}

	return bytes_to_show;
}
