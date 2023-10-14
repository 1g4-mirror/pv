/*
 * Signal handling functions.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
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

/*@null@*/ static pvstate_t pv_sig_state = NULL;


/*
 * Ensure that terminal attribute TOSTOP is set.  If we have to set it,
 * record that fact by setting the state boolean "pv_tty_tostop_added" to
 * true, so that in pv_sig_fini() we can turn it back off again.
 */
static void pv_sig_ensure_tty_tostop()
{
	struct termios terminal_attributes;

	if (NULL == pv_sig_state)
		return;

	if (0 != tcgetattr(STDERR_FILENO, &terminal_attributes)) {
		debug("%s: %s", "failed to read terminal attributes", strerror(errno));
		return;
	}

	if (0 == (terminal_attributes.c_lflag & TOSTOP)) {
		terminal_attributes.c_lflag |= TOSTOP;
		if (0 == tcsetattr(STDERR_FILENO, TCSANOW, &terminal_attributes)) {
			pv_sig_state->signal.pv_tty_tostop_added = true;
			debug("%s", "set terminal TOSTOP attribute");
		} else {
			debug("%s: %s", "failed to set terminal TOSTOP attribute", strerror(errno));
		}
#if HAVE_IPC
		/*
		 * In "-c" mode with IPC, make all "pv -c" instances aware
		 * that we set TOSTOP, so the last one can clear it on exit.
		 */
		if (pv_sig_state->control.cursor && (NULL != pv_sig_state->cursor.shared)
		    && (!pv_sig_state->cursor.noipc)) {
			pv_sig_state->cursor.shared->tty_tostop_added = true;
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
static void pv_sig_ttou( /*@unused@ */  __attribute__((unused))
			int s)
{
	int fd;

	if (NULL == pv_sig_state)
		return;

	fd = open("/dev/null", O_RDWR);	    /* flawfinder: ignore */
	if (fd < 0) {
		debug("%s: %s", "failed to open /dev/null", strerror(errno));
		return;
	}

	/*
	 * flawfinder rationale: not checking for symlinks because this is
	 * explicitly a device file under /dev so we assume it is safe to
	 * open.
	 *
	 * TODO: look at just preventing stderr output instead of writing to
	 * stderr while backgrounded.
	 */

	if (-1 == pv_sig_state->signal.old_stderr)
		pv_sig_state->signal.old_stderr = dup(STDERR_FILENO);

	if (dup2(fd, STDERR_FILENO) < 0) {
		debug("%s: %s", "failed to replace stderr", strerror(errno));
	}

	if (0 != close(fd)) {
		debug("%s: %s", "failed to close /dev/null", strerror(errno));
	}
}


/*
 * Handle SIGTSTP (stop typed at tty) by storing the time the signal
 * happened for later use by pv_sig_cont(), and then stopping the process.
 */
static void pv_sig_tstp( /*@unused@ */  __attribute__((unused))
			int s)
{
	if (NULL == pv_sig_state)
		return;
	pv_elapsedtime_read(&(pv_sig_state->signal.tstp_time));
	if (0 != raise(SIGSTOP)) {
		debug("%s: %s", "raise", strerror(errno));
	}
}


/*
 * Handle SIGCONT (continue if stopped) by adding the elapsed time since the
 * last SIGTSTP to the elapsed time offset, and by trying to write to the
 * terminal again (by replacing the /dev/null stderr with the old stderr).
 */
static void pv_sig_cont( /*@unused@ */  __attribute__((unused))
			int s)
{
	struct timespec current_time;
	struct timespec time_spent_stopped;

	if (NULL == pv_sig_state)
		return;

	pv_sig_state->flag.terminal_resized = 1;

	/*
	 * We can only make the time adjustments if this SIGCONT followed a
	 * SIGTSTP such that we have a stop time.
	 */
	if (0 != pv_sig_state->signal.tstp_time.tv_sec) {

		memset(&current_time, 0, sizeof(current_time));
		memset(&time_spent_stopped, 0, sizeof(time_spent_stopped));

		pv_elapsedtime_read(&current_time);

		/* time spent stopped = current time - time SIGTSTP received */
		pv_elapsedtime_subtract(&time_spent_stopped, &current_time, &(pv_sig_state->signal.tstp_time));

		/* add time spent stopped the total stopped-time count */
		pv_elapsedtime_add(&(pv_sig_state->signal.toffset), &(pv_sig_state->signal.toffset),
				   &time_spent_stopped);

		/* reset the SIGTSTP receipt time */
		pv_elapsedtime_zero(&(pv_sig_state->signal.tstp_time));
	}

	/*
	 * Restore the old stderr, if we had replaced it.
	 */
	if (pv_sig_state->signal.old_stderr != -1) {
		if (dup2(pv_sig_state->signal.old_stderr, STDERR_FILENO) < 0) {
			debug("%s: %s", "failed to restore old stderr", strerror(errno));
		}
		if (0 != close(pv_sig_state->signal.old_stderr)) {
			debug("%s: %s", "failed to close duplicate old stderr", strerror(errno));
		}
		pv_sig_state->signal.old_stderr = -1;
	}

	pv_sig_ensure_tty_tostop();

#ifdef HAVE_IPC
	pv_crs_needreinit(pv_sig_state);
#endif
}


#ifdef SIGWINCH
/*
 * Handle SIGWINCH (window size changed) by setting a flag.
 */
static void pv_sig_winch( /*@unused@ */  __attribute__((unused))
			 int s)
{
	if (NULL == pv_sig_state)
		return;
	pv_sig_state->flag.terminal_resized = 1;
}
#endif


/*
 * Handle termination signals by setting the abort flag.
 */
static void pv_sig_term( /*@unused@ */  __attribute__((unused))
			int s)
{
	if (NULL == pv_sig_state)
		return;
	pv_sig_state->flag.trigger_exit = 1;
}


#ifdef SA_SIGINFO
/*
 * Handle a SIGUSR2 by setting a flag to say we received it, after recording
 * the sending PID.
 */
static void pv_sig_usr2( /*@unused@ */  __attribute__((unused))
			int sig, siginfo_t * info, /*@unused@ */  __attribute__((unused))
			void *ucontext)
{
	if (NULL == pv_sig_state)
		return;
	if (NULL == info)
		return;
	pv_sig_state->signal.rxusr2 = 1;
	pv_sig_state->signal.sender = info->si_pid;
}


/*
 * Return true if a SIGUSR2 signal has been received since the last time
 * this function was called, populating *pid with the sending PID if so.
 */
bool pv_sigusr2_received(pvstate_t state, pid_t * pid)
{
	if (NULL == state)
		return false;
	if (0 == state->signal.rxusr2)
		return false;
	if (NULL != pid)
		*pid = state->signal.sender;
	state->signal.rxusr2 = 0;
	return true;
}

#endif


/*
 * Initialise signal handling.
 */
void pv_sig_init(pvstate_t state)
{
	static struct sigaction sa;

	memset(&sa, 0, sizeof(sa));

	/*
	 * Note that wherever we use a "struct sigaction", we declare it
	 * static and explicitly zero it before use, because it may contain
	 * deeper structures (e.g.  "sigset_t") which trigger splint
	 * warnings about potential memory leaks.
	 */

	pv_sig_state = state;

	pv_sig_state->signal.old_stderr = -1;
	pv_elapsedtime_zero(&(pv_sig_state->signal.tstp_time));
	pv_elapsedtime_zero(&(pv_sig_state->signal.toffset));

	/*
	 * Note that we cast all sigemptyset() and sigaction() return values
	 * to void, because there's nothing we can reasonably do about any
	 * conceivable error they may return.
	 */

	/*
	 * Ignore SIGPIPE, so we don't die if stdout is a pipe and the other
	 * end closes unexpectedly.
	 */
	sa.sa_handler = SIG_IGN;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGPIPE, &sa, &(pv_sig_state->signal.old_sigpipe));

	/*
	 * Handle SIGTTOU by continuing with output switched off, so that we
	 * can be stopped and backgrounded without messing up the terminal.
	 */
	sa.sa_handler = pv_sig_ttou;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGTTOU, &sa, &(pv_sig_state->signal.old_sigttou));

	/*
	 * Handle SIGTSTP by storing the time the signal happened for later
	 * use by pv_sig_cont(), and then stopping the process.
	 */
	sa.sa_handler = pv_sig_tstp;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGTSTP, &sa, &(pv_sig_state->signal.old_sigtstp));

	/*
	 * Handle SIGCONT by adding the elapsed time since the last SIGTSTP
	 * to the elapsed time offset, and by trying to write to the
	 * terminal again.
	 */
	sa.sa_handler = pv_sig_cont;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGCONT, &sa, &(pv_sig_state->signal.old_sigcont));

	/*
	 * Handle SIGWINCH by setting a flag to let the main loop know it
	 * has to reread the terminal size.
	 */
