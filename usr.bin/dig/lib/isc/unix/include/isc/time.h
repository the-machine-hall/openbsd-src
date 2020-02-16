/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: time.h,v 1.11 2020/02/16 21:08:59 florian Exp $ */

#ifndef ISC_TIME_H
#define ISC_TIME_H 1

/*! \file */

#include <sys/time.h>
#include <time.h>
#include <isc/types.h>

/*
 * ISC_FORMATHTTPTIMESTAMP_SIZE needs to be 30 in C locale and potentially
 * more for other locales to handle longer national abbreviations when
 * expanding strftime's %a and %b.
 */
#define ISC_FORMATHTTPTIMESTAMP_SIZE 50

/***
 *** Absolute Times
 ***/

isc_result_t
isc_time_now(struct timespec *t);
/*%<
 * Set 't' to the current absolute time.
 *
 * Requires:
 *
 *\li	't' is a valid pointer.
 *
 * Returns:
 *
 *\li	Success
 *\li	Unexpected error
 *		Getting the time from the system failed.
 */

uint64_t
isc_time_microdiff(const struct timespec *t1, const struct timespec *t2);
/*%<
 * Find the difference in microseconds between time t1 and time t2.
 * t2 is the subtrahend of t1; ie, difference = t1 - t2.
 *
 * Requires:
 *
 *\li	't1' and 't2' are valid pointers.
 *
 * Returns:
 *\li	The difference of t1 - t2, or 0 if t1 <= t2.
 */

void
isc_time_formattimestamp(const struct timespec *t, char *buf, unsigned int len);
/*%<
 * Format the time 't' into the buffer 'buf' of length 'len',
 * using a format like "30-Aug-2000 04:06:47.997" and the local time zone.
 * If the text does not fit in the buffer, the result is indeterminate,
 * but is always guaranteed to be null terminated.
 *
 *  Requires:
 *\li      'len' > 0
 *\li      'buf' points to an array of at least len chars
 *
 */

void
isc_time_formathttptimestamp(const struct timespec *t, char *buf, unsigned int len);
/*%<
 * Format the time 't' into the buffer 'buf' of length 'len',
 * using a format like "Mon, 30 Aug 2000 04:06:47 GMT"
 * If the text does not fit in the buffer, the result is indeterminate,
 * but is always guaranteed to be null terminated.
 *
 *  Requires:
 *\li      'len' > 0
 *\li      'buf' points to an array of at least len chars
 *
 */

#endif /* ISC_TIME_H */
