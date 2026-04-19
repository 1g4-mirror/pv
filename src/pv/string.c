/*
 * Functions for portably managing strings.
 *
 * Copyright 2023-2026 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#if defined(ENABLE_NLS) && defined(HAVE_WCHAR_H)
#include <wchar.h>
#if defined(HAVE_WCTYPE_H)
#include <wctype.h>
#endif
#endif


/*
 * Wrapper for snprintf(), falling back to sprintf() on systems without that
 * function.
 *
 * Returns -1 if "format" is NULL.
 *
 * Otherwise, ensures that the buffer "str" is always terminated with a '\0'
 * byte, before returning whatever the system's vsnprintf() or vsprintf()
 * returned.
 */
int pv_snprintf(char *str, size_t size, const char *format, ...)
{
	va_list ap;
	int ret;

	if (NULL == format) {
		errno = EINVAL;
		return -1;
	}

	if (NULL != str && size > 0)
		str[0] = '\0';

	va_start(ap, format);
	/*@-nullpass@ */
#ifdef HAVE_VSNPRINTF
	ret = vsnprintf(str, size, format, ap);	/* flawfinder: ignore */
#else				/* ! HAVE_VSNPRINTF */
	ret = vsprintf(str, format, ap);    /* flawfinder: ignore */
#endif				/* HAVE_VSNPRINTF */
	/*@+nullpass@ *//* explicitly allowing NULL to behave as vsnprintf() does. */
	va_end(ap);

	if (NULL != str && size > 0)
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
 * Wrapper for asprintf(), working around it on systems without that
 * function.
 *
 * Allocates a buffer large enough for the expanded format string plus a
 * terminating '\0' byte, points *strp to it, and returns whatever the
 * system's vasprintf() or vsprintf() returned.
 *
 * Returns -1 if "strp" is NULL.
 * Returns -1 and sets *strp to NULL if "format" is NULL.
 */
int pv_asprintf(nullable_string_ptr *strp, const char *format, ...)
{
	va_list ap;
	char *new_string = NULL;
	size_t new_size = 0;
	int ret;

	if (NULL == strp)
		return -1;
	*strp = NULL;
	if (NULL == format)
		return -1;

#ifdef HAVE_VASPRINTF
	va_start(ap, format);
	/*@-unrecog@ */
	ret = vasprintf(strp, format, ap);  /* flawfinder: ignore */
	/*@+unrecog@ *//* splint doesn't know about vasprintf(). */
	va_end(ap);
	if (ret < 0)
		return ret;

	new_string = *strp;
	new_size = (size_t) ret + 1;
#else				/* ! HAVE_VASPRINTF */

#ifdef HAVE_VSNPRINTF
	/* Find out the required size. */
	{
		char tmpbuf[8];		 /* flawfinder: ignore - bounded by vsnprintf(). */
		va_start(ap, format);
		ret = vsnprintf(tmpbuf, 7, format, ap);	/* flawfinder: ignore */
		va_end(ap);
		if (ret < 0)
			return ret;
	}

	/* Allocate a buffer big enough to include a terminating \0. */
	new_size = (size_t) ret + 1;
	new_string = malloc(new_size);
	if (NULL == new_string)
		return -1;
	new_string[0] = '\0';

	/* Generate the string. */
	va_start(ap, format);
	ret = vsnprintf(new_string, new_size, format, ap);	/* flawfinder: ignore */
	va_end(ap);
	if (ret < 0) {
		int old_errno;
		old_errno = errno;
		free(new_string);
		errno = old_errno;
		return ret;
	}
	*strp = new_string;
#else				/* ! HAVE_VSNPRINTF */
	/*
	 * Without vsnprintf(), determining the required size is impossible
	 * without a buffer to write to.  The best effort here is to
	 * allocate a large buffer, write to it, then duplicate the string
	 * afterwards.  This means there's an arbitrary upper bound on
	 * string size and there will be some heap fragmentation.
	 */

	/* Allocate a large buffer for the string. */
	new_size = 16384;
	new_string = malloc(new_size);
	if (NULL == new_string)
		return -1;
	new_string[0] = '\0';

	/* Generate the string. */
	va_start(ap, format);
	ret = vsprintf(new_string, format, ap);	/* flawfinder: ignore */
	va_end(ap);
	if (ret < 0) {
		int old_errno;
		old_errno = errno;
		free(new_string);
		errno = old_errno;
		return ret;
	}

	/* Duplicate the string into a buffer just long enough for it. */
	*strp = pv_strdup(new_string);
	if (NULL == *strp) {
		int old_errno;
		old_errno = errno;
		free(new_string);
		errno = old_errno;
		return -1;
	}

	/* Free the original large buffer, and use the new one instead. */
	free(new_string);
	new_string = *strp;
#endif				/* HAVE_VSNPRINTF */
#endif				/* HAVE_VASPRINTF */

	/* Terminate the new string. */
	if (NULL != new_string && new_size > 0)
		new_string[new_size - 1] = '\0';

	/*
	 * flawfinder rationale: this function replaces asprintf so
	 * explicitly takes a non-constant format; also it explicitly
	 * \0-terminates the output buffer.
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
 * '\0', i.e. strlen(src)+strlen(dst), regardless of whether truncation
 * occurred.
 *
 * Note that this implementation has the side effect that "dst" will always
 * be terminated with a '\0' even if "src" was zero bytes long.
 */
size_t pv_strlcat(char *dst, const char *src, size_t dstsize)
{
#ifdef HAVE_STRLCAT
	size_t result;

	/*@-unrecog@ *//* splint doesn't recognise strlcat. */
	result = strlcat(dst, src, dstsize);
	/*@+unrecog@ */

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
	 * this is up to the caller; with dst, \0 termination is enforced
	 * before strlen() is called.
	 */

	available = dstsize - dstlen;
	if (available > 1)
		(void) pv_snprintf(dst + dstlen, available, "%.*s", available - 1, src);

	return dstlen + srclen;
#endif
}


/*
 * Allocate and return a duplicate of a \0-terminated string, ensuring that
 * the duplicate is also \0-terminated.  Returns NULL on error.
 */
/*@null@ */
/*@only@ */
char *pv_strdup(const /*@null@ */ char *original)
{
	size_t length;
	char *duplicate;

	if (NULL == original) {
		errno = EINVAL;
		return NULL;
	}

	length = strlen(original);	    /* flawfinder: ignore */
	/*
	 * flawfinder rationale: the original string is explicitly required
	 * to be \0 terminated.
	 */
	duplicate = calloc(1, 1 + length);
	if (NULL == duplicate)
		return NULL;

	memcpy(duplicate, original, length);	/* flawfinder: ignore */
	/*
	 * flawfinder rationale: the buffer is explicitly allocated to be
	 * large enough.
	 */

	duplicate[length] = '\0';

	return duplicate;
}


/*
 * Return a pointer to the last matching character in the buffer, or NULL if
 * not found.
 */
/*@null@ */
/*@temp@ */
void *pv_memrchr(const void *buffer, int match, size_t length)
{
#ifdef HAVE_MEMRCHR
	/*@-unrecog@ *//* splint doesn't know of memrchr() */
	return memrchr(buffer, match, length);
	/*@+unrecog@ */
#else
	unsigned char *ptr;

	if (length < 1)
		return NULL;

	ptr = ((unsigned char *) buffer) + length - 1;
	while (ptr >= (unsigned char *) buffer) {
		if ((int) (ptr[0]) == match)
			return (void *) ptr;
		ptr--;
	}

	return NULL;
#endif
}


/*
 * Return the number of display columns needed to show the
 * non-null-terminated string "string" whose length in bytes is "bytes".
 *
 * Skips ECMA-48 CSI (ESC [ ...) sequences, but any other control characters
 * are treated as printable.
 *
 * Internally, after skipping CSI sequences, the string is converted to a
 * wide character string, and each wide character's width is checked with
 * "wcswidth()".
 *
 * If NLS is disabled, or the string cannot be converted, this just returns
 * the number of bytes in the string that aren't part of CSI sequences.
 *
 * Note that this function uses internal buffers if the string is short
 * enough, otherwise it has to call malloc() and free(), so it becomes less
 * efficient with larger strings.
 */
size_t pv_strwidth(const char *string, size_t bytes)
{
	char *allocated_raw = NULL;
	static char internal_raw[256];	 /* flawfinder: ignore - bounded */
	char *raw_string = NULL;
	size_t read_pos, write_pos;
	size_t raw_bytes, width;
#if defined(ENABLE_NLS) && defined(HAVE_WCHAR_H)
	size_t wide_char_count;
	size_t wide_string_buffer_size;
	wchar_t *allocated_wide = NULL;
	static wchar_t internal_wide[256];	/* flawfinder: ignore - bounded */
	wchar_t *wide_string = NULL;
#endif				/* defined(ENABLE_NLS) && defined(HAVE_WCHAR_H) */

	if (NULL == string)
		return 0;
	if (0 == bytes)
		return 0;

	if (bytes < sizeof(internal_raw) - 1) {
		raw_string = internal_raw;
	} else {
		allocated_raw = calloc(1, 1 + bytes);
		if (NULL == allocated_raw)
			return bytes;
		raw_string = allocated_raw;
	}

	/* Copy the original string, skipping ECMA-48 CSI sequences. */
	for (read_pos = 0, write_pos = 0; read_pos < bytes; read_pos++) {
		if ((string[read_pos] != '\033') || (read_pos >= bytes - 1) || (string[read_pos + 1] != '[')) {
			raw_string[write_pos++] = string[read_pos];
			continue;
		}
		read_pos += 2;
		while ((read_pos < bytes - 1)
		       && ((string[read_pos] >= '0' && string[read_pos] <= '9')
			   || (';' == string[read_pos])
		       )
		    ) {
			read_pos++;
		}
	}
	raw_string[write_pos] = '\0';
	raw_bytes = write_pos;

	width = raw_bytes;

#if defined(ENABLE_NLS) && defined(HAVE_WCHAR_H)
	/*@-nullpass@ */
	/*
	 * splint note: mbstowcs() manual page on Linux explicitly says it
	 * takes NULL.
	 */
	wide_char_count = mbstowcs(NULL, raw_string, 0);
	/*@+nullpass@ */
	if (wide_char_count == (size_t) -1) {
		debug("%s: %s: %s", "mbstowcs", raw_string, strerror(errno));
		if (NULL != allocated_raw)
			free(allocated_raw);
		return raw_bytes;
	}

	wide_string_buffer_size = sizeof(*wide_string) * (1 + wide_char_count);

	if (wide_string_buffer_size < sizeof(internal_wide)) {
		wide_string = internal_wide;
	} else {
		allocated_wide = malloc(wide_string_buffer_size);
		if (NULL == allocated_wide) {
			pv_perror("%s", "malloc");
			if (NULL != allocated_raw)
				free(allocated_raw);
			return raw_bytes;
		}
		wide_string = allocated_wide;
	}
	memset(wide_string, 0, wide_string_buffer_size);

	if (mbstowcs(wide_string, raw_string, 1 + wide_char_count) == (size_t) -1) {
		debug("%s: %s: %s", "mbstowcs", raw_string, strerror(errno));
	} else if (NULL != wide_string) {
		/*@-unrecog@ *//* splint doesn't see the prototype. */
		width = wcswidth(wide_string, wide_char_count);
		/*@+unrecog@ */
	} else {
		width = 0;
	}

	if (NULL != allocated_wide)
		free(allocated_wide);
#endif				/* defined(ENABLE_NLS) && defined(HAVE_WCHAR_H) */

	if (NULL != allocated_raw)
		free(allocated_raw);

	return width;
}


/*
 * Return true if the character is printable 7-bit ASCII.  This function is
 * used instead of the macro from <ctype.h> to avoid causing versioned glibc
 * dependencies on some systems.
 */
bool pv_isprint(char c)
{
	return ((c >= (char) 32) && (c <= (char) 126)) ? true : false;
}
