/*	$OpenBSD: fixdfdi.c,v 1.1 2022/09/03 08:26:05 mbuhl Exp $	*/
/*	$NetBSD: fixdfdi_ieee754.c,v 1.1 2013/08/24 00:51:48 matt Exp $	*/

/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This software was developed by the Computer Systems Engineering group
 * at Lawrence Berkeley Laboratory under DARPA contract BG 91-66 and
 * contributed to Berkeley.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#if defined(LIBC_SCCS) && !defined(lint)
__RCSID("$NetBSD: fixdfdi_ieee754.c,v 1.1 2013/08/24 00:51:48 matt Exp $");
#endif /* LIBC_SCCS and not lint */

#if defined(SOFTFLOAT) || defined(__ARM_EABI__)
#include "softfloat/softfloat-for-gcc.h"
#endif

#include "../../../quad/quad.h"
#include <limits.h>
#include <stdbool.h>
#include <machine/ieee.h>

/*
 * Convert double to signed quad.
 * Not sure what to do with negative numbers---for now, anything out
 * of range becomes UQUAD_MAX.
 */
quad_t
__fixdfdi(double x)
{
	struct ieee_double ux = *(struct ieee_double *)&x;
	signed int exp = ux.dbl_exp - DBL_EXP_BIAS;
	const bool neg = ux.dbl_sign;
	quad_t r;

	if (exp > 62)
		return neg ? QUAD_MIN : QUAD_MAX;

	r = 1 << DBL_FRACHBITS;		/* implicit bit */
	r |= ux.dbl_frach;
	exp -= DBL_FRACHBITS;
	if (exp < 0) {
		r >>= -exp;
	} else if (exp > 0) {
		r <<= DBL_FRACLBITS;
		r |= ux.dbl_fracl;
		exp -= DBL_FRACLBITS;
		if (exp < 0) {
			r >>= -exp;
		} else if (exp > 0) {
			r <<= exp;
		}
	}
	return neg ? -r : r;
}
