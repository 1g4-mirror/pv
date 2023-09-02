/*
 * Signal handling functions.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * Distributed under the Artistic License v2.0; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"
#include "pv-internal.h"

#include <string.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef HAVE_IPC
void pv_crs_needreinit(pvstate_t);
#endif

static pvstate_t pv_sig_state = NULL;


/*
 * Ensure that terminal attribute TOSTOP is set.  If we have to set it,
 * record that fact by setting the state boolean "pv_tty_tostop_added" to
 * true, so that in pv_sig_fini() we can turn it back off again.
 */
static void pv_sig_ensure_tty_tostop()
{
	struct termios terminal_attributes;

	if (0 != tcgetattr(STDERR_FILENO, &terminal_attributes)) {
		debug("%s: %s", "failed to read terminal attributes", strerror(errno));
		return;
	}

	if (0 == (terminal_attributes.c_lflag & TOSTOP)) {
		terminal_attributes.c_lflag |= TOSTOP;
		if (0 == tcsetattr(STDERR_FILENO, TCSANOW, &terminal_attributes)) {
			pv_sig_state->pv_tty_tostop_added = true;
			debug("%s", "set terminal TOSTOP attribute");
		} else {
			debug("%s: %s", "failed to set terminal TOSTOP attribute", strerror(errno));
		}
#if HAVE_IPC
		/*
		 * In "-c" mode with IPC, make all "pv -c" instances aware
		 * that we set TOSTOP, so the last one can clear it on exit.
		 */
		if (pv_sig_state->cursor && (NULL != pv_sig_state->crs_shared) && (!pv_sig_state->crs_noipc)) {
			pv_sig_state->crs_shared->tty_tostop_added = true;
		}
#endif
	}
}

/*
 * Handle SIGTTOU (tty output for background process) by redirecting stderr
 * to /dev/null, so that we can be stopped and backgrounded without messing
 * up the terminal. We store the old stderr file descriptor so that on a
 * subsequent SIGCONT we can try writing to the terminal again, in case we
 * get backgrounded and later get foregrounded again.
 */
static void pv_sig_ttou( __attribute__((unused))
			int s)
{
	int fd;

	fd = open("/dev/null", O_RDWR);
	if (fd < 0)
		return;

	if (-1 == pv_sig_state->pv_sig_old_stderr)
		pv_sig_state->pv_sig_old_stderr = dup(STDERR_FILENO);

	dup2(fd, STDERR_FILENO);
	close(fd);
}


/*
 * Handle SIGTSTP (stop typed at tty) by storing the time the signal
 * happened for later use by pv_sig_cont(), and then stopping the process.
 */
static void pv_sig_tstp( __attribute__((unused))
			int s)
{
	pv_elapsedtime_read(&(pv_sig_state->pv_sig_tstp_time));
	raise(SIGSTOP);
}


/*
 * Handle SIGCONT (continue if stopped) by adding the elapsed time since the
 * last SIGTSTP to the elapsed time offset, and by trying to write to the
 * terminal again (by replacing the /dev/null stderr with the old stderr).
 */
static void pv_sig_cont( __attribute__((unused))
			int s)
{
	struct timespec current_time;
	struct timespec time_spent_stopped;

	pv_sig_state->pv_sig_newsize = 1;

	/*
	 * We can only make the time adjustments if this SIGCONT followed a
	 * SIGTSTP such that we have a stop time.
	 */
	if (0 != pv_sig_state->pv_sig_tstp_time.tv_sec) {

		pv_elapsedtime_read(&current_time);

		/* time spent stopped = current time - time SIGTSTP received */
		pv_elapsedtime_subtract(&time_spent_stopped, &current_time, &(pv_sig_state->pv_sig_tstp_time));

		/* add time spent stopped the total stopped-time count */
		pv_elapsedtime_add(&(pv_sig_state->pv_sig_toffset), &(pv_sig_state->pv_sig_toffset),
				   &time_spent_stopped);

		/* reset the SIGTSTP receipt time */
		pv_elapsedtime_zero(&(pv_sig_state->pv_sig_tstp_time));
	}

	/*
	 * Restore the old stderr, if we had replaced it.
	 */
	if (pv_sig_state->pv_sig_old_stderr != -1) {
		dup2(pv_sig_state->pv_sig_old_stderr, STDERR_FILENO);
		close(pv_sig_state->pv_sig_old_stderr);
		pv_sig_state->pv_sig_old_stderr = -1;
	}

	pv_sig_ensure_tty_tostop();

#ifdef HAVE_IPC
	pv_crs_needreinit(pv_sig_state);
#endif
}


