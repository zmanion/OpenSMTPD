/*	$OpenBSD: compress_backend.c,v 1.8 2013/05/24 17:03:14 eric Exp $	*/

/*
 * Copyright (c) 2012 Charles Longeau <chl@openbsd.org>
 * Copyright (c) 2012 Gilles Chehade <gilles@poolp.org>
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

#include "includes.h"

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <imsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smtpd.h"

#define	BUFFER_SIZE	16364

extern struct compress_backend compress_gzip;

struct compress_backend *
compress_backend_lookup(const char *name)
{
	if (!strcmp(name, "gzip"))
		return &compress_gzip;

	return NULL;
}

size_t
compress_chunk(void *ib, size_t ibsz, void *ob, size_t obsz)
{
	return (env->sc_comp->compress_chunk(ib, ibsz, ob, obsz));
}

size_t
uncompress_chunk(void *ib, size_t ibsz, void *ob, size_t obsz)
{
	return (env->sc_comp->uncompress_chunk(ib, ibsz, ob, obsz));
}

int
compress_file(FILE *ifile, FILE *ofile)
{
	return (env->sc_comp->compress_file(ifile, ofile));
}

int
uncompress_file(FILE *ifile, FILE *ofile)
{
	return (env->sc_comp->uncompress_file(ifile, ofile));
}
