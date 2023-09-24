/*
 * Parse command-line options.
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
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif


void display_help(void);
void display_version(void);


/*
 * Free an opts_t object.
 */
void opts_free( /*@only@ */ opts_t opts)
{
	if (!opts)
		return;
	/*@-keeptrans@ */
	/*
	 * splint note: we're explicitly being handed the "opts" object to
	 * free it, so the previously "kept" internally allocated buffers
	 * are now ours to free.
	 */
	if (NULL != opts->name)
		free(opts->name);
	if (NULL != opts->format)
		free(opts->format);
	if (NULL != opts->pidfile)
		free(opts->pidfile);
	if (NULL != opts->argv)
		free(opts->argv);
	/*@+keeptrans@ */
	free(opts);
}

/*
 * Add a filename to the list of non-option arguments, returning false on
 * error.  The filename is not copied - the pointer is stored.
 */
bool opts_add_file(opts_t opts, const char *filename)
{
	/*@-branchstate@ */
	if ((opts->argc >= opts->argv_length) || (NULL == opts->argv)) {
		opts->argv_length = opts->argc + 10;
		/*@-keeptrans@ */
		opts->argv = realloc(opts->argv, opts->argv_length * sizeof(char *));
		/*@+keeptrans@ */
		if (NULL == opts->argv) {
			fprintf(stderr, "%s: %s\n", opts->program_name, strerror(errno));
			opts->argv_length = 0;
			opts->argc = 0;
			return false;
		}
	}
	/*@+branchstate@ */

	/*
	 * splint notes: we turned off "branchstate" above because depending
	 * on whether we have to extend the array, we change argv from
	 * "keep" to "only", which is also why we turned off "keeptrans";
	 * there doesn't seem to be a clean way to tell splint that everyone
	 * else should not touch argv but we're allowed to reallocate it and
	 * so is opts_parse.
	 */

	opts->argv[opts->argc++] = filename;

	return true;
}


/*
 * Parse the given command-line arguments into an opts_t object, handling
 * "help" and "version" options internally.
 *
 * Returns an opts_t, or NULL on error.
 *
 * Note that the contents of *argv[] (i.e. the command line parameters)
 * aren't copied anywhere, just the pointers are copied, so make sure the
 * command line data isn't overwritten or argv[1] free()d or whatever.
 */