/*
 * Handle SIGWINCH (window size changed) by setting a flag.
 */
static void pv_sig_winch( __attribute__((unused))
			 int s)
{
	pv_sig_state->pv_sig_newsize = 1;
}


/*
 * Handle termination signals by setting the abort flag.
 */
static void pv_sig_term( __attribute__((unused))
			int s)
{
	pv_sig_state->pv_sig_abort = 1;
}


/*
 * Initialise signal handling.
 */
void pv_sig_init(pvstate_t state)
{
	struct sigaction sa;

	pv_sig_state = state;

	pv_sig_state->pv_sig_old_stderr = -1;
	pv_elapsedtime_zero(&(pv_sig_state->pv_sig_tstp_time));
	pv_elapsedtime_zero(&(pv_sig_state->pv_sig_toffset));

	/*
	 * Ignore SIGPIPE, so we don't die if stdout is a pipe and the other
	 * end closes unexpectedly.
	 */
	sa.sa_handler = SIG_IGN;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGPIPE, &sa, &(pv_sig_state->pv_sig_old_sigpipe));

	/*
	 * Handle SIGTTOU by continuing with output switched off, so that we
	 * can be stopped and backgrounded without messing up the terminal.
	 */
	sa.sa_handler = pv_sig_ttou;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGTTOU, &sa, &(pv_sig_state->pv_sig_old_sigttou));

	/*
	 * Handle SIGTSTP by storing the time the signal happened for later
	 * use by pv_sig_cont(), and then stopping the process.
	 */
	sa.sa_handler = pv_sig_tstp;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGTSTP, &sa, &(pv_sig_state->pv_sig_old_sigtstp));

	/*
	 * Handle SIGCONT by adding the elapsed time since the last SIGTSTP
	 * to the elapsed time offset, and by trying to write to the
	 * terminal again.
	 */
	sa.sa_handler = pv_sig_cont;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGCONT, &sa, &(pv_sig_state->pv_sig_old_sigcont));

	/*
	 * Handle SIGWINCH by setting a flag to let the main loop know it
	 * has to reread the terminal size.
	 */
	sa.sa_handler = pv_sig_winch;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGWINCH, &sa, &(pv_sig_state->pv_sig_old_sigwinch));

	/*
	 * Handle SIGINT, SIGHUP, SIGTERM by setting a flag to let the
	 * main loop know it should quit now.
	 */
	sa.sa_handler = pv_sig_term;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, &(pv_sig_state->pv_sig_old_sigint));

	sa.sa_handler = pv_sig_term;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGHUP, &sa, &(pv_sig_state->pv_sig_old_sighup));

	sa.sa_handler = pv_sig_term;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, &(pv_sig_state->pv_sig_old_sigterm));

	/*
	 * Ensure that the TOSTOP terminal attribute is set, so that a
	 * SIGTTOU signal will be raised if we try to write to the terminal
	 * while backgrounded (see the SIGTTOU handler above).
	 */
	pv_sig_ensure_tty_tostop();
}


/*
 * Shut down signal handling.  If we had set the TOSTOP terminal attribute,
 * and we're in the foreground, also turn that off (though if we're in
 * cursor "-c" mode, only do that if we're the last PV instance, otherwise
 * leave the terminal alone).
 */
