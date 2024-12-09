/*
 * Functions internal to the PV library.  Include "config.h" first.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023-2024 Andrew Wood
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

#define RATE_GRANULARITY	100000000	 /* nsec between -L rate chunks */
#define RATE_BURST_WINDOW	5	 	 /* rate burst window (multiples of rate) */
#define REMOTE_INTERVAL		100000000	 /* nsec between checks for -R */
#define BUFFER_SIZE		(size_t) 409600	 /* default transfer buffer size */
#define BUFFER_SIZE_MAX		(size_t) 524288	 /* max auto transfer buffer size */
#define MAX_READ_AT_ONCE	(size_t) 524288	 /* max to read() in one go */
#define MAX_WRITE_AT_ONCE	(size_t) 524288	 /* max to write() in one go */
#define TRANSFER_READ_TIMEOUT	0.09L		 /* seconds to time reads out at */
#define TRANSFER_WRITE_TIMEOUT	0.9L		 /* seconds to time writes out at */
#define MAX_LINE_POSITIONS	100000		 /* number of lines to remember positions of */

#define MAXIMISE_BUFFER_FILL	1

#define PV_SIZEOF_DEFAULT_FORMAT	512
#define PV_SIZEOF_CWD			4096
#define PV_SIZEOF_LASTWRITTEN_BUFFER	256
#define PV_SIZEOF_PREVLINE_BUFFER	1024
#define PV_FORMAT_ARRAY_MAX		100
#define PV_SIZEOF_CRS_LOCK_FILE		1024

#define PV_SIZEOF_FILE_FDINFO		4096
#define PV_SIZEOF_FILE_FD		4096
#define PV_SIZEOF_FILE_FDPATH		4096
#define PV_SIZEOF_DISPLAY_NAME		512

#define PV_DISPLAY_WINDOWTITLE		1
#define PV_DISPLAY_PROCESSTITLE		2


/*
 * Structure for data shared between multiple "pv -c" instances.
 */
struct pvcursorstate_s {
	int y_topmost;		/* terminal row of topmost "pv" instance */
	bool tty_tostop_added;	/* whether any instance had to set TOSTOP on the terminal */
};

/*
 * Types of transfer count - bytes, decimal bytes or lines.
 */
typedef enum {
	PV_TRANSFERCOUNT_BYTES,
	PV_TRANSFERCOUNT_DECBYTES,
	PV_TRANSFERCOUNT_LINES
} pvtransfercount_t;


