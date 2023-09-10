/*
 * Functions internal to the PV library.  Include "config.h" first.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * Distributed under the Artistic License v2.0; see `docs/COPYING'.
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

#define PV_DISPLAY_PROGRESS	1
#define PV_DISPLAY_TIMER	2
#define PV_DISPLAY_ETA		4
#define PV_DISPLAY_RATE		8
#define PV_DISPLAY_AVERAGERATE	16
#define PV_DISPLAY_BYTES	32
#define PV_DISPLAY_NAME		64
#define PV_DISPLAY_BUFPERCENT	128
#define PV_DISPLAY_OUTPUTBUF	256
#define PV_DISPLAY_FINETA	512

#define RATE_GRANULARITY	100000000.0L	 /* nsec between -L rate chunks */
#define RATE_BURST_WINDOW	5	 	 /* rate burst window (multiples of rate) */
#define REMOTE_INTERVAL		100000000	 /* nsec between checks for -R */
#define BUFFER_SIZE		409600		 /* default transfer buffer size */
#define BUFFER_SIZE_MAX		524288		 /* max auto transfer buffer size */
#define MAX_READ_AT_ONCE	524288		 /* max to read() in one go */
#define MAX_WRITE_AT_ONCE	524288		 /* max to write() in one go */
#define TRANSFER_READ_TIMEOUT	0.09L		 /* seconds to time reads out at */
#define TRANSFER_WRITE_TIMEOUT	0.9L		 /* seconds to time writes out at */

#define MAXIMISE_BUFFER_FILL	1


typedef struct pvhistory {
	long long   total_bytes;
	long double elapsed_sec;
} pvhistory_t;

