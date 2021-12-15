/* $OpenBSD: ungc.c,v 1.2 2021/12/15 21:25:55 bluhm Exp $ */

/*
 * Copyright (c) 2021 Vitaliy Makkoveev <mvs@openbsd.org>
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
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

union msg_control{
	struct cmsghdr cmsgh;
	char control[CMSG_SPACE(sizeof(int)*2)];
};

int main(int argc, char *argv[])
{
	struct timespec ts_start, ts_now, ts_time;
	union msg_control msg_control;
	int iov_buf;
	struct iovec iov;
	struct msghdr msgh;
	struct cmsghdr *cmsgh;
	int s[2];
	int infinite = 0;

	if (argc > 1 && !strcmp(argv[1], "--infinite"))
		infinite = 1;

	if (!infinite)
		if (clock_gettime(CLOCK_BOOTTIME, &ts_start) <0)
			err(1, "clock_gettime");

	while (1) {
		if (socketpair(AF_UNIX, SOCK_STREAM|O_NONBLOCK, 0, s) < 0)
			err(1, "socketpair");

		iov_buf = 0;
		iov.iov_base = &iov_buf;
		iov.iov_len = sizeof(iov_buf);
		msgh.msg_control = msg_control.control;
		msgh.msg_controllen = sizeof(msg_control.control);
		msgh.msg_iov = &iov;
		msgh.msg_iovlen = 1;
		msgh.msg_name = NULL;
		msgh.msg_namelen = 0;
		cmsgh = CMSG_FIRSTHDR(&msgh);
		cmsgh->cmsg_len = CMSG_LEN(sizeof(int) * 2);
		cmsgh->cmsg_level = SOL_SOCKET;
		cmsgh->cmsg_type = SCM_RIGHTS;
		*((int *)CMSG_DATA(cmsgh) + 0) = s[0];
		*((int *)CMSG_DATA(cmsgh) + 1) = s[1];

		if (sendmsg(s[0], &msgh, 0) < 0) {
			if (errno != EMFILE)
				err(1, "sendmsg");
		}

		close(s[0]);
		close(s[1]);

		if (!infinite) {
			if (clock_gettime(CLOCK_BOOTTIME, &ts_now) <0)
				err(1, "clock_gettime");

			timespecsub(&ts_now, &ts_start, &ts_time);
			if (ts_time.tv_sec >= 20)
				break;
		}
	}

	return 0;
}
