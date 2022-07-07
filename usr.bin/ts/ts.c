/*	$OpenBSD: ts.c,v 1.8 2022/07/07 10:40:25 claudio Exp $	*/
/*
 * Copyright (c) 2022 Job Snijders <job@openbsd.org>
 * Copyright (c) 2022 Claudio Jeker <claudio@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/time.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static char		*format = "%b %d %H:%M:%S";
static char		*buf;
static char		*outbuf;
static size_t		 bufsize;
static size_t		 obsize;

static void		 fmtfmt(const struct timespec *);
static void __dead	 usage(void);

int
main(int argc, char *argv[])
{
	int iflag, mflag, sflag;
	int ch, prev;
	struct timespec start, now, utc_offset, ts;
	clockid_t clock = CLOCK_REALTIME;

	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");

	iflag = mflag = sflag = 0;

	while ((ch = getopt(argc, argv, "ims")) != -1) {
		switch (ch) {
		case 'i':
			iflag = 1;
			format = "%H:%M:%S";
			clock = CLOCK_MONOTONIC;
			break;
		case 'm':
			mflag = 1;
			clock = CLOCK_MONOTONIC;
			break;
		case 's':
			sflag = 1;
			format = "%H:%M:%S";
			clock = CLOCK_MONOTONIC;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if ((iflag && sflag) || argc > 1)
		usage();

	if (argc == 1)
		format = *argv;

	bufsize = strlen(format) + 1;
	if (bufsize > SIZE_MAX / 10)
		errx(1, "format string too big");
	bufsize *= 10;
	obsize = bufsize;
	if ((buf = calloc(1, bufsize)) == NULL)
		err(1, NULL);
	if ((outbuf = calloc(1, obsize)) == NULL)
		err(1, NULL);

	/* force UTC for interval calculations */
	if (iflag || sflag)
		if (setenv("TZ", "UTC", 1) == -1)
			err(1, "setenv UTC");

	clock_gettime(clock, &start);
	clock_gettime(CLOCK_REALTIME, &utc_offset);
	timespecsub(&utc_offset, &start, &utc_offset);

	for (prev = '\n'; (ch = getchar()) != EOF; prev = ch) {
		if (prev == '\n') {
			clock_gettime(clock, &now);
			if (iflag || sflag)
				timespecsub(&now, &start, &ts);
			else if (mflag)
				timespecadd(&now, &utc_offset, &ts);
			else
				ts = now;
			fmtfmt(&ts);
			if (iflag)
				start = now;
		}
		if (putchar(ch) == EOF)
			break;
	}

	if (fclose(stdout))
		err(1, "stdout");
	return 0;
}

static void __dead
usage(void)
{
	fprintf(stderr, "usage: %s [-i | -s] [-m] [format]\n", getprogname());
	exit(1);
}

/*
 * yo dawg, i heard you like format strings
 * so i put format strings in your user supplied input
 * so you can format while you format
 */
static void
fmtfmt(const struct timespec *ts)
{
	struct tm *tm;
	char *f, us[7];

	if ((tm = localtime(&ts->tv_sec)) == NULL)
		err(1, "localtime");

	snprintf(us, sizeof(us), "%06ld", ts->tv_nsec / 1000);
	strlcpy(buf, format, bufsize);
	f = buf;

	do {
		while ((f = strchr(f, '%')) != NULL && f[1] == '%')
			f += 2;

		if (f == NULL)
			break;

		f++;
		if (f[0] == '.' &&
		    (f[1] == 'S' || f[1] == 's' || f[1] == 'T')) {
			size_t l;

			f[0] = f[1];
			f[1] = '.';
			f += 2;
			l = strlen(f);
			memmove(f + 6, f, l + 1);
			memcpy(f, us, 6);
			f += 6;
		}
	} while (*f != '\0');

	*outbuf = '\0';
	if (*buf != '\0') {
		while (strftime(outbuf, obsize, buf, tm) == 0) {
			if ((outbuf = reallocarray(outbuf, 2, obsize)) == NULL)
				err(1, NULL);
			obsize *= 2;
		}
	}
	fprintf(stdout, "%s ", outbuf);
	if (ferror(stdout))
		exit(1);
}