#define PV_SIZEOF_DEFAULT_FORMAT	512
#define PV_SIZEOF_CWD			4096
#define PV_SIZEOF_LASTOUTPUT_BUFFER	256
#define PV_SIZEOF_STR_NAME		512
#define PV_SIZEOF_STR_TRANSFERRED	128
#define PV_SIZEOF_STR_BUFPERCENT	128
#define PV_SIZEOF_STR_TIMER		128
#define PV_SIZEOF_STR_RATE		128
#define PV_SIZEOF_STR_AVERAGE_RATE	128
#define PV_SIZEOF_STR_PROGRESS		1024
#define PV_SIZEOF_STR_LASTOUTPUT	512
#define PV_SIZEOF_STR_ETA		128
#define PV_SIZEOF_STR_FINETA		128
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
	/***************
	 * Input files *
	 ***************/
	unsigned int input_file_count;	 /* number of input files */
	/*@only@*/ /*@null@*/ char **input_files; /* input files */

	/*******************
	 * Program control *
	 *******************/
	bool force;                      /* display even if not on terminal */
	bool cursor;                     /* use cursor positioning */
	bool numeric;                    /* numeric output only */
	bool wait;                       /* wait for data before display */
	bool linemode;                   /* count lines instead of bytes */
	bool bits;			 /* report bits instead of bytes */
	bool null_terminated_lines;      /* lines are null-terminated */
	bool no_display;                 /* do nothing other than pipe data */
	unsigned int skip_errors;        /* skip read errors counter */
	unsigned long long error_skip_block; /* skip block size, 0 for adaptive */
	bool stop_at_size;               /* set if we stop at "size" bytes */
	bool sync_after_write;           /* set if we sync after every write */
	bool direct_io;                  /* set if O_DIRECT is to be used */
	bool direct_io_changed;          /* set when direct_io is changed */
	bool no_splice;                  /* never use splice() */
	bool discard_input;              /* write nothing to stdout */
	unsigned long long rate_limit;   /* rate limit, in bytes per second */
	unsigned long long target_buffer_size;  /* buffer size (0=default) */
	unsigned long long size;         /* total size of data */
	double interval;                 /* interval between updates */
	double delay_start;              /* delay before first display */
	unsigned int watch_pid;		 /* process to watch fds of */
	int watch_fd;			 /* fd to watch */
	unsigned int width;              /* screen width */
	unsigned int height;             /* screen height */
	bool width_set_manually;	 /* width was set manually, not detected */
	bool height_set_manually;	 /* height was set manually, not detected */
	/*@only@*/ /*@null@*/ char *name;		 /* display name */
	char default_format[PV_SIZEOF_DEFAULT_FORMAT];	 /* default format string */
	/*@only@*/ /*@null@*/ char *format_string;	 /* output format string */

	/******************
	 * Program status *
	 ******************/
	/*@only@*/ char *program_name;		 /* program name for error reporting */
	char cwd[PV_SIZEOF_CWD];	 /* current working directory for relative path */
	int current_input_file;		 /* index of current file being read */
	int exit_status; 		 /* exit status to give (0=OK) */

	/*******************
	 * Signal handling *
	 *******************/
	int pv_sig_old_stderr;		 /* see pv_sig_ttou() */
	bool pv_tty_tostop_added;	 /* whether we had to set TOSTOP on the terminal */
	struct timespec pv_sig_tstp_time; /* see pv_sig_tstp() / __cont() */
	struct timespec pv_sig_toffset;		 /* total time spent stopped */
	volatile sig_atomic_t pv_sig_newsize;	 /* whether we need to get term size again */
	volatile sig_atomic_t pv_sig_abort;	 /* whether we need to abort right now */
	volatile sig_atomic_t reparse_display;	 /* whether to re-check format string */
	struct sigaction pv_sig_old_sigpipe;
	struct sigaction pv_sig_old_sigttou;
	struct sigaction pv_sig_old_sigtstp;
	struct sigaction pv_sig_old_sigcont;
	struct sigaction pv_sig_old_sigwinch;
	struct sigaction pv_sig_old_sigint;
	struct sigaction pv_sig_old_sighup;
	struct sigaction pv_sig_old_sigterm;

	/*****************
	 * Display state *
	 *****************/
	long percentage;
	long double prev_elapsed_sec;
	long double prev_rate;
	long double prev_trans;

	/* Keep track of progress over last intervals to compute current average rate. */
	/*@null@*/ pvhistory_t *history; /* state at previous intervals (circular buffer) */
	unsigned int history_len;	 /* total size */
	int history_interval;		 /* seconds between each history entry */
	int history_first;
	int history_last;
	long double current_avg_rate;    /* current average rate over last history intervals */
	
	unsigned long long initial_offset;
	/*@only@*/ char *display_buffer;
	long display_buffer_size;
	int lastoutput_length;		 /* number of last-output bytes to show */
	unsigned char lastoutput_buffer[PV_SIZEOF_LASTOUTPUT_BUFFER];
	int prev_width;			 /* screen width last time we were called */
	int prev_length;		 /* length of last string we output */
	char str_name[PV_SIZEOF_STR_NAME];
	char str_transferred[PV_SIZEOF_STR_TRANSFERRED];
	char str_bufpercent[PV_SIZEOF_STR_BUFPERCENT];
	char str_timer[PV_SIZEOF_STR_TIMER];
	char str_rate[PV_SIZEOF_STR_RATE];
	char str_average_rate[PV_SIZEOF_STR_AVERAGE_RATE];
	char str_progress[PV_SIZEOF_STR_PROGRESS];
	char str_lastoutput[PV_SIZEOF_STR_LASTOUTPUT];
	char str_eta[PV_SIZEOF_STR_ETA];
	char str_fineta[PV_SIZEOF_STR_FINETA];
	unsigned long components_used;	 /* bitmask of components used */
	struct {
		const char *string;
		int length;
	} format[PV_FORMAT_ARRAY_MAX];
	bool display_visible;		 /* set once anything written to terminal */

	/********************
	 * Cursor/IPC state *
	 ********************/
