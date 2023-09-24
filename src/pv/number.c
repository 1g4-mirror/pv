/*
 * Functions for converting strings to numbers.
 *
 * Copyright 2002-2008, 2010, 2012-2015, 2017, 2021, 2023 Andrew Wood
 *
 * License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.
 */

#include "config.h"
#include "pv.h"

#include <stddef.h>


/*
 * This function is used instead of the macro from <ctype.h> because
 * including <ctype.h> causes weird versioned glibc dependencies on certain
 * Red Hat systems, complicating package management.
 */
static bool pv__isdigit(char c)
{
	return ((c >= '0') && (c <= '9')) ? true : false;
}


/*
 * Return the numeric value of "str", as an off_t, where "str" is expected
 * to be a sequence of digits (without a thousands separator), possibly with
 * a fractional part, optionally followed by a units suffix such as "K" for
 * kibibytes.
 */
off_t pv_getnum_size(const char *str)
{
	off_t integral_part = 0;
	off_t fractional_part = 0;
	unsigned int fractional_divisor = 1;
	unsigned int shift = 0;

	if (NULL == str)
		return (off_t) 0;

	/* Skip any non-numeric leading characters. */
	while (str[0] != '\0' && (!pv__isdigit(str[0])))
		str++;

	/*
	 * Parse the integral part of the number - the digits before the
	 * decimal mark or units.
	 */
	for (; pv__isdigit(str[0]); str++) {
		integral_part = integral_part * 10;
		integral_part += (off_t) (str[0] - '0');
	}

	/*
	 * If the next character is a decimal mark, skip over it and parse
	 * the following digits as the fractional part of the number.
	 *
	 * Note that we hard-code the decimal mark as '.' or ',' so this
	 * will fail if there are any locales whose decimal mark is not one
	 * of those two characters.
	 */
	if (('.' == str[0]) || (',' == str[0])) {
		str++;
		for (; pv__isdigit(str[0]); str++) {
			/* Stop counting below 0.0001. */
			if (fractional_divisor < 10000) {
				fractional_part = fractional_part * 10;
				fractional_part += (off_t) (str[0] - '0');
				fractional_divisor = fractional_divisor * 10;
			}
		}
	}

	/*
	 * Parse any units given (K=KiB=*1024, M=MiB=1024KiB, G=GiB=1024MiB,
	 * T=TiB=1024GiB).
	 */
	if (str[0] != '\0') {
		/* Skip any spaces or tabs after the digits. */
		while ((' ' == str[0]) || ('\t' == str[0]))
			str++;
		switch (str[0]) {
		case 'k':
		case 'K':
			shift = 10;
			break;
		case 'm':
		case 'M':
			shift = 20;
			break;
		case 'g':
		case 'G':
			shift = 30;
			break;
		case 't':
		case 'T':
			shift = 40;
			break;
		default:
			break;
		}
	}

	/*
	 * Binary left-shift the supplied number by "shift" times, i.e.
	 * apply the given units (KiB, MiB, etc) to it, but never shift left
	 * more than 30 at a time to avoid overflows.
	 */
	while (shift > 0) {
		unsigned int shiftby;

		shiftby = shift;
		if (shiftby > 30)
			shiftby = 30;

		/*@-shiftimplementation@ */
		/*
		 * splint note: ignore the fact that the types we are
		 * shifting are signed, because we know they are definitely
		 * not negative.
		 */
		integral_part = (off_t) (integral_part << shiftby);
		fractional_part = (off_t) (fractional_part << shiftby);
		/*@+shiftimplementation@ */

		shift -= shiftby;
	}

	/*
	 * Add the fractional part, divided by its divisor, to the integral
	 * part, now that we've multiplied everything by the appropriate
	 * units.
	 */
	fractional_part = fractional_part / fractional_divisor;
	integral_part += fractional_part;

	return integral_part;
}


/*
 * Return the numeric value of "str", as a double, where "str" is expected
 * to be a positive decimal number expressing a time interval.
 */
double pv_getnum_interval(const char *str)
{
	double result = 0.0;
	double step = 1;

	if (NULL == str)
		return 0.0;

	/* Skip any non-digit characters at the start. */
	while (str[0] != '\0' && (!pv__isdigit(str[0])))
		str++;

	/* Parse the digits before the decimal mark. */
	for (; pv__isdigit(str[0]); str++) {
		result = result * 10;
		result += (double) (str[0] - '0');
	}

	/* If there is no decimal mark, return the value as-is. */
	if ((str[0] != '.') && (str[0] != ','))
		return result;

	/* Move past the decimal mark. */
	str++;

	/* Parse the digits after the decimal mark, up to 0.0000001. */
	for (; pv__isdigit(str[0]) && step < 1000000; str++) {
		step = step * 10;
		result += ((double) (str[0] - '0')) / step;
	}

	return result;
}


/*
 * Return the numeric value of "str", as an unsigned int, following the same
 * rules as pv_getnum_size(), expecting "str" to express a value to be used
 * as a count (such as number of screen columns, or size of a buffer).
 */
unsigned int pv_getnum_count(const char *str)
{
	return (unsigned int) pv_getnum_size(str);
}


/*
 * Return true if the given string is a valid number of the given type.
 */
bool pv_getnum_check(const char *str, pv_numtype type)
{
	if (NULL == str)
		return false;

	/* Skip leading spaces and tabs. */
	while ((' ' == str[0]) || ('\t' == str[0]))
		str++;

	/* If the next character isn't a digit, this isn't a number. */
	if (!pv__isdigit(str[0]))
		return false;

	/* Skip over the digits. */
	for (; pv__isdigit(str[0]); str++);

	/*
	 * If there's a decimal mark (see note in pv_getnum_size() above),
	 * check that too.
	 */
	if (('.' == str[0]) || (',' == str[0])) {
		/* Integers should have no decimal mark. */
		if (type == PV_NUMTYPE_INTEGER)
			return false;
		/* Skip the decimal mark, then all digits. */
		str++;
		for (; pv__isdigit(str[0]); str++);
	}

	/* If the string ends here, this is a valid number. */
	if ('\0' == str[0])
		return true;

	/* A units suffix is not allowed for doubles, only for integers. */
	if (type == PV_NUMTYPE_DOUBLE)
		return false;

	/* Skip trailing spaces or tabs. */
	while ((' ' == str[0]) || ('\t' == str[0]))
		str++;

	/* Check the units suffix is one we know about. */
	switch (str[0]) {
	case 'k':
	case 'K':
	case 'm':
	case 'M':
	case 'g':
	case 'G':
	case 't':
	case 'T':
		str++;
		break;
	default:
		return false;
	}

	/* If the string has trailing text, it's not a valid number. */
	if (str[0] != '\0')
		return false;

	return true;
}

/* EOF */
