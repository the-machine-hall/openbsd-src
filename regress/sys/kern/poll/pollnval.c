/*	$OpenBSD: pollnval.c,v 1.1 2021/10/29 13:13:04 mpi Exp $	*/

/*
 * Copyright (c) 2021 Leah Neukirchen <leah@vuxu.org>
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

#include <assert.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>

int
main(void)
{
	struct pollfd fds[1];

	fds[0].fd = 0;
	fds[0].events = POLLIN | POLLHUP;
	close(0);

	assert(poll(fds, 1, -1) == 1);
	assert(fds[0].revents & POLLNVAL);

	return 0;
}
