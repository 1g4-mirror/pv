/*
 * Functions internal to the PV library.  Include "config.h" first.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#ifndef _PV_INTERNAL_H
#define _PV_INTERNAL_H 1

#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Types of display component that make up an output string.
 */
typedef enum {
  PV_COMPONENT_STRING,		/* fixed string */
  PV_COMPONENT_PROGRESS,	/* progress bar, with percentage if known */
  PV_COMPONENT_BYTES,		/* number of bytes transferred */
  PV_COMPONENT_TIMER,		/* elapsed time */
  PV_COMPONENT_RATE,		/* current transfer rate */
  PV_COMPONENT_AVERAGERATE,	/* average transfer rate */
  PV_COMPONENT_ETA,		/* estimated time remaining until completion */
  PV_COMPONENT_FINETA,		/* estimated time of completion */
  PV_COMPONENT_NAME,		/* name prefix */
  PV_COMPONENT_BUFPERCENT,	/* percentage of buffer used */
  PV_COMPONENT_OUTPUTBUF,	/* recent bytes in output buffer */
  PV_COMPONENT__MAX
} pv_display_component;

#define PV_SIZEOF_COMPONENT_STR	1024	/* size of buffer for each component */

#define RATE_GRANULARITY	100000000	 /* nsec between -L rate chunks */
#define RATE_BURST_WINDOW	5	 	 /* rate burst window (multiples of rate) */
#define REMOTE_INTERVAL		100000000	 /* nsec between checks for -R */
#define BUFFER_SIZE		(size_t) 409600	 /* default transfer buffer size */
#define BUFFER_SIZE_MAX		(size_t) 524288	 /* max auto transfer buffer size */
#define MAX_READ_AT_ONCE	(size_t) 524288	 /* max to read() in one go */
#define MAX_WRITE_AT_ONCE	(size_t) 524288	 /* max to write() in one go */
#define TRANSFER_READ_TIMEOUT	0.09L		 /* seconds to time reads out at */
#define TRANSFER_WRITE_TIMEOUT	0.9L		 /* seconds to time writes out at */

#define MAXIMISE_BUFFER_FILL	1

#define PV_SIZEOF_DEFAULT_FORMAT	512
#define PV_SIZEOF_CWD			4096
#define PV_SIZEOF_LASTOUTPUT_BUFFER	256
#define PV_FORMAT_ARRAY_MAX		100
#define PV_SIZEOF_CRS_LOCK_FILE		1024

#define PV_SIZEOF_FILE_FDINFO		4096
#define PV_SIZEOF_FILE_FD		4096
#define PV_SIZEOF_FILE_FDPATH		4096
#define PV_SIZEOF_DISPLAY_NAME		512


/*
 * Structure for data shared between multiple "pv -c" instances.
 */
struct pvcursorstate_s {
	int y_topmost;		/* terminal row of topmost "pv" instance */
	bool tty_tostop_added;	/* whether any instance had to set TOSTOP on the terminal */
};


/*
 * Structure for holding PV internal state. Opaque outside the PV library.
 */
struct pvstate_s {
	/******************
	 * Program status *
	 ******************/
	struct {
		/*@only@*/ char *program_name;	 /* program name for error reporting */
		char cwd[PV_SIZEOF_CWD];	 /* current working directory for relative path */
		int current_input_file;		 /* index of current file being read */
		int exit_status; 		 /* exit status to give (0=OK) */
	} status;

	/***************
	 * Input files *
	 ***************/
	struct {
		unsigned int file_count;	 /* number of input files */
		/*@only@*/ /*@null@*/ char **filename; /* input filenames */
	} files;

	/*******************
	 * Program control *
	 *******************/
	struct {
		bool force;                      /* display even if not on terminal */
		bool cursor;                     /* use cursor positioning */
		bool numeric;                    /* numeric output only */
		bool wait;                       /* wait for data before display */
		bool linemode;                   /* count lines instead of bytes */
		bool bits;			 /* report bits instead of bytes */
		bool null_terminated_lines;      /* lines are null-terminated */
		bool no_display;                 /* do nothing other than pipe data */
		unsigned int skip_errors;        /* skip read errors counter */
		off_t error_skip_block;          /* skip block size, 0 for adaptive */
		bool stop_at_size;               /* set if we stop at "size" bytes */
		bool sync_after_write;           /* set if we sync after every write */
		bool direct_io;                  /* set if O_DIRECT is to be used */
		bool direct_io_changed;          /* set when direct_io is changed */
		bool no_splice;                  /* never use splice() */
		bool discard_input;              /* write nothing to stdout */
		off_t rate_limit;                /* rate limit, in bytes per second */
		size_t target_buffer_size;       /* buffer size (0=default) */
		off_t size;                      /* total size of data */
		double interval;                 /* interval between updates */
		double delay_start;              /* delay before first display */
		pid_t watch_pid;		 /* process to watch fds of */
		int watch_fd;			 /* fd to watch */
		unsigned int average_rate_window; /* time window in seconds for average rate calculations */
		unsigned int width;              /* screen width */
		unsigned int height;             /* screen height */
		bool width_set_manually;	 /* width was set manually, not detected */
		bool height_set_manually;	 /* height was set manually, not detected */
		/*@only@*/ /*@null@*/ char *name;		 /* display name */
		char default_format[PV_SIZEOF_DEFAULT_FORMAT];	 /* default format string */
		/*@only@*/ /*@null@*/ char *format_string;	 /* output format string */
	} control;

