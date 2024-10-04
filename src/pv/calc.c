/*
 * Functions for updating the calculated state of the transfer.
 *
 * Copyright 2024 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"


/*
 * Update the current average rate, using a ring buffer of past transfer
 * positions - if this is the first entry, use the provided instantaneous
 * rate, otherwise calulate the average rate from the difference between the
 * current position + elapsed time pair, and the oldest pair in the buffer.
 */
static void pv__update_average_rate_history(pvstate_t state, long double rate)
{
	size_t first = state->calc.history_first;
	size_t last = state->calc.history_last;
	long double last_elapsed;

	if (NULL == state->calc.history)
		return;

	last_elapsed = state->calc.history[last].elapsed_sec;

	/*
	 * Do nothing if this is not the first call but not enough time has
	 * elapsed since the previous call yet.
	 */
	if ((last_elapsed > 0.0)
	    && (state->transfer.elapsed_seconds < (last_elapsed + state->control.history_interval)))
		return;

	/*
	 * If this is not the first call, add a new entry to the circular
	 * buffer.
	 */
	if (last_elapsed > 0.0) {
		size_t len = state->calc.history_len;
		last = (last + 1) % len;
		state->calc.history_last = last;
		if (last == first) {
			first = (first + 1) % len;
			state->calc.history_first = first;
		}
	}

	state->calc.history[last].elapsed_sec = state->transfer.elapsed_seconds;
	state->calc.history[last].total_written = state->transfer.total_written;

	if (first == last) {
		state->calc.current_avg_rate = rate;
	} else {
		off_t bytes = (state->calc.history[last].total_written - state->calc.history[first].total_written);
		long double sec = (state->calc.history[last].elapsed_sec - state->calc.history[first].elapsed_sec);
		state->calc.current_avg_rate = (long double) bytes / sec;
	}
}


/*
 * Update all calculated transfer state (state->calc).
 *
 * If "final" is true, this is the final update, so
 * state->calc.transfer_rate and state->calc.average_rate are given as an
 * average over the whole transfer; otherwise they are the current transfer
 * rate and current average rate.
 *
 * The value of state->calc.percentage will reflect the percentage
 * completion if state->control.size is greater than zero, otherwise it will
 * increase by 2 each call and wrap at 200.
 */
void pv_calculate_transfer_rate(pvstate_t state, bool final)
{
	off_t bytes_since_last;
	long double time_since_last, transfer_rate, average_rate;

	/* Quick safety check - state must exist. */
	if (NULL == state)
		return;

	bytes_since_last = 0;
	if (state->transfer.total_written >= 0) {
		bytes_since_last = state->transfer.total_written - state->calc.prev_total_written;
		state->calc.prev_total_written = state->transfer.total_written;
	}

	/*
	 * In case the time since the last update is very small, we keep
	 * track of amount transferred since the last update, and just keep
	 * adding to that until a reasonable amount of time has passed to
	 * avoid rate spikes or division by zero.
	 */
	time_since_last = state->transfer.elapsed_seconds - state->calc.prev_elapsed_sec;
	if (time_since_last <= 0.01) {
		transfer_rate = state->calc.prev_rate;
		state->calc.prev_trans += bytes_since_last;
	} else {
		long double measured_rate;

		transfer_rate = ((long double) bytes_since_last + state->calc.prev_trans) / time_since_last;
		measured_rate = transfer_rate;

		state->calc.prev_elapsed_sec = state->transfer.elapsed_seconds;
		state->calc.prev_trans = 0;

		if (state->control.bits)
			measured_rate = 8.0 * measured_rate;

		if ((state->calc.measurements_taken < 1) || (measured_rate < state->calc.rate_min)) {
			state->calc.rate_min = measured_rate;
		}
		if (measured_rate > state->calc.rate_max) {
			state->calc.rate_max = measured_rate;
		}
		state->calc.rate_sum += measured_rate;
		state->calc.ratesquared_sum += (measured_rate * measured_rate);
		state->calc.measurements_taken++;
	}
	state->calc.prev_rate = transfer_rate;

	/* Update history and current average rate for ETA. */
	pv__update_average_rate_history(state, transfer_rate);
	average_rate = state->calc.current_avg_rate;

	/*
	 * If this is the final update at the end of the transfer, we
	 * recalculate the rate - and the average rate - across the whole
	 * period of the transfer.
	 */
	if (final) {
		/* Safety check to avoid division by zero. */
		if (state->transfer.elapsed_seconds < 0.000001)
			state->transfer.elapsed_seconds = 0.000001;
		average_rate =
		    (((long double) state->transfer.total_written) -
		     ((long double) state->display.initial_offset)) / (long double) (state->transfer.elapsed_seconds);
		transfer_rate = average_rate;
	}

	state->calc.transfer_rate = transfer_rate;
	state->calc.average_rate = average_rate;

	if (state->control.size <= 0) {
		/*
		 * If we don't know the total size of the incoming data,
		 * then for a percentage, we gradually increase the
		 * percentage completion as data arrives, to a maximum of
		 * 200, then reset it - we use this if we can't calculate
		 * it, so that the numeric percentage output will go
		 * 0%-100%, 100%-0%, 0%-100%, and so on.
		 */
		if (transfer_rate > 0)
			state->calc.percentage += 2;
		if (state->calc.percentage > 199)
			state->calc.percentage = 0;
	} else {
		state->calc.percentage = pv_percentage(state->transfer.total_written, state->control.size);
	}

	/* Ensure the percentage is never negative or huge. */
	if (state->calc.percentage < 0)
		state->calc.percentage = 0;
	if (state->calc.percentage > 100000)
		state->calc.percentage = 100000;
}

/* EOF */