/*
 * Structure for holding PV internal state. Opaque outside the PV library.
 *
 * In general, members are ordered by size, to minimise padding.
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
		/*@only@*/ /*@null@*/ char **filename; /* input filenames */
		unsigned int file_count;	 /* number of input files */
	} files;

	/*******************
	 * Program control *
	 *******************/
	struct {
		char default_format[PV_SIZEOF_DEFAULT_FORMAT];	 /* default format string */
		double interval;                 /* interval between updates */
		double delay_start;              /* delay before first display */
		/*@only@*/ /*@null@*/ char *name;		 /* display name */
		/*@only@*/ /*@null@*/ char *format_string;	 /* output format string */
		/*@only@*/ /*@null@*/ char *extra_format_string; /* extra format string */
		/*@null@*/ char *output_name;    /* name of the output, for diagnostics */
		off_t error_skip_block;          /* skip block size, 0 for adaptive */
		off_t rate_limit;                /* rate limit, in bytes per second */
		size_t target_buffer_size;       /* buffer size (0=default) */
		off_t size;                      /* total size of data */
		pid_t watch_pid;		 /* process to watch fds of */
		unsigned int skip_errors;        /* skip read errors counter */
		int watch_fd;			 /* fd to watch */
		int output_fd;                   /* fd to write output to */
		unsigned int average_rate_window; /* time window in seconds for average rate calculations */
		unsigned int history_interval;	 /* seconds between each average rate calc history entry */
		unsigned int width;              /* screen width */
		unsigned int height;             /* screen height */
		unsigned int extra_displays;	 /* bitmask of extra display destinations */
		bool force;                      /* display even if not on terminal */
		bool cursor;                     /* use cursor positioning */
		bool numeric;                    /* numeric output only */
		bool wait;                       /* wait for data before display */
		bool rate_gauge;                 /* if size unknown, show rate vs max rate */
		bool linemode;                   /* count lines instead of bytes */
		bool bits;			 /* report bits instead of bytes */
		bool decimal_units;		 /* use decimal prefixes */
		bool null_terminated_lines;      /* lines are null-terminated */
		bool no_display;                 /* do nothing other than pipe data */
		bool stop_at_size;               /* set if we stop at "size" bytes */
		bool sync_after_write;           /* set if we sync after every write */
		bool direct_io;                  /* set if O_DIRECT is to be used */
		bool direct_io_changed;          /* set when direct_io is changed */
		bool no_splice;                  /* never use splice() */
		bool discard_input;              /* write nothing to stdout */
		bool show_stats;		 /* show statistics on exit */
		bool can_display_utf8;		 /* whether UTF-8 output is permitted */
		bool width_set_manually;	 /* width was set manually, not detected */
		bool height_set_manually;	 /* height was set manually, not detected */
	} control;

	/*******************
	 * Signal handling *
	 *******************/
	struct {
		/* old signal handlers to restore in pv_sig_fini(). */
		struct sigaction old_sigpipe;
		struct sigaction old_sigttou;
		struct sigaction old_sigtstp;
		struct sigaction old_sigcont;
		struct sigaction old_sigwinch;
		struct sigaction old_sigint;
		struct sigaction old_sighup;
		struct sigaction old_sigterm;
#ifdef PV_REMOTE_CONTROL
		struct sigaction old_sigusr2;
#endif
		struct sigaction old_sigalrm;
		struct timespec tstp_time;	 /* see pv_sig_tstp() / __cont() */
		struct timespec toffset;	 /* total time spent stopped */
#ifdef PV_REMOTE_CONTROL
		volatile sig_atomic_t rxusr2;	 /* whether SIGUSR2 was received */
		volatile pid_t sender;		 /* PID of sending process for SIGUSR2 */
#endif
	} signal;

	/*******************
	 * Transient flags *
	 *******************/
	struct {
		volatile sig_atomic_t reparse_display;	 /* whether to re-check format string */
		volatile sig_atomic_t terminal_resized;	 /* whether we need to get term size again */
		volatile sig_atomic_t trigger_exit;	 /* whether we need to abort right now */
		volatile sig_atomic_t clear_tty_tostop_on_exit;	/* whether to clear tty TOSTOP on exit */
		volatile sig_atomic_t suspend_stderr;	 /* whether writing to stderr is suspended */
		volatile sig_atomic_t skip_next_sigcont; /* whether to ignore the next SIGCONT */
		volatile sig_atomic_t pipe_closed;	 /* whether the output pipe was closed */
	} flag;

	/*****************
	 * Display state *
	 *****************/
	struct pvdisplay_s {

		struct pvdisplay_segment_s {	/* format string broken into segments */
			/* See pv__format_init() for more details. */
			int type;			/* component type, -1 for static string */
			size_t chosen_size;		/* "n" from %<n>A, or 0 */
			size_t offset;			/* start offset of this segment */
			size_t bytes;			/* length of segment in bytes */
			size_t width;			/* displayed width of segment */
		} format[PV_FORMAT_ARRAY_MAX];

		/* The last-written "n" bytes. */
		char lastwritten_buffer[PV_SIZEOF_LASTWRITTEN_BUFFER];

		/* The most recently output complete line. */
		char previous_line[PV_SIZEOF_PREVLINE_BUFFER];
		/* The line being received now. */
		char next_line[PV_SIZEOF_PREVLINE_BUFFER];

		/*@only@*/ /*@null@*/ char *display_buffer;	/* buffer for display string */
		size_t display_buffer_size;	 /* size allocated to display buffer */
		size_t display_string_bytes;	 /* byte length of string in display buffer */
		size_t display_string_width;	 /* displayed width of string in display buffer */
		off_t initial_offset;		 /* offset when first opened (when watching fds) */
		size_t lastwritten_bytes;	 /* largest number of last-written bytes to show */
		size_t next_line_len;		 /* length of currently receiving line so far */

		size_t format_segment_count;	 /* number of format string segments */

		pvtransfercount_t count_type;	 /* type of count for transfer, rate, etc */

		unsigned int prev_screen_width;	 /* screen width last time we were called */

		bool showing_timer;		 /* set if showing timer */
		bool showing_bytes;		 /* set if showing byte/line count */
		bool showing_rate;		 /* set if showing transfer rate */
		bool showing_last_written;	 /* set if displaying the last few bytes written */
		bool showing_previous_line;	 /* set if displaying the previously output line */

		bool final_update;		 /* set internally on the final update */
		bool display_visible;		 /* set once anything written to terminal */

	} display;

	/* Extra display for alternate outputs like a window title. */
	struct pvdisplay_s extra_display;

	/************************************
	 * Calculated state of the transfer *
	 ************************************/
	struct {
		long double transfer_rate;	 /* calculated transfer rate */
		long double average_rate;	 /* calculated average transfer rate */

		long double prev_elapsed_sec;	 /* elapsed sec at which rate last calculated */
		long double prev_rate;		 /* last calculated instantaneous transfer rate */
		long double prev_trans;		 /* amount transferred since last rate calculation */
		long double current_avg_rate;    /* current average rate over last history intervals */

		long double rate_min;		 /* minimum measured transfer rate */
		long double rate_max;		 /* maximum measured transfer rate */
		long double rate_sum;		 /* sum of all measured transfer rates */
		long double ratesquared_sum;	 /* sum of the squares of each transfer rate */
		unsigned long measurements_taken; /* how many times the rate was measured */

		/* Keep track of progress over last intervals to compute current average rate. */
		/*@null@*/ struct {	 /* state at previous intervals (circular buffer) */
			long double elapsed_sec;	/* time since start of transfer */
			off_t transferred;		/* amount transferred by that time */
		} *history;
		size_t history_len;		 /* total size of history array */
		size_t history_first;		 /* index of oldest entry */
		size_t history_last;		 /* index of newest entry */

		off_t prev_transferred;		 /* total amount transferred when called last time */

		int percentage;			 /* transfer percentage completion */
	} calc;

	/********************
	 * Cursor/IPC state *
	 ********************/
	struct {
		char lock_file[PV_SIZEOF_CRS_LOCK_FILE];
#ifdef HAVE_IPC
		/*@keep@*/ /*@null@*/ struct pvcursorstate_s *shared; /* data shared between instances */
		int shmid;		 /* ID of our shared memory segment */
		int pvcount;		 /* number of `pv' processes in total */
		int pvmax;		 /* highest number of `pv's seen */
		int y_lastread;		 /* last value of _y_top seen */
		int y_offset;		 /* our Y offset from this top position */
		int needreinit;		 /* counter if we need to reinit cursor pos */
#endif				/* HAVE_IPC */
		int lock_fd;		 /* fd of lockfile, -1 if none open */
		int y_start;		 /* our initial Y coordinate */
#ifdef HAVE_IPC
		bool noipc;		 /* set if we can't use IPC */
#endif				/* HAVE_IPC */
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
		long double elapsed_seconds;	 /* how long we have been transferring data for */
		/*@only@*/ /*@null@*/ char *transfer_buffer;	 /* data transfer buffer */
		size_t buffer_size;		 /* size of buffer */
		size_t read_position;		 /* amount of data in buffer */
		size_t write_position;		 /* buffered data written */

		ssize_t to_write;		 /* max to write this time around */
		ssize_t written;		 /* bytes sent to stdout this time */

		size_t written_but_not_consumed; /* bytes in the output pipe, unread */

		off_t total_written;		 /* total bytes or lines written */
		off_t transferred;		 /* amount transferred (written - unconsumed) */

		/* Keep track of line positions to backtrack written_but_not_consumed. */
		/*@only@*/ /*@null@*/ off_t *line_positions; /* line separator write positions (circular buffer) */
		size_t line_positions_capacity;	 /* total size of line position array */
		size_t line_positions_length;	 /* number of positions stored in array */
		size_t line_positions_head;	 /* index to use for next position */
		off_t last_output_position;	 /* write position last sent to output */

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
		off_t read_errors_in_a_row;
		int last_read_skip_fd;
		/* read_error_warning_shown is defined below. */
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
		bool read_error_warning_shown;
	} transfer;
};

