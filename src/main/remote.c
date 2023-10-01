/*
 * Remote-control functions.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "options.h"
#include "pv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#ifdef SA_SIGINFO
void pv_error(pvstate_t, char *, ...);

struct remote_msg {
	bool progress;			 /* progress bar flag */
	bool timer;			 /* timer flag */
	bool eta;			 /* ETA flag */
	bool fineta;			 /* absolute ETA flag */
	bool rate;			 /* rate counter flag */
	bool average_rate;		 /* average rate counter flag */
	bool bytes;			 /* bytes transferred flag */
	bool bufpercent;		 /* transfer buffer percentage flag */
	size_t lastwritten;		 /* last-written bytes count */
	off_t rate_limit;		 /* rate limit, in bytes per second */
	size_t buffer_size;		 /* buffer size, in bytes (0=default) */
	off_t size;			 /* total size of data */
	double interval;		 /* interval between updates */
	unsigned int width;		 /* screen width */
	unsigned int height;		 /* screen height */
	bool width_set_manually;	 /* width was set manually, not detected */
	bool height_set_manually;	 /* height was set manually, not detected */
	char name[256];			 /* flawfinder: ignore */
	char format[256];		 /* flawfinder: ignore */
};

/*
 * flawfinder rationale: name and format are always explicitly zeroed and
 * bounded to one less than their size so they are always \0 terminated.
 */


/*
 * Return a stream pointer, and populate the filename buffer, for a control
 * file associated with a particular process ID; it will be opened for
 * writing if "sender" is true.  Returns NULL on error.
 */
static FILE *pv__control_file(char *filename, size_t bufsize, pid_t control_pid, bool sender)
{
	int open_flags, open_mode, control_fd;
	FILE *control_fptr;

	open_flags = O_RDONLY;
#ifdef O_NOFOLLOW
	open_flags += O_NOFOLLOW;
#endif
	if (sender)
		open_flags = O_WRONLY | O_CREAT | O_EXCL;

	open_mode = 0644;

	(void) pv_snprintf(filename, bufsize, "/run/user/%lu/pv.remote.%lu", (unsigned long) geteuid(),
			   (unsigned long) control_pid);
	control_fd = open(filename, open_flags, open_mode);	/* flawfinder: ignore */

	/*
	 * If /run/user/<uid> wasn't usable, try $HOME/.pv instead.
	 */
	if (control_fd < 0) {
		char *home_dir;

		home_dir = getenv("HOME");  /* flawfinder: ignore */
		if (NULL == home_dir)
			return NULL;

		(void) pv_snprintf(filename, bufsize, "%s/.pv/remote.%lu", home_dir, (unsigned long) control_pid);
		control_fd = open(filename, open_flags, open_mode);	/* flawfinder: ignore */

		/*
		 * If the open failed, try creating the $HOME/.pv directory
		 * first.
		 */
		if (control_fd < 0) {
			(void) pv_snprintf(filename, bufsize, "%s/.pv", home_dir);
			(void) mkdir(filename, 0700);
			/* In case of weird umask, explicitly chmod the dir. */
			(void) chmod(filename, 0700);	/* flawfinder: ignore */
			(void) pv_snprintf(filename, bufsize, "%s/.pv/remote.%lu", home_dir,
					   (unsigned long) control_pid);
			control_fd = open(filename, open_flags, open_mode);	/* flawfinder: ignore */
		}
	}

	/*
	 * flawfinder rationale: the files are in a directory whose parents
	 * cannot be manipulated, and we are not allowing the final
	 * component to be a symbolic link.  We are checking that $HOME is
	 * not NULL, and it's bounded by pv_snprintf() so it can't overshoot
	 * the filename buffer.  When we chmod $HOME/.pv, we assume that an
	 * attacker is unlikely to be able to manipulate $HOME's contents to
	 * make use of us setting $HOME/.pv to mode 700.
	 */

	if (control_fd < 0)
		return NULL;

	debug("%s: %s", "control filename", filename);

	control_fptr = fdopen(control_fd, sender ? "wb" : "rb");

	return control_fptr;
}


/*
 * Set the options of a remote process by writing them to a file, sending a
 * signal to the receiving process, and waiting for the message to be
 * consumed by the remote process.
 *
 * Returns nonzero on error.
 */