/*@null@ */
/*@only@ */
opts_t opts_parse(unsigned int argc, char **argv)
{
#ifdef HAVE_GETOPT_LONG
	/*@-nullassign@ */
	/* splint rationale: NULL is allowed for "flags" in long options. */
	struct option long_options[] = {
		{ "help", 0, NULL, (int) 'h' },
		{ "version", 0, NULL, (int) 'V' },
		{ "progress", 0, NULL, (int) 'p' },
		{ "timer", 0, NULL, (int) 't' },
		{ "eta", 0, NULL, (int) 'e' },
		{ "fineta", 0, NULL, (int) 'I' },
		{ "rate", 0, NULL, (int) 'r' },
		{ "average-rate", 0, NULL, (int) 'a' },
		{ "bytes", 0, NULL, (int) 'b' },
		{ "bits", 0, NULL, (int) '8' },
		{ "buffer-percent", 0, NULL, (int) 'T' },
		{ "last-written", 1, NULL, (int) 'A' },
		{ "force", 0, NULL, (int) 'f' },
		{ "numeric", 0, NULL, (int) 'n' },
		{ "quiet", 0, NULL, (int) 'q' },
		{ "cursor", 0, NULL, (int) 'c' },
		{ "wait", 0, NULL, (int) 'W' },
		{ "delay-start", 1, NULL, (int) 'D' },
		{ "size", 1, NULL, (int) 's' },
		{ "line-mode", 0, NULL, (int) 'l' },
		{ "null", 0, NULL, (int) '0' },
		{ "interval", 1, NULL, (int) 'i' },
		{ "width", 1, NULL, (int) 'w' },
		{ "height", 1, NULL, (int) 'H' },
		{ "name", 1, NULL, (int) 'N' },
		{ "format", 1, NULL, (int) 'F' },
		{ "rate-limit", 1, NULL, (int) 'L' },
		{ "buffer-size", 1, NULL, (int) 'B' },
		{ "no-splice", 0, NULL, (int) 'C' },
		{ "skip-errors", 0, NULL, (int) 'E' },
		{ "error-skip-block", 1, NULL, (int) 'Z' },
		{ "stop-at-size", 0, NULL, (int) 'S' },
		{ "sync", 0, NULL, (int) 'Y' },
		{ "direct-io", 0, NULL, (int) 'K' },
		{ "discard", 0, NULL, (int) 'X' },
		{ "remote", 1, NULL, (int) 'R' },
		{ "pidfile", 1, NULL, (int) 'P' },
		{ "watchfd", 1, NULL, (int) 'd' },
		{ "average-rate-window", 1, NULL, (int) 'm' },
#ifdef ENABLE_DEBUGGING
		{ "debug", 1, NULL, (int) '!' },
#endif				/* ENABLE_DEBUGGING */
		{ NULL, 0, NULL, 0 }
	};
	/*@+nullassign@ */
	int option_index = 0;
#endif				/* HAVE_GETOPT_LONG */
	char *short_options = "hVpteIrab8TA:fnqcWD:s:l0i:w:H:N:F:L:B:CEZ:SYKXR:P:d:m:"
#ifdef ENABLE_DEBUGGING
	    "!:"
#endif
	    ;
	int c, numopts;
	unsigned int check_pid, parse_pid;
	int check_fd, parse_fd;
	opts_t opts;
	char *leafptr;

	opts = calloc(1, sizeof(*opts));
	if (!opts) {
		/*@-mustfreefresh@ */
		/*
		 * splint note: the gettext calls made by _() cause memory
		 * leak warnings, but in this case it's unavoidable, and
		 * mitigated by the fact we only translate each string once.
		 */
		fprintf(stderr, "%s: %s: %s\n", argv[0], _("option structure allocation failed"), strerror(errno));
		return NULL;
		/*@+mustfreefresh@ */
	}

	leafptr = strrchr(argv[0], '/');
	if (NULL != leafptr) {
		opts->program_name = 1 + leafptr;
	} else {
		leafptr = argv[0];	    /* avoid splint "keep" warnings */
		opts->program_name = leafptr;
	}

	opts->argc = 0;
	opts->argv = calloc((size_t) (argc + 1), sizeof(char *));
	if (NULL == opts->argv) {
		/*@-mustfreefresh@ */
		/* splint note: as above. */
		fprintf(stderr, "%s: %s: %s\n", opts->program_name,
			_("option structure argv allocation failed"), strerror(errno));
		free(opts);		    /* can't call opts_free as argv is not set */
		return NULL;
		/*@+mustfreefresh@ */
	}
	opts->argv_length = 1 + argc;

	numopts = 0;

	opts->interval = 1;
	opts->delay_start = 0;
	opts->watch_pid = 0;
	opts->watch_fd = -1;
	opts->average_rate_window = 30;

	opts->width_set_manually = false;
	opts->height_set_manually = false;

	do {
#ifdef HAVE_GETOPT_LONG
		c = getopt_long((int) argc, argv, short_options, long_options, &option_index);	/* flawfinder: ignore */
#else
		c = getopt((int) argc, argv, short_options);	/* flawfinder: ignore */
#endif
		/*
		 * flawfinder rationale: we have to pass argv to getopt, and
		 * limiting the argument sizes would be impractical and
		 * cumbersome (and likely lead to more bugs); so we have to
		 * trust the system getopt to not have internal buffer
		 * overflows.
		 */

		if (c < 0)
			continue;

		/*
		 * Check that any numeric arguments are of the right type.
		 */
		switch (c) {
		case 's':
			/* "-s @" is valid, so allow it. */
			if ('@' == *optarg)
				break;
			/* falls through */
			/*@fallthrough@ */
		case 'A':
			/*@fallthrough@ */
		case 'w':
			/*@fallthrough@ */
		case 'H':
			/*@fallthrough@ */
		case 'L':
			/*@fallthrough@ */
		case 'B':
			/*@fallthrough@ */
		case 'R':
			/*@fallthrough@ */
		case 'm':
			/*@fallthrough@ */
		case 'Z':
			if (!pv_getnum_check(optarg, PV_NUMTYPE_INTEGER)) {
				/*@-mustfreefresh@ *//* see above */
				fprintf(stderr, "%s: -%c: %s\n", opts->program_name, c, _("integer argument expected"));
				opts_free(opts);
				return NULL;
				/*@+mustfreefresh@ */
			}
			break;
		case 'i':
			/*@fallthrough@ */
		case 'D':
			if (!pv_getnum_check(optarg, PV_NUMTYPE_DOUBLE)) {
				/*@-mustfreefresh@ *//* see above */
				fprintf(stderr, "%s: -%c: %s\n", opts->program_name, c, _("numeric argument expected"));
				opts_free(opts);
				return NULL;
				/*@+mustfreefresh@ */
			}
			break;
		case 'd':
			if (sscanf(optarg, "%u:%d", &check_pid, &check_fd)
			    < 1) {
				/*@-mustfreefresh@ *//* see above */
				fprintf(stderr, "%s: -%c: %s\n",
					opts->program_name, c, _("process ID or pid:fd pair expected"));
				opts_free(opts);
				return NULL;
				/*@+mustfreefresh@ */
			}
			if (check_pid < 1) {
				/*@-mustfreefresh@ *//* see above */
				fprintf(stderr, "%s: -%c: %s\n", opts->program_name, c, _("invalid process ID"));
				opts_free(opts);
				return NULL;
				/*@+mustfreefresh@ */
			}
			break;
		default:
			break;
		}

		/*
		 * Parse each command line option.
		 */
		switch (c) {
		case 'h':
			display_help();
			opts->do_nothing = true;
			return opts;	    /* early return */
		case 'V':
			display_version();
			opts->do_nothing = true;
			return opts;	    /* early return */
		case 'p':
			opts->progress = true;
			numopts++;
			break;
		case 't':
			opts->timer = true;
			numopts++;
			break;
		case 'I':
			opts->fineta = true;
			numopts++;
			break;
		case 'e':
			opts->eta = true;
			numopts++;
			break;
		case 'r':
			opts->rate = true;
			numopts++;
			break;
		case 'a':
			opts->average_rate = true;
			numopts++;
			break;
		case 'b':
			opts->bytes = true;
			numopts++;
			break;
		case '8':
			opts->bytes = true;
			opts->bits = true;
			numopts++;
			break;
		case 'T':
			opts->bufpercent = true;
			numopts++;
			opts->no_splice = true;
			break;
		case 'A':
			opts->lastwritten = (size_t) pv_getnum_count(optarg);
			numopts++;
			opts->no_splice = true;
			break;
		case 'f':
			opts->force = true;
			break;
		case 'n':
			opts->numeric = true;
			numopts++;
			break;
		case 'q':
			opts->no_display = true;
			numopts++;
			break;
		case 'c':
			opts->cursor = true;
			break;
		case 'W':
			opts->wait = true;
			break;
		case 'D':
			opts->delay_start = pv_getnum_interval(optarg);
			break;
		case 's':
			/* Permit "@<filename>" as well as just a number. */
			if ('@' == *optarg) {
				const char *size_file = 1 + optarg;
				struct stat sb;
				int rc;

				rc = 0;
				memset(&sb, 0, sizeof(sb));
				rc = stat(size_file, &sb);
				if (0 == rc) {
					opts->size = (off_t) (sb.st_size);
				} else {
					/*@-mustfreefresh@ *//* see above */
					fprintf(stderr, "%s: %s %s: %s\n",
						opts->program_name,
						_("failed to stat file"), size_file, strerror(errno));
					opts_free(opts);
					return NULL;
					/*@+mustfreefresh@ */
				}
			} else {
				opts->size = pv_getnum_size(optarg);
			}
			break;
		case 'l':
			opts->linemode = true;
			break;
		case '0':
			opts->null_terminated_lines = true;
			opts->linemode = true;
			break;
		case 'i':
			opts->interval = pv_getnum_interval(optarg);
			break;
		case 'w':
			opts->width = pv_getnum_count(optarg);
			opts->width_set_manually = opts->width == 0 ? false : true;
			break;
		case 'H':
			opts->height = pv_getnum_count(optarg);
			opts->height_set_manually = opts->height == 0 ? false : true;
			break;
		case 'N':
			opts->name = pv_strdup(optarg);
			if (NULL == opts->name) {
				fprintf(stderr, "%s: -N: %s\n", opts->program_name, strerror(errno));
				opts_free(opts);
				return NULL;
			}
			break;
		case 'L':
			opts->rate_limit = pv_getnum_size(optarg);
			break;
		case 'B':
			opts->buffer_size = (size_t) pv_getnum_size(optarg);
			opts->no_splice = true;
			break;
		case 'C':
			opts->no_splice = true;
			break;
		case 'E':
			opts->skip_errors++;
			break;
		case 'Z':
			opts->error_skip_block = pv_getnum_size(optarg);
			break;
		case 'S':
			opts->stop_at_size = true;
			break;
		case 'Y':
			opts->sync_after_write = true;
			break;
		case 'K':
			opts->direct_io = true;
			break;
		case 'X':
			opts->discard_input = true;
			opts->no_splice = true;
			break;
		case 'R':
			opts->remote = pv_getnum_count(optarg);
			break;
		case 'P':
			opts->pidfile = pv_strdup(optarg);
			if (NULL == opts->pidfile) {
				fprintf(stderr, "%s: -P: %s\n", opts->program_name, strerror(errno));
				opts_free(opts);
				return NULL;
			}
			break;
		case 'F':
			opts->format = pv_strdup(optarg);
			if (NULL == opts->format) {
				fprintf(stderr, "%s: -F: %s\n", opts->program_name, strerror(errno));
				opts_free(opts);
				return NULL;
			}
			break;
		case 'd':
			parse_pid = 0;
			parse_fd = -1;
			/* No syntax check here, already done earlier */
			(void) sscanf(optarg, "%u:%d", &parse_pid, &parse_fd);
			opts->watch_pid = (pid_t) parse_pid;
			opts->watch_fd = parse_fd;
			break;
		case 'm':
			opts->average_rate_window = pv_getnum_count(optarg);
			break;
#ifdef ENABLE_DEBUGGING
		case '!':
			debugging_output_destination(optarg);
			break;
#endif				/* ENABLE_DEBUGGING */
		default:
			/*@-mustfreefresh@ *//* see above */
			/*@-formatconst@ */
#ifdef HAVE_GETOPT_LONG
			fprintf(stderr, _("Try `%s --help' for more information."), opts->program_name);
#else
			fprintf(stderr, _("Try `%s -h' for more information."), opts->program_name);
#endif
			/*@+formatconst@ */
			/*
			 * splint note: formatconst is warning about the use
			 * of a non constant (translatable) format string;
			 * this is unavoidable here and the only attack
			 * vector is through the message catalogue.
			 */
			fprintf(stderr, "\n");
			opts_free(opts);
			opts = NULL;
			return NULL;	    /* early return */
			/*@+mustfreefresh@ */
		}

	} while (c != -1);

	/*
	 * splint thinks we can reach here after opts_free() and opts=NULL
	 * above, so explicitly return here if opts was set to NULL.
	 */
	if (NULL == opts)
		return NULL;

	if (0 != opts->watch_pid) {
		if (opts->linemode || opts->null_terminated_lines || opts->stop_at_size
		    || (opts->skip_errors > 0) || (opts->buffer_size > 0)
		    || (opts->rate_limit > 0)) {
			/*@-mustfreefresh@ *//* see above */
			fprintf(stderr, "%s: %s\n", opts->program_name,
				_("cannot use line mode or transfer modifier options when watching file descriptors"));
			opts_free(opts);
			return NULL;
			/*@+mustfreefresh@ */
		}

		if (opts->cursor) {
			/*@-mustfreefresh@ *//* see above */
			fprintf(stderr, "%s: %s\n", opts->program_name,
				_("cannot use cursor positioning when watching file descriptors"));
			opts_free(opts);
			return NULL;
			/*@+mustfreefresh@ */
		}

		if (0 != opts->remote) {
			/*@-mustfreefresh@ *//* see above */
			fprintf(stderr, "%s: %s\n", opts->program_name,
				_("cannot use remote control when watching file descriptors"));
			opts_free(opts);
			return NULL;
			/*@+mustfreefresh@ */
		}

		if (optind < (int) argc) {
			/*@-mustfreefresh@ *//* see above */
			fprintf(stderr, "%s: %s\n", opts->program_name,
				_("cannot transfer files when watching file descriptors"));
			opts_free(opts);
			return NULL;
			/*@+mustfreefresh@ */
		}
#ifndef __APPLE__
		if (0 != access("/proc/self/fdinfo", X_OK)) {	/* flawfinder: ignore */
			/*
			 * flawfinder rationale: access() is used here as a
			 * low cost stat() to see whether the path exists at
			 * all, under a path only modifiable by root, so is
			 * unlikely to be exploitable.
			 */
			/*@-mustfreefresh@ *//* see above */
			fprintf(stderr, "%s: -d: %s\n", opts->program_name,
				_("not available on systems without /proc/self/fdinfo"));
			opts_free(opts);
			return NULL;
			/*@+mustfreefresh@ */
		}
#endif
	}

	/*
	 * Default options: -pterb
	 */
	if (0 == numopts) {
		opts->progress = true;
		opts->timer = true;
		opts->eta = true;
		opts->rate = true;
		opts->bytes = true;
	}

	/* If -Z was given but not -E, pretend one -E was given too. */
	if (opts->error_skip_block > 0 && 0 == opts->skip_errors)
		opts->skip_errors = 1;

	/*
	 * Store remaining command-line arguments.
	 */
	while (optind < (int) argc) {
		if (!opts_add_file(opts, argv[optind++])) {
			opts_free(opts);
			return NULL;
		}
	}

	return opts;
}

/* EOF */