void pv_sig_fini( __attribute__((unused)) pvstate_t state)
{
	bool need_to_clear_tostop = false;

	sigaction(SIGPIPE, &(pv_sig_state->pv_sig_old_sigpipe), NULL);
	sigaction(SIGTTOU, &(pv_sig_state->pv_sig_old_sigttou), NULL);
	sigaction(SIGTSTP, &(pv_sig_state->pv_sig_old_sigtstp), NULL);
	sigaction(SIGCONT, &(pv_sig_state->pv_sig_old_sigcont), NULL);
	sigaction(SIGWINCH, &(pv_sig_state->pv_sig_old_sigwinch), NULL);
	sigaction(SIGINT, &(pv_sig_state->pv_sig_old_sigint), NULL);
	sigaction(SIGHUP, &(pv_sig_state->pv_sig_old_sighup), NULL);
	sigaction(SIGTERM, &(pv_sig_state->pv_sig_old_sigterm), NULL);

	need_to_clear_tostop = pv_sig_state->pv_tty_tostop_added;

	if (pv_sig_state->cursor) {
#ifdef HAVE_IPC
		/*
		 * We won't clear TOSTOP if other "pv -c" instances
		 * were still running when pv_crs_fini() ran.
		 *
		 * TODO: we need a better way to determine if we're the last
		 * "pv" left.
		 */
		if (pv_sig_state->cursor && pv_sig_state->crs_pvcount > 1) {
			need_to_clear_tostop = false;
		}
#else				/* !HAVE_IPC */
		/*
		 * Without IPC we can't tell whether the other "pv -c"
		 * instances in the pipeline have finished so we will just
		 * have to clear TOSTOP anyway.
		 */
#endif				/* !HAVE_IPC */
	}

	debug("%s=%s", "need_to_clear_tostop", need_to_clear_tostop ? "true" : "false");

	if (need_to_clear_tostop && pv_in_foreground()) {
		struct termios terminal_attributes;

		debug("%s", "about to to clear TOSTOP terminal attribute if it is set");

		tcgetattr(STDERR_FILENO, &terminal_attributes);
		if (0 != (terminal_attributes.c_lflag & TOSTOP)) {
			terminal_attributes.c_lflag -= TOSTOP;
			if (0 == tcsetattr(STDERR_FILENO, TCSANOW, &terminal_attributes)) {
				debug("%s", "cleared TOSTOP terminal attribute");
			} else {
				debug("%s: %s", "failed to clear TOSTOP terminal attribute", strerror(errno));
			}
		}

		pv_sig_state->pv_tty_tostop_added = false;
	}
}


/*
 * Stop reacting to SIGTSTP and SIGCONT.
 */
void pv_sig_nopause(void)
{
	struct sigaction sa;

	sa.sa_handler = SIG_IGN;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGTSTP, &sa, NULL);

	sa.sa_handler = SIG_DFL;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGCONT, &sa, NULL);
}


/*
 * Start catching SIGTSTP and SIGCONT again.
 */
void pv_sig_allowpause(void)
{
	struct sigaction sa;

	sa.sa_handler = pv_sig_tstp;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGTSTP, &sa, NULL);

	sa.sa_handler = pv_sig_cont;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGCONT, &sa, NULL);
}


/*
 * If we have redirected stderr to /dev/null, check every second or so to
 * see whether we can write to the terminal again - this is so that if we
 * get backgrounded, then foregrounded again, we start writing to the
 * terminal again.
 */
void pv_sig_checkbg(void)
{
	static time_t next_check = 0;

	if (time(NULL) < next_check)
		return;

	next_check = time(NULL) + 1;

	if (-1 == pv_sig_state->pv_sig_old_stderr)
		return;

	dup2(pv_sig_state->pv_sig_old_stderr, STDERR_FILENO);
	close(pv_sig_state->pv_sig_old_stderr);
	pv_sig_state->pv_sig_old_stderr = -1;

	pv_sig_ensure_tty_tostop();
#ifdef HAVE_IPC
	pv_crs_needreinit(pv_sig_state);
#endif
}

/* EOF */