	/*******************
	 * Signal handling *
	 *******************/
	struct {
		int old_stderr;		 /* see pv_sig_ttou() */
		bool pv_tty_tostop_added;	 /* whether we had to set TOSTOP on the terminal */
		struct timespec tstp_time;	 /* see pv_sig_tstp() / __cont() */
		struct timespec toffset;	 /* total time spent stopped */
#ifdef SA_SIGINFO
		volatile sig_atomic_t rxusr2;	 /* whether SIGUSR2 was received */
		volatile pid_t sender;		 /* PID of sending process for SIGUSR2 */
#endif
		/* old signal handlers to restore in pv_sig_fini(). */
		struct sigaction old_sigpipe;
		struct sigaction old_sigttou;
		struct sigaction old_sigtstp;
		struct sigaction old_sigcont;
		struct sigaction old_sigwinch;
		struct sigaction old_sigint;
		struct sigaction old_sighup;
		struct sigaction old_sigterm;
#ifdef SA_SIGINFO
		struct sigaction old_sigusr2;
#endif
	} signal;

	/*******************
	 * Transient flags *
	 *******************/
	struct {
		volatile sig_atomic_t reparse_display;	 /* whether to re-check format string */
		volatile sig_atomic_t terminal_resized;	 /* whether we need to get term size again */
		volatile sig_atomic_t trigger_exit;	 /* whether we need to abort right now */
	} flag;

	/*****************
	 * Display state *
	 *****************/
	struct {
		/*@only@*/ /*@null@*/ char *display_buffer;	/* buffer for display string */
		size_t display_buffer_size;	 /* size allocated to display buffer */
		size_t display_string_len;	 /* length of string in display buffer */
		unsigned int prev_screen_width;	 /* screen width last time we were called */
		bool display_visible;		 /* set once anything written to terminal */

		int percentage;			 /* transfer percentage completion */

		long double prev_elapsed_sec;	 /* elapsed sec at which rate last calculated */
		long double prev_rate;		 /* last calculated instantaneous transfer rate */
		long double prev_trans;		 /* bytes transferred since last rate calculation */

		/* Keep track of progress over last intervals to compute current average rate. */
		/*@null@*/ struct {	 /* state at previous intervals (circular buffer) */
			long double elapsed_sec;	/* time since start of transfer */
			off_t total_bytes;		/* amount transferred by that time */
		} *history;
		size_t history_len;		 /* total size of history array */
		int history_interval;		 /* seconds between each history entry */
		size_t history_first;		 /* index of oldest entry */
		size_t history_last;		 /* index of newest entry */
		long double current_avg_rate;    /* current average rate over last history intervals */

		off_t initial_offset;		 /* offset when first opened (when watching fds) */

		size_t lastoutput_length;	 /* number of last-output bytes to show */
		char lastoutput_buffer[PV_SIZEOF_LASTOUTPUT_BUFFER];

		size_t format_segment_count;	 /* number of format string segments */
		struct {	/* format string broken into display components */
			pv_display_component type;	/* type of display component */
			size_t str_start;		/* for strings: start offset */
			size_t str_length;		/* for strings: length */
		} format[PV_FORMAT_ARRAY_MAX];

		struct {	/* display components */
			bool required;				/* true if included in format */
			char content[PV_SIZEOF_COMPONENT_STR];	/* string to display */
			size_t length;				/* number of bytes in string */
		} component[PV_COMPONENT__MAX];

	} display;

	/********************
	 * Cursor/IPC state *
	 ********************/
	struct {
#ifdef HAVE_IPC
		int shmid;		 /* ID of our shared memory segment */
		int pvcount;		 /* number of `pv' processes in total */
		int pvmax;		 /* highest number of `pv's seen */
		/*@keep@*/ /*@null@*/ struct pvcursorstate_s *shared; /* data shared between instances */
		int y_lastread;		 /* last value of _y_top seen */
		int y_offset;		 /* our Y offset from this top position */
		int needreinit;		 /* counter if we need to reinit cursor pos */
		bool noipc;		 /* set if we can't use IPC */
#endif				/* HAVE_IPC */
		int lock_fd;		 /* fd of lockfile, -1 if none open */
		char lock_file[PV_SIZEOF_CRS_LOCK_FILE];
		int y_start;		 /* our initial Y coordinate */
	} cursor;