#ifdef SIGWINCH
	sa.sa_handler = pv_sig_winch;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGWINCH, &sa, &(pv_sig_state->signal.old_sigwinch));
#endif

	/*
	 * Handle SIGINT, SIGHUP, SIGTERM by setting a flag to let the
	 * main loop know it should quit now.
	 */
	sa.sa_handler = pv_sig_term;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGINT, &sa, &(pv_sig_state->signal.old_sigint));

	sa.sa_handler = pv_sig_term;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGHUP, &sa, &(pv_sig_state->signal.old_sighup));

	sa.sa_handler = pv_sig_term;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGTERM, &sa, &(pv_sig_state->signal.old_sigterm));

#ifdef SA_SIGINFO
	/*
	 * Handle SIGUSR2 by setting a flag to say the signal has been
	 * received, and storing the sending process's PID.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = pv_sig_usr2;
	(void) sigemptyset(&(sa.sa_mask));
	/*@-unrecog@ *//* splint doesn't know about SA_SIGINFO */
	sa.sa_flags = SA_SIGINFO;
	/*@+unrecog@ */
	(void) sigaction(SIGUSR2, &sa, &(pv_sig_state->signal.old_sigusr2));
	memset(&sa, 0, sizeof(sa));
#endif

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
void pv_sig_fini( /*@unused@ */  __attribute__((unused)) pvstate_t state)
{
	bool need_to_clear_tostop = false;

	if (NULL == pv_sig_state)
		return;

	(void) sigaction(SIGPIPE, &(pv_sig_state->signal.old_sigpipe), NULL);
	(void) sigaction(SIGTTOU, &(pv_sig_state->signal.old_sigttou), NULL);
	(void) sigaction(SIGTSTP, &(pv_sig_state->signal.old_sigtstp), NULL);
	(void) sigaction(SIGCONT, &(pv_sig_state->signal.old_sigcont), NULL);
#ifdef SIGWINCH
	(void) sigaction(SIGWINCH, &(pv_sig_state->signal.old_sigwinch), NULL);
#endif
	(void) sigaction(SIGINT, &(pv_sig_state->signal.old_sigint), NULL);
	(void) sigaction(SIGHUP, &(pv_sig_state->signal.old_sighup), NULL);
	(void) sigaction(SIGTERM, &(pv_sig_state->signal.old_sigterm), NULL);
#ifdef SA_SIGINFO
	(void) sigaction(SIGUSR2, &(pv_sig_state->signal.old_sigusr2), NULL);
#endif

	need_to_clear_tostop = pv_sig_state->signal.pv_tty_tostop_added;

	if (pv_sig_state->control.cursor) {
#ifdef HAVE_IPC
		/*
		 * We won't clear TOSTOP if other "pv -c" instances
		 * were still running when pv_crs_fini() ran.
		 *
		 * TODO: we need a better way to determine if we're the last
		 * "pv" left.
		 */
		if (pv_sig_state->control.cursor && pv_sig_state->cursor.pvcount > 1) {
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

		if (0 != tcgetattr(STDERR_FILENO, &terminal_attributes)) {
			debug("%s: %s", "tcgetattr", strerror(errno));
		} else if (0 != (terminal_attributes.c_lflag & TOSTOP)) {
			terminal_attributes.c_lflag -= TOSTOP;
			if (0 == tcsetattr(STDERR_FILENO, TCSANOW, &terminal_attributes)) {
				debug("%s", "cleared TOSTOP terminal attribute");
			} else {
				debug("%s: %s", "failed to clear TOSTOP terminal attribute", strerror(errno));
			}
		}

		pv_sig_state->signal.pv_tty_tostop_added = false;
	}
}


/*
 * Stop reacting to SIGTSTP and SIGCONT.
 */
void pv_sig_nopause(void)
{
	static struct sigaction sa;

	memset(&sa, 0, sizeof(sa));

	sa.sa_handler = SIG_IGN;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGTSTP, &sa, NULL);

	sa.sa_handler = SIG_DFL;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGCONT, &sa, NULL);
}


/*
 * Start catching SIGTSTP and SIGCONT again.
 */
void pv_sig_allowpause(void)
{
	static struct sigaction sa;

	memset(&sa, 0, sizeof(sa));

	sa.sa_handler = pv_sig_tstp;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGTSTP, &sa, NULL);

	sa.sa_handler = pv_sig_cont;
	(void) sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void) sigaction(SIGCONT, &sa, NULL);
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

	if (NULL == pv_sig_state)
		return;

	if (time(NULL) < next_check)
		return;

	next_check = time(NULL) + 1;

	if (-1 == pv_sig_state->signal.old_stderr)
		return;

	if (dup2(pv_sig_state->signal.old_stderr, STDERR_FILENO) < 0) {
		debug("%s: %s", "failed to restore old stderr", strerror(errno));
	}

	if (0 != close(pv_sig_state->signal.old_stderr)) {
		debug("%s: %s", "failed to close duplicate old stderr", strerror(errno));
	}

	pv_sig_state->signal.old_stderr = -1;

	pv_sig_ensure_tty_tostop();
#ifdef HAVE_IPC
	pv_crs_needreinit(pv_sig_state);
#endif
}

/* EOF */
