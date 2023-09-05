/*
 * Functions for portably managing strings.
 *
 * Copyright 2023 Andrew Wood
 *
 * Distributed under the Artistic License v2.0; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>


/*
 * Wrapper for sprintf(), falling back to sprintf() on systems without that
 * function.
 *
 * Returns -1 if "str" or "format" are NULL or if "size" is 0.
 *
 * Otherwise, ensures that the buffer "str" is always terminated with a '\0'
 * byte, before returning whatever the system's vsnprintf() or vsprintf()
 * returned.
 */
int pv_snprintf(char *str, size_t size, const char *format, ...)
{
	va_list ap;
	int ret;

	if (NULL == str)
		return -1;
	if (0 == size)
		return -1;
	if (NULL == format)
		return -1;

	str[0] = '\0';

	va_start(ap, format);
#ifdef HAVE_VSNPRINTF
	ret = vsnprintf(str, size, format, ap);	/* flawfinder: ignore */
#else				/* ! HAVE_VSNPRINTF */
	ret = vsprintf(str, format, ap);    /* flawfinder: ignore */
#endif				/* HAVE_VSNPRINTF */
	va_end(ap);

	str[size - 1] = '\0';

	/*
	 * flawfinder rationale: this function replaces snprintf so
	 * explicitly takes a non-constant format; also it explicitly
	 * \0-terminates the output buffer, as flawfinder warns that some
	 * sprintf() variants do not.
	 */

	return ret;
}

/*
 * Implementation of strlcat() where it is unavailable: append a string to a
 * buffer, constraining the buffer to a particular size and ensuring
 * termination with '\0'.
 *
 * Appends the string "src" to the buffer "dst", assuming "dst" is "dstsize"
 * bytes long, and ensuring that "dst" is always terminated with a '\0'
 * byte.
 *
 * Returns the intended length of the string, not including the terminating
 * '\0', i.e.  strlen(src)+strlen(dst), regardless of whether truncation
 * occurred.
 *
 * Note that this implementation has the side effect that "dst" will always
 * be terminated with a '\0' even if "src" was zero bytes long.
 */
size_t pv_strlcat(char *dst, const char *src, size_t dstsize)
{
#ifdef HAVE_STRLCAT
	size_t result;

	result = strlcat(dst, src, dstsize);

	if ((NULL != dst) && (dstsize > 0))
		dst[dstsize - 1] = '\0';

	return result;
#else
	size_t dstlen, srclen, available;

	if (NULL == dst)
		return 0;
	if (NULL == src)
		return 0;
	if (0 == dstsize)
		return 0;

	dst[dstsize - 1] = '\0';
	dstlen = strlen(dst);		    /* flawfinder: ignore */
	srclen = strlen(src);		    /* flawfinder: ignore */

	/*
	 * flawfinder rationale: src must explicitly be \0 terminated, so
	 * this is up to the caller; with dst, we enforce \0 termination
	 * before calling strlen().
	 */

	available = dstsize - dstlen;
	if (available > 1)
		(void) pv_snprintf(dst + dstlen, available, "%.*s", available - 1, src);

	return dstlen + srclen;
#endif
}

/* EOF */