	/*******************
	 * Transfer state  *
	 *******************/
	/*
	 * The transfer buffer is used for moving data from the input files
	 * to the output when splice() is not available.
	 *
	 * If buffer_size is smaller than pv__target_bufsize, then
	 * pv_transfer() will try to reallocate transfer_buffer to make
	 * buffer_size equal to pv__target_bufsize.
	 *
	 * Data from the input files is read into the buffer; read_position
	 * is the offset in the buffer that we've read data up to.
	 *
	 * Data is written to the output from the buffer, and write_position
	 * is the offset in the buffer that we've written data up to.  It
	 * will always be less than or equal to read_position.
	 */
	struct {
		/*@only@*/ /*@null@*/ char *transfer_buffer;	 /* data transfer buffer */
		size_t buffer_size;		 /* size of buffer */
		size_t read_position;		 /* amount of data in buffer */
		size_t write_position;		 /* buffered data written */

		/*
		 * While reading from a file descriptor we keep track of how
		 * many times in a row we've seen errors
		 * (read_errors_in_a_row), and whether or not we have put a
		 * warning on stderr about read errors on this fd
		 * (read_error_warning_shown).
		 *
		 * Whenever the active file descriptor changes from
		 * last_read_skip_fd, we reset read_errors_in_a_row to 0 and
		 * read_error_warning_shown to false for the new file
		 * descriptor and set last_read_skip_fd to the new fd
		 * number.
		 *
		 * This way, we're treating each input file separately.
		 */
		int last_read_skip_fd;
		off_t read_errors_in_a_row;
		bool read_error_warning_shown;
#ifdef HAVE_SPLICE
		/*
		 * These variables are used to keep track of whether
		 * splice() was used; splice_failed_fd is the file
		 * descriptor that splice() last failed on, so that we don't
		 * keep trying to use it on an fd that doesn't support it,
		 * and splice_used is set to true if splice() was used this
		 * time within pv_transfer().
		 */
		int splice_failed_fd;
		bool splice_used;
#endif
		ssize_t to_write;		 /* max to write this time around */
		ssize_t written;		 /* bytes sent to stdout this time */
	} transfer;
};


struct pvwatchfd_s {
	pid_t watch_pid;		 /* PID to watch */
	int watch_fd;			 /* fd to watch, -1 = not displayed */
#ifdef __APPLE__
#else
	char file_fdinfo[PV_SIZEOF_FILE_FDINFO]; /* path to /proc fdinfo file */
	char file_fd[PV_SIZEOF_FILE_FD];	 /* path to /proc fd symlink  */
#endif
	char file_fdpath[PV_SIZEOF_FILE_FDPATH]; /* path to file that was opened */
	char display_name[PV_SIZEOF_DISPLAY_NAME]; /* name to show on progress bar */
	struct stat sb_fd;		 /* stat of fd symlink */
	struct stat sb_fd_link;		 /* lstat of fd symlink */
	off_t size;			 /* size of whole file, 0 if unknown */
	off_t position;			 /* position last seen at */
	struct timespec start_time;	 /* time we started watching the fd */
	/*@null@*/ pvstate_t state;	 /* state object for flags and display */
};
typedef struct pvwatchfd_s *pvwatchfd_t;

void pv_error(pvstate_t, char *, ...);

int pv_main_loop(pvstate_t);
void pv_display(pvstate_t, long double, off_t, off_t);
ssize_t pv_transfer(pvstate_t, int, bool *, bool *, off_t, long *);
int pv_next_file(pvstate_t, unsigned int, int);
/*@out@*/ const char *pv_current_file_name(pvstate_t);

void pv_write_retry(int, const char *, size_t);

void pv_crs_fini(pvstate_t);
void pv_crs_init(pvstate_t);
void pv_crs_update(pvstate_t, const char *);
#ifdef HAVE_IPC
void pv_crs_needreinit(pvstate_t);
#endif

void pv_sig_allowpause(void);
void pv_sig_checkbg(void);
void pv_sig_nopause(void);

void pv_remote_init(pvstate_t);
void pv_remote_check(pvstate_t);
void pv_remote_fini(pvstate_t);
int pv_remote_set(pvstate_t);

int pv_watchfd_info(pvstate_t, pvwatchfd_t, bool);
bool pv_watchfd_changed(pvwatchfd_t);
off_t pv_watchfd_position(pvwatchfd_t);
int pv_watchpid_scanfds(pvstate_t, pid_t, int *, pvwatchfd_t *, int *);
void pv_watchpid_setname(pvstate_t, pvwatchfd_t);

#ifdef __cplusplus
}
#endif

#endif /* _PV_INTERNAL_H */

/* EOF */
