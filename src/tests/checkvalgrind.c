/*
 * Trigger valgrind leak and overflow detection to confirm it's working.
 *
 * Copyright 2026 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (2 == argc && 0 == strcmp(argv[1], "leak_memory")) {
		/* Leak memory by allocating and never freeing it. */
		char *buf;
		buf = malloc(65536);
		printf("%s: %p\n", "allocated buffer, never freed", buf);
	} else if (2 == argc && 0 == strcmp(argv[1], "leak_fd")) {
		/* Leak a file descriptor by opening and not closing it. */
		int fd;
		fd = dup(STDOUT_FILENO);
		printf("%s: %d\n", "file descriptor opened, never closed", fd);
	} else if (2 == argc && 0 == strcmp(argv[1], "overflow")) {
		/* Write past the end of a buffer. */
		char *buf;
		buf = malloc(1020);
		buf[1020] = 'a';
		printf("%s: %p\n", "overflowed this buffer", buf);
		free(buf);
	} else {
		fprintf(stderr, "%s: %s %s\n", "Usage", argv[0], "leak_memory|leak_fd|overflow");
		/* Return 0, not 1, so only valgrind will ever exit nonzero. */
		return 0;
	}
	return 0;
}
