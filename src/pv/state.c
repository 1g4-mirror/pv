/*
 * State management functions.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>


/* alloc / realloc history buffer */
static void pv_alloc_history(pvstate_t state)
{
	if (NULL != state->display.history)
		free(state->display.history);
	state->display.history = NULL;

	state->display.history = calloc((size_t) (state->display.history_len), sizeof(state->display.history[0]));
	if (NULL == state->display.history) {
		/*@-mustfreefresh@ */
		/*
		 * splint note: the gettext calls made by _() cause memory
		 * leak warnings, but in this case it's unavoidable, and
		 * mitigated by the fact we only translate each string once.
		 */
		fprintf(stderr, "%s: %s: %s\n", state->status.program_name,
			_("history structure allocation failed"), strerror(errno));
		/*@+mustfreefresh@ */
		return;
	}

	state->display.history_first = state->display.history_last = 0;
	state->display.history[0].elapsed_sec = 0.0;	/* to be safe, memset() not recommended for doubles */
}

/*
 * Create a new state structure, and return it, or 0 (NULL) on error.
 */
pvstate_t pv_state_alloc(const char *program_name)
{
	pvstate_t state;

	state = calloc(1, sizeof(*state));
	if (NULL == state)
		return NULL;
	memset(state, 0, sizeof(*state));

	/* splint 3.1.2 thinks this is required for some reason. */
	if (NULL != state->status.program_name) {
		free(state->status.program_name);
	}

	state->status.program_name = pv_strdup(program_name);
	if (NULL == state->status.program_name) {
		free(state);
		return NULL;
	}

	state->control.watch_pid = 0;
	state->control.watch_fd = -1;
#ifdef HAVE_IPC
	state->cursor.shmid = -1;
	state->cursor.pvcount = 1;
#endif				/* HAVE_IPC */
	state->cursor.lock_fd = -1;

	state->flag.reparse_display = 1;
	state->status.current_input_file = -1;
#ifdef HAVE_SPLICE
	state->transfer.splice_failed_fd = -1;
#endif				/* HAVE_SPLICE */
	state->display.display_visible = false;

	/*
	 * Get the current working directory, if possible, as a base for
	 * showing relative filenames with --watchfd.
	 */
	if (NULL == getcwd(state->status.cwd, PV_SIZEOF_CWD - 1)) {
		/* failed - will always show full path */
		state->status.cwd[0] = '\0';
	}
	if ('\0' == state->status.cwd[1]) {
		/* CWD is root directory - always show full path */
		state->status.cwd[0] = '\0';
	}
	state->status.cwd[PV_SIZEOF_CWD - 1] = '\0';

	return state;
}


/*
 * Free a state structure, after which it can no longer be used.
 */
void pv_state_free(pvstate_t state)
{
	if (0 == state)
		return;

	if (NULL != state->status.program_name)
		free(state->status.program_name);
	state->status.program_name = NULL;

	if (NULL != state->display.display_buffer)
		free(state->display.display_buffer);
	state->display.display_buffer = NULL;

	if (NULL != state->control.name) {
		free(state->control.name);
		state->control.name = NULL;
	}

	if (NULL != state->control.format_string) {
		free(state->control.format_string);
		state->control.format_string = NULL;
	}

	/*@-keeptrans@ */
	if (NULL != state->transfer.transfer_buffer)
		free(state->transfer.transfer_buffer);
	state->transfer.transfer_buffer = NULL;
	/*@+keeptrans@ */
	/* splint - explicitly freeing this structure, so free() here is OK. */

	if (NULL != state->display.history)
		free(state->display.history);
	state->display.history = NULL;

	if (NULL != state->files.filename) {
		unsigned int file_idx;
		for (file_idx = 0; file_idx < state->files.file_count; file_idx++) {
			/*@-unqualifiedtrans@ */
			free(state->files.filename[file_idx]);
			/*@+unqualifiedtrans@ */
			/* splint: see similar code below. */
		}
		free(state->files.filename);
		state->files.filename = NULL;
	}

	free(state);

	return;
}


/*
 * Set the formatting string, given a set of old-style formatting options.
 */
void pv_state_set_format(pvstate_t state, bool progress, bool timer, bool eta, bool fineta, bool rate, bool average_rate, bool bytes, bool bufpercent, size_t lastwritten,	/*@null@ */
			 const char *name)
{
#define PV_ADDFORMAT(x,y) if (x) { \
		if (state->control.default_format[0] != '\0') \
			(void) pv_strlcat(state->control.default_format, " ", sizeof(state->control.default_format)); \
		(void) pv_strlcat(state->control.default_format, y, sizeof(state->control.default_format)); \
	}

	state->control.default_format[0] = '\0';
	PV_ADDFORMAT(name, "%N");
	PV_ADDFORMAT(bytes, "%b");
	PV_ADDFORMAT(bufpercent, "%T");
	PV_ADDFORMAT(timer, "%t");
	PV_ADDFORMAT(rate, "%r");
	PV_ADDFORMAT(average_rate, "%a");
	PV_ADDFORMAT(progress, "%p");
	PV_ADDFORMAT(eta, "%e");
	PV_ADDFORMAT(fineta, "%I");
	if (lastwritten > 0) {
		char buf[16];		 /* flawfinder: ignore */
		memset(buf, 0, sizeof(buf));
		(void) pv_snprintf(buf, sizeof(buf), "%%%uA", (unsigned int) lastwritten);
		PV_ADDFORMAT(lastwritten > 0, buf);
		/*
		 * flawfinder rationale: large enough for string, zeroed
		 * before use, only written to by pv_snprintf() with the
		 * right buffer length.
		 */
	}

	if (NULL != state->control.name) {
		free(state->control.name);
		state->control.name = NULL;
	}

	if (NULL != name)
		state->control.name = pv_strdup(name);

	state->flag.reparse_display = 1;
}