typedef struct pvdisplay_s *pvdisplay_t;
typedef struct pvdisplay_segment_s *pvdisplay_segment_t;


/* Pointer to a formatter function. */
typedef size_t (*pvdisplay_formatter_t)(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);


/*
 * Structure defining a format string sequence following a %.
 */
struct pvdisplay_component_s {
	/*@null@ */ const char *match;			/* string to match */
	/*@null@ */ pvdisplay_formatter_t function;	/* function to call */
	bool dynamic;			 /* whether it can scale with screen size */
};


struct pvwatchfd_s {
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
	pid_t watch_pid;		 /* PID to watch */
	int watch_fd;			 /* fd to watch, -1 = not displayed */
};
typedef struct pvwatchfd_s *pvwatchfd_t;

void pv_error(pvstate_t, char *, ...);

int pv_main_loop(pvstate_t);
void pv_calculate_transfer_rate(pvstate_t, bool);

long pv_bound_long(long, long, long);
long pv_seconds_remaining(const off_t, const off_t, const long double);
void pv_si_prefix(long double *, char *, const long double, pvtransfercount_t);
void pv_describe_amount(char *, size_t, char *, long double, char *, char *, pvtransfercount_t);

size_t pv_formatter_segmentcontent(char *, pvdisplay_segment_t, char *, size_t, size_t);

