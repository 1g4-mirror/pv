/*
 * State management functions.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * Distributed under the Artistic License v2.0; see `docs/COPYING'.
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
	if (NULL != state->history)
		free(state->history);
	state->history = NULL;

	state->history = calloc((size_t) (state->history_len), sizeof(state->history[0]));
	if (NULL == state->history) {
		/*@-mustfreefresh@ */
		/*
		 * splint note: the gettext calls made by _() cause memory
		 * leak warnings, but in this case it's unavoidable, and
		 * mitigated by the fact we only translate each string once.
		 */
		fprintf(stderr, "%s: %s: %s\n", state->program_name,
			_("history structure allocation failed"), strerror(errno));
		/*@+mustfreefresh@ */
		return;
	}

	state->history_first = state->history_last = 0;
	state->history[0].elapsed_sec = 0.0;	/* to be safe, memset() not recommended for doubles */
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
	if (NULL != state->program_name) {
		free(state->program_name);
	}

	state->program_name = pv_strdup(program_name);
	if (NULL == state->program_name) {
		free(state);
		return NULL;
	}

	state->watch_pid = 0;
	state->watch_fd = -1;
#ifdef HAVE_IPC
	state->crs_shmid = -1;
	state->crs_pvcount = 1;
#endif				/* HAVE_IPC */
	state->crs_lock_fd = -1;

	state->reparse_display = 1;
	state->current_input_file = -1;
#ifdef HAVE_SPLICE
	state->splice_failed_fd = -1;
#endif				/* HAVE_SPLICE */
	state->display_visible = false;

	/*
	 * Get the current working directory, if possible, as a base for
	 * showing relative filenames with --watchfd.
	 */
	if (NULL == getcwd(state->cwd, PV_SIZEOF_CWD - 1)) {
		/* failed - will always show full path */
		state->cwd[0] = '\0';
	}
	if ('\0' == state->cwd[1]) {
		/* CWD is root directory - always show full path */
		state->cwd[0] = '\0';
	}
	state->cwd[PV_SIZEOF_CWD - 1] = '\0';

	return state;
}


/*
 * Free a state structure, after which it can no longer be used.
 */
void pv_state_free(pvstate_t state)
{
	if (0 == state)
		return;

	if (NULL != state->program_name)
		free(state->program_name);
	state->program_name = NULL;

	if (NULL != state->display_buffer)
		free(state->display_buffer);
	state->display_buffer = NULL;

	if (NULL != state->name) {
		free(state->name);
		state->name = NULL;
	}

	if (NULL != state->format_string) {
		free(state->format_string);
		state->format_string = NULL;
	}

	if (NULL != state->transfer_buffer)
		free(state->transfer_buffer);
	state->transfer_buffer = NULL;

	if (NULL != state->history)
		free(state->history);
	state->history = NULL;

	if (NULL != state->input_files) {
		unsigned int file_idx;
		for (file_idx = 0; file_idx < state->input_file_count; file_idx++) {
			/*@-unqualifiedtrans@ */
			free(state->input_files[file_idx]);
			/*@+unqualifiedtrans@ */
			/* splint: see similar code below. */
		}
		free(state->input_files);
		state->input_files = NULL;
	}

	free(state);

	return;
}


/*
 * Set the formatting string, given a set of old-style formatting options.
 */
void pv_state_set_format(pvstate_t state, bool progress, bool timer, bool eta, bool fineta, bool rate, bool average_rate, bool bytes, bool bufpercent, unsigned int lastwritten,	/*@null@ */
			 const char *name)
{
#define PV_ADDFORMAT(x,y) if (x) { \
		if (state->default_format[0] != '\0') \
			(void) pv_strlcat(state->default_format, " ", sizeof(state->default_format)); \
		(void) pv_strlcat(state->default_format, y, sizeof(state->default_format)); \
	}

	state->default_format[0] = '\0';
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
		(void) pv_snprintf(buf, sizeof(buf), "%%%uA", lastwritten);
		PV_ADDFORMAT(lastwritten > 0, buf);
		/*
		 * flawfinder rationale: large enough for string, zeroed
		 * before use, only written to by pv_snprintf() with the
		 * right buffer length.
		 */
	}

	if (NULL != state->name) {
		free(state->name);
		state->name = NULL;
	}

	if (NULL != name)
		state->name = pv_strdup(name);

	state->reparse_display = 1;
}


void pv_state_force_set(pvstate_t state, bool val)
{
	state->force = val;
}

void pv_state_cursor_set(pvstate_t state, bool val)
{
	state->cursor = val;
}

void pv_state_numeric_set(pvstate_t state, bool val)
{
	state->numeric = val;
}

void pv_state_wait_set(pvstate_t state, bool val)
{
	state->wait = val;
}