void pv_state_force_set(pvstate_t state, bool val)
{
	state->control.force = val;
}

void pv_state_cursor_set(pvstate_t state, bool val)
{
	state->control.cursor = val;
}

void pv_state_numeric_set(pvstate_t state, bool val)
{
	state->control.numeric = val;
}

void pv_state_wait_set(pvstate_t state, bool val)
{
	state->control.wait = val;
}

void pv_state_delay_start_set(pvstate_t state, double val)
{
	state->control.delay_start = val;
}

void pv_state_linemode_set(pvstate_t state, bool val)
{
	state->control.linemode = val;
}

void pv_state_bits_set(pvstate_t state, bool bits)
{
	state->control.bits = bits;
}

void pv_state_null_terminated_lines_set(pvstate_t state, bool val)
{
	state->control.null_terminated_lines = val;
}

void pv_state_no_display_set(pvstate_t state, bool val)
{
	state->control.no_display = val;
}

void pv_state_skip_errors_set(pvstate_t state, unsigned int val)
{
	state->control.skip_errors = val;
}

void pv_state_error_skip_block_set(pvstate_t state, off_t val)
{
	state->control.error_skip_block = val;
}

void pv_state_stop_at_size_set(pvstate_t state, bool val)
{
	state->control.stop_at_size = val;
}

void pv_state_sync_after_write_set(pvstate_t state, bool val)
{
	state->control.sync_after_write = val;
}

void pv_state_direct_io_set(pvstate_t state, bool val)
{
	state->control.direct_io = val;
	state->control.direct_io_changed = true;
}

void pv_state_discard_input_set(pvstate_t state, bool val)
{
	state->control.discard_input = val;
}

void pv_state_rate_limit_set(pvstate_t state, off_t val)
{
	state->control.rate_limit = val;
}

void pv_state_target_buffer_size_set(pvstate_t state, size_t val)
{
	state->control.target_buffer_size = val;
}

void pv_state_no_splice_set(pvstate_t state, bool val)
{
	state->control.no_splice = val;
}

void pv_state_size_set(pvstate_t state, off_t val)
{
	state->control.size = val;
}

void pv_state_interval_set(pvstate_t state, double val)
{
	state->control.interval = val;
}

void pv_state_width_set(pvstate_t state, unsigned int val, bool was_set_manually)
{
	state->control.width = val;
	state->control.width_set_manually = was_set_manually;
}

void pv_state_height_set(pvstate_t state, unsigned int val, bool was_set_manually)
{
	state->control.height = val;
	state->control.height_set_manually = was_set_manually;
}

void pv_state_name_set(pvstate_t state, /*@null@ */ const char *val)
{
	if (NULL != state->control.name) {
		free(state->control.name);
		state->control.name = NULL;
	}
	if (NULL != val)
		state->control.name = pv_strdup(val);
}

void pv_state_format_string_set(pvstate_t state, /*@null@ */ const char *val)
{
	if (NULL != state->control.format_string) {
		free(state->control.format_string);
		state->control.format_string = NULL;
	}
	if (NULL != val)
		state->control.format_string = pv_strdup(val);
}

void pv_state_watch_pid_set(pvstate_t state, pid_t val)
{
	state->control.watch_pid = val;
}

void pv_state_watch_fd_set(pvstate_t state, int val)
{
	state->control.watch_fd = val;
}

void pv_state_average_rate_window_set(pvstate_t state, unsigned int val)
{
	if (val < 1)
		val = 1;
	state->control.average_rate_window = val;
	if (val >= 20) {
		state->display.history_len = (size_t) (val / 5 + 1);
		state->display.history_interval = 5;
	} else {
		state->display.history_len = (size_t) (val + 1);
		state->display.history_interval = 1;
	}
	pv_alloc_history(state);
}


/*
 * Set the array of input files.
 */
void pv_state_inputfiles(pvstate_t state, unsigned int input_file_count, const char **input_files)
{
	unsigned int file_idx;

	if (NULL != state->files.filename) {
		for (file_idx = 0; file_idx < state->files.file_count; file_idx++) {
			/*@-unqualifiedtrans@ */
			free(state->files.filename[file_idx]);
			/*@+unqualifiedtrans@ */
			/*
			 * TODO: find a way to tell splint the array
			 * contents are "only" and "null" as well as the
			 * array itself.
			 */
		}
		free(state->files.filename);
		state->files.filename = NULL;
		state->files.file_count = 0;
	}
	state->files.filename = calloc((size_t) (input_file_count + 1), sizeof(char *));
	if (NULL == state->files.filename) {
		/*@-mustfreefresh@ *//* see similar _() issue above */
		fprintf(stderr, "%s: %s: %s\n", state->status.program_name, _("file list allocation failed"),
			strerror(errno));
		/*@+mustfreefresh@ */
		return;
	}
	for (file_idx = 0; file_idx < input_file_count; file_idx++) {
		/*@-nullstate@ */
		state->files.filename[file_idx] = pv_strdup(input_files[file_idx]);
		if (NULL == state->files.filename[file_idx]) {
			/*@-mustfreefresh@ *//* see similar _() issue above */
			fprintf(stderr, "%s: %s: %s\n", state->status.program_name,
				_("file list allocation failed"), strerror(errno));
			/*@+mustfreefresh@ */
			return;
		}
	}
	state->files.file_count = input_file_count;
}

/*@+nullstate@*/
/* splint: see unqualifiedtrans note by free() above. */

/* EOF */