int pv_remote_set(opts_t opts, pvstate_t state)
{
	char control_filename[4096];	 /* flawfinder: ignore */
	FILE *control_fptr;
	struct remote_msg msgbuf;
	pid_t signal_sender;
	long timeout;
	bool received;

	/*
	 * flawfinder rationale: buffer is large enough, explicitly zeroed,
	 * and always bounded properly as we are only writing to it with
	 * pv_snprintf().
	 */

	/*
	 * Check that the remote process exists.
	 */
	if (kill((pid_t) (opts->remote), 0) != 0) {
		pv_error(state, "%u: %s", opts->remote, strerror(errno));
		return 1;
	}

	/*
	 * Make sure parameters are within sensible bounds.
	 */
	if (opts->width < 1)
		opts->width = 80;
	if (opts->height < 1)
		opts->height = 25;
	if (opts->width > 999999)
		opts->width = 999999;
	if (opts->height > 999999)
		opts->height = 999999;
	if ((opts->interval > 0) && (opts->interval < 0.1))
		opts->interval = 0.1;
	if (opts->interval > 600)
		opts->interval = 600;

	/*
	 * Copy parameters into message buffer.
	 */
	memset(&msgbuf, 0, sizeof(msgbuf));
	msgbuf.progress = opts->progress;
	msgbuf.timer = opts->timer;
	msgbuf.eta = opts->eta;
	msgbuf.fineta = opts->fineta;
	msgbuf.rate = opts->rate;
	msgbuf.average_rate = opts->average_rate;
	msgbuf.bytes = opts->bytes;
	msgbuf.bufpercent = opts->bufpercent;
	msgbuf.lastwritten = opts->lastwritten;
	msgbuf.rate_limit = opts->rate_limit;
	msgbuf.buffer_size = opts->buffer_size;
	msgbuf.size = opts->size;
	msgbuf.interval = opts->interval;
	msgbuf.width = opts->width;
	msgbuf.height = opts->height;
	msgbuf.width_set_manually = opts->width_set_manually;
	msgbuf.height_set_manually = opts->height_set_manually;

	if (opts->name != NULL) {
		strncpy(msgbuf.name, opts->name, sizeof(msgbuf.name) - 1);	/* flawfinder: ignore */
	}
	if (opts->format != NULL) {
		strncpy(msgbuf.format, opts->format, sizeof(msgbuf.format) - 1);	/* flawfinder: ignore */
	}

	/*
	 * flawfinder rationale: both name and format are explicitly bounded
	 * to 1 less than the size of their buffer and the buffer is \0
	 * terminated by memset() earlier.
	 */

	/*
	 * Get the filename and file stream to use for remote control.
	 */
	memset(control_filename, 0, sizeof(control_filename));
	control_fptr = pv__control_file(control_filename, sizeof(control_filename), getpid(), true);
	if (NULL == control_fptr) {
		pv_error(state, "%s", strerror(errno));
		return 1;
	}

	/*
	 * Write the message buffer to the remote control file, and close
	 * it.
	 */
	if (1 != fwrite(&msgbuf, sizeof(msgbuf), 1, control_fptr)) {
		pv_error(state, "%s", strerror(errno));
		(void) fclose(control_fptr);
		(void) remove(control_filename);
		return 1;
	}

	if (0 != fclose(control_fptr)) {
		pv_error(state, "%s", strerror(errno));
		(void) remove(control_filename);
		return 1;
	}

	/*
	 * Send a SIGUSR2 signal to the remote process, to tell it a message
	 * is ready to read, after clearing our own "SIGUSR2 received" flag.
	 */
	signal_sender = 0;
	(void) pv_sigusr2_received(state, &signal_sender);
	if (kill((pid_t) (opts->remote), SIGUSR2) != 0) {
		pv_error(state, "%u: %s", opts->remote, strerror(errno));
		(void) remove(control_filename);
		return 1;
	}

	debug("%s", "message sent");

	/*
	 * Wait for a signal from the remote process to say it has received
	 * the message.
	 */

	timeout = 1100000;
	received = false;

	while (timeout > 10000 && !received) {
		struct timeval tv;

		memset(&tv, 0, sizeof(tv));
		tv.tv_sec = 0;
		tv.tv_usec = 10000;
		/*@-nullpass@ *//* splint: NULL is OK with select() */
		(void) select(0, NULL, NULL, NULL, &tv);
		/*@+nullpass@ */
		timeout -= 10000;

		if (pv_sigusr2_received(state, &signal_sender)) {
			if (signal_sender == opts->remote) {
				debug("%s", "message received");
				received = true;
			}
		}
	}

	/*
	 * Remove the remote control file.
	 */
	if (0 != remove(control_filename)) {
		pv_error(state, "%s", strerror(errno));
	}

	/*
	 * Return 0 if the message was received.
	 */
	if (received)
		return 0;

	/*@-mustfreefresh@ */
	/*
	 * splint note: the gettext calls made by _() cause memory leak
	 * warnings, but in this case it's unavoidable, and mitigated by the
	 * fact we only translate each string once.
	 */
	pv_error(state, "%u: %s", opts->remote, _("message not received"));
	return 1;
	/*@+mustfreefresh @ */
}