#ifdef HAVE_IPC
	int crs_shmid;			 /* ID of our shared memory segment */
	int crs_pvcount;		 /* number of `pv' processes in total */
	int crs_pvmax;			 /* highest number of `pv's seen */
	/*@keep@*/ /*@null@*/ struct pvcursorstate_s *crs_shared; /* data shared between instances */
	int crs_y_lastread;		 /* last value of _y_top seen */
	int crs_y_offset;		 /* our Y offset from this top position */
	int crs_needreinit;		 /* counter if we need to reinit cursor pos */
	bool crs_noipc;			 /* set if we can't use IPC */
#endif				/* HAVE_IPC */
	int crs_lock_fd;		 /* fd of lockfile, -1 if none open */
	char crs_lock_file[PV_SIZEOF_CRS_LOCK_FILE];
	int crs_y_start;		 /* our initial Y coordinate */

	/*******************
	 * Transfer state  *
	 *******************/
	/*
	 * The transfer buffer is used for moving data from the input files
	 * to the output when splice() is not available.
	 *
	 * If buffer_size is smaller than pv__target_bufsize, then
	 * pv_transfer will try to reallocate transfer_buffer to make
	 * buffer_size equal to pv__target_bufsize.
	 *
	 * Data from the input files is read into the buffer; read_position
	 * is the offset in the buffer that we've read data up to.
	 *
	 * Data is written to the output from the buffer, and write_position
	 * is the offset in the buffer that we've written data up to.  It
	 * will always be less than or equal to read_position.
	 */
	unsigned char *transfer_buffer;	 /* data transfer buffer */
	unsigned long long buffer_size;	 /* size of buffer */
	unsigned long read_position;	 /* amount of data in buffer */
	unsigned long write_position;	 /* buffered data written */

	/*
	 * While reading from a file descriptor we keep track of how many
	 * times in a row we've seen errors (read_errors_in_a_row), and
	 * whether or not we have put a warning on stderr about read errors
	 * on this fd (read_error_warning_shown).
	 *
	 * Whenever the active file descriptor changes from
	 * last_read_skip_fd, we reset read_errors_in_a_row and
	 * read_error_warning_shown to 0 for the new file descriptor and set
	 * last_read_skip_fd to the new fd number.
	 *
	 * This way, we're treating each input file separately.
	 */
	int last_read_skip_fd;
	unsigned long read_errors_in_a_row;
	int read_error_warning_shown;
#ifdef HAVE_SPLICE
	/*
	 * These variables are used to keep track of whether splice() was
	 * used; splice_failed_fd is the file descriptor that splice() last
	 * failed on, so that we don't keep trying to use it on an fd that
	 * doesn't support it, and splice_used is set to 1 if splice() was
	 * used this time within pv_transfer().
	 */
	int splice_failed_fd;
	int splice_used;
#endif
	long to_write;			 /* max to write this time around */
	long written;			 /* bytes sent to stdout this time */
};


struct pvwatchfd_s {
	unsigned int watch_pid;		 /* PID to watch */
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
	unsigned long long size;	 /* size of whole file, 0 if unknown */
	long long position;		 /* position last seen at */
	struct timespec start_time;	 /* time we started watching the fd */
};
typedef struct pvwatchfd_s *pvwatchfd_t;

void pv_error(pvstate_t, char *, ...);

int pv_main_loop(pvstate_t);
void pv_display(pvstate_t, long double, long long, long long);
long pv_transfer(pvstate_t, int, int *, int *, unsigned long long, long *);
void pv_set_buffer_size(unsigned long long, int);
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

int pv_watchfd_info(pvstate_t, pvwatchfd_t, int);
int pv_watchfd_changed(pvwatchfd_t);
long long pv_watchfd_position(pvwatchfd_t);
int pv_watchpid_scanfds(pvstate_t, pvstate_t, unsigned int, int *, pvwatchfd_t *, pvstate_t *, int *);
void pv_watchpid_setname(pvstate_t, pvwatchfd_t);

#ifdef __cplusplus
}
#endif

#endif /* _PV_INTERNAL_H */

/* EOF */