/*
 * Formatting functions.
 *
 * Each formatting function takes a state, the current display, and the
 * segment it's for; it also takes a buffer, with a particular size, and an
 * offset at which to start writing to the buffer.
 *
 * If the component is dynamically sized (such as a progress bar with no
 * chosen_size constraint), the segment's "width" is expected to have
 * already been populated by the caller, with the target width.
 *
 * The function writes the appropriate string to the buffer at the offset,
 * and updates the segment's "offset" and "bytes".  The number of bytes
 * written ("bytes") is also returned; it will be 0 if the string would not
 * fit into the buffer.
 *
 * The caller is expected to update the segment's "width".
 *
 * If called with a buffer size of 0, only the side effects occur (such as
 * setting flags like display->showing_timer).
 */
size_t pv_formatter_progress(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_progress_bar_only(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_progress_amount_only(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_timer(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_eta(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_fineta(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_rate(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_average_rate(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_bytes(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_buffer_percent(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_last_written(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_previous_line(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);
size_t pv_formatter_name(pvstate_t, pvdisplay_t, pvdisplay_segment_t, char *, size_t, size_t);

bool pv_format(pvstate_t, /*@null@*/ const char *, pvdisplay_t, bool, bool);
void pv_display(pvstate_t, bool);
ssize_t pv_transfer(pvstate_t, int, bool *, bool *, off_t, long *);
int pv_next_file(pvstate_t, unsigned int, int);
/*@keep@*/ const char *pv_current_file_name(pvstate_t);

void pv_write_retry(int, const char *, size_t);
void pv_tty_write(pvstate_t, const char *, size_t);

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