/*
 * Check for a remote control message and, if there is one, replace the
 * current process's options with those being passed in.
 *
 * NB relies on pv_state_set_format() causing the output format to be
 * reparsed.
 */
void pv_remote_check(pvstate_t state)
{
	pid_t signal_sender;
	char control_filename[4096];	 /* flawfinder: ignore */
	FILE *control_fptr;
	struct remote_msg msgbuf;

	/* flawfinder rationale: as above. */

	/*
	 * Return early if a SIGUSR2 signal has not been received.
	 */
	signal_sender = 0;
	if (!pv_sigusr2_received(state, &signal_sender))
		return;

	memset(control_filename, 0, sizeof(control_filename));
	control_fptr = pv__control_file(control_filename, sizeof(control_filename), signal_sender, false);
	if (NULL == control_fptr) {
		pv_error(state, "%s", strerror(errno));
		return;
	}

	/*
	 * Read the message buffer from the remote control file, and close
	 * it.
	 */
	if (1 != fread(&msgbuf, sizeof(msgbuf), 1, control_fptr)) {
		pv_error(state, "%s", strerror(errno));
		(void) fclose(control_fptr);
		return;
	}

	if (0 != fclose(control_fptr)) {
		pv_error(state, "%s", strerror(errno));
		return;
	}

	/*
	 * Send a SIGUSR2 signal to the sending process, to tell it the
	 * message has been received.
	 */
	if (kill(signal_sender, SIGUSR2) != 0) {
		debug("%u: %s", signal_sender, strerror(errno));
	}

	debug("%s", "received remote message");

	pv_state_format_string_set(state, NULL);
	pv_state_name_set(state, NULL);

	msgbuf.name[sizeof(msgbuf.name) - 1] = '\0';
	msgbuf.format[sizeof(msgbuf.format) - 1] = '\0';

	pv_state_set_format(state, msgbuf.progress, msgbuf.timer,
			    msgbuf.eta, msgbuf.fineta, msgbuf.rate,
			    msgbuf.average_rate,
			    msgbuf.bytes, msgbuf.bufpercent,
			    msgbuf.lastwritten, '\0' == msgbuf.name[0] ? NULL : msgbuf.name);

	if (msgbuf.rate_limit > 0)
		pv_state_rate_limit_set(state, msgbuf.rate_limit);
	if (msgbuf.buffer_size > 0) {
		pv_state_target_buffer_size_set(state, msgbuf.buffer_size);
	}
	if (msgbuf.size > 0)
		pv_state_size_set(state, msgbuf.size);
	if (msgbuf.interval > 0)
		pv_state_interval_set(state, msgbuf.interval);
	if (msgbuf.width > 0 && msgbuf.width_set_manually)
		pv_state_width_set(state, msgbuf.width, msgbuf.width_set_manually);
	if (msgbuf.height > 0 && msgbuf.height_set_manually)
		pv_state_height_set(state, msgbuf.height, msgbuf.height_set_manually);
	if (msgbuf.format[0] != '\0')
		pv_state_format_string_set(state, msgbuf.format);
}


/*
 * Initialise remote message reception handling.
 */
void pv_remote_init(void)
{
}


/*
 * Clean up after remote message reception handling.
 */
void pv_remote_fini(void)
{
}

#else				/* !SA_SIGINFO */

/*
 * Dummy stubs for remote control when we don't have SA_SIGINFO.
 */
void pv_remote_init(void)
{
}

void pv_remote_check( /*@unused@ */  __attribute__((unused)) pvstate_t state)
{
}

void pv_remote_fini(void)
{
}

int pv_remote_set(			 /*@unused@ */
			 __attribute__((unused)) opts_t opts, /*@unused@ */  __attribute__((unused)) pvstate_t state)
{
	/*@-mustfreefresh@ *//* splint - see above */
	fprintf(stderr, "%s\n", _("SA_SIGINFO not supported on this system"));
	/*@+mustfreefresh@ */
	return 1;
}

#endif				/* SA_SIGINFO */

/* EOF */