void pv_state_delay_start_set(pvstate_t state, double val)
{
	state->delay_start = val;
}

void pv_state_linemode_set(pvstate_t state, bool val)
{
	state->linemode = val;
}

void pv_state_bits_set(pvstate_t state, bool bits)
{
	state->bits = bits;
}

void pv_state_null_terminated_lines_set(pvstate_t state, bool val)
{
	state->null_terminated_lines = val;
}

void pv_state_no_display_set(pvstate_t state, bool val)
{
	state->no_display = val;
}

void pv_state_skip_errors_set(pvstate_t state, unsigned int val)
{
	state->skip_errors = val;
}

void pv_state_error_skip_block_set(pvstate_t state, off_t val)
{
	state->error_skip_block = val;
}

void pv_state_stop_at_size_set(pvstate_t state, bool val)
{
	state->stop_at_size = val;
}

void pv_state_sync_after_write_set(pvstate_t state, bool val)
{
	state->sync_after_write = val;
}

void pv_state_direct_io_set(pvstate_t state, bool val)
{
	state->direct_io = val;
	state->direct_io_changed = true;
}

void pv_state_discard_input_set(pvstate_t state, bool val)
{
	state->discard_input = val;
}

void pv_state_rate_limit_set(pvstate_t state, off_t val)
{
	state->rate_limit = val;
}

void pv_state_target_buffer_size_set(pvstate_t state, size_t val)
{
	state->target_buffer_size = val;
}

void pv_state_no_splice_set(pvstate_t state, bool val)
{
	state->no_splice = val;
}

void pv_state_size_set(pvstate_t state, off_t val)
{
	state->size = val;
}

void pv_state_interval_set(pvstate_t state, double val)
{
	state->interval = val;
}

void pv_state_width_set(pvstate_t state, unsigned int val, bool was_set_manually)
{
	state->width = val;
	state->width_set_manually = was_set_manually;
}

void pv_state_height_set(pvstate_t state, unsigned int val, bool was_set_manually)
{
	state->height = val;
	state->height_set_manually = was_set_manually;
}

void pv_state_name_set(pvstate_t state, /*@null@ */ const char *val)
{
	if (NULL != state->name) {
		free(state->name);
		state->name = NULL;
	}
	if (NULL != val)
		state->name = pv_strdup(val);
}

void pv_state_format_string_set(pvstate_t state, /*@null@ */ const char *val)
{
	if (NULL != state->format_string) {
		free(state->format_string);
		state->format_string = NULL;
	}
	if (NULL != val)
		state->format_string = pv_strdup(val);
}

void pv_state_watch_pid_set(pvstate_t state, unsigned int val)
{
	state->watch_pid = val;
}

void pv_state_watch_fd_set(pvstate_t state, int val)
{
	state->watch_fd = val;
}

void pv_state_average_rate_window_set(pvstate_t state, unsigned int val)
{
	if (val < 1)
		val = 1;
	if (val >= 20) {
		state->history_len = val / 5 + 1;
		state->history_interval = 5;
	} else {
		state->history_len = val + 1;
		state->history_interval = 1;
	}
	pv_alloc_history(state);
}


/*
 * Set the array of input files.
 */
void pv_state_inputfiles(pvstate_t state, unsigned int input_file_count, const char **input_files)
{
	unsigned int file_idx;

	if (NULL != state->input_files) {
		for (file_idx = 0; file_idx < state->input_file_count; file_idx++) {
			/*@-unqualifiedtrans@ */
			free(state->input_files[file_idx]);
			/*@+unqualifiedtrans@ */
			/*
			 * TODO: find a way to tell splint the array
			 * contents are "only" and "null" as well as the
			 * array itself.
			 */
		}
		free(state->input_files);
		state->input_files = NULL;
		state->input_file_count = 0;
	}
	state->input_files = calloc((size_t) (input_file_count + 1), sizeof(char *));
	if (NULL == state->input_files) {
		/*@-mustfreefresh@ *//* see similar _() issue above */
		fprintf(stderr, "%s: %s: %s\n", state->program_name, _("file list allocation failed"), strerror(errno));
		/*@+mustfreefresh@ */
		return;
	}
	for (file_idx = 0; file_idx < input_file_count; file_idx++) {
		/*@-nullstate@ */
		state->input_files[file_idx] = pv_strdup(input_files[file_idx]);
		if (NULL == state->input_files[file_idx]) {
			/*@-mustfreefresh@ *//* see similar _() issue above */
			fprintf(stderr, "%s: %s: %s\n", state->program_name,
				_("file list allocation failed"), strerror(errno));
			/*@+mustfreefresh@ */
			return;
		}
	}
	state->input_file_count = input_file_count;
}

/*@+nullstate@*/
/* splint: see unqualifiedtrans note by free() above. */

/* EOF */
