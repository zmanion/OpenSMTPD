/*	$OpenBSD: filter_api.c,v 1.4 2012/08/19 14:16:58 chl Exp $	*/

/*
 * Copyright (c) 2011 Gilles Chehade <gilles@openbsd.org>
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
#include <sys/queue.h>
#include <sys/uio.h>

#include <err.h>
#include <event.h>
#include <fcntl.h>
#include <imsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smtpd-api.h"

static struct tree		sessions;
struct session {
	enum filter_type	hook;
	struct filter_msg	fm;
};

static struct filter_internals {
	uint32_t	filtermask;

	struct event	ev;
	struct imsgbuf	ibuf;

	void (*connect_cb)(uint64_t, struct filter_connect *, void *);
	void *connect_cb_arg;

	void (*helo_cb)(uint64_t, struct filter_helo *, void *);
	void *helo_cb_arg;

	void (*ehlo_cb)(uint64_t, struct filter_helo *, void *);
	void *ehlo_cb_arg;

	void (*mail_cb)(uint64_t, struct filter_mail *, void *);
	void *mail_cb_arg;

	void (*rcpt_cb)(uint64_t, struct filter_rcpt *, void *);
	void *rcpt_cb_arg;

	void (*dataline_cb)(uint64_t, struct filter_dataline *, void *);
	void *dataline_cb_arg;

	void (*quit_cb)(uint64_t, void *);
	void *quit_cb_arg;

	void (*close_cb)(uint64_t, void *);
	void *close_cb_arg;

	void (*rset_cb)(uint64_t, void *);
	void *rset_cb_arg;
} fi;

static void filter_handler(int, short, void *);
static void filter_register_callback(enum filter_type, void *, void *);

void
filter_init(void)
{
	bzero(&fi, sizeof (fi));

	imsg_init(&fi.ibuf, 0);
	event_init();
	event_set(&fi.ev, 0, EV_READ, filter_handler, (void *)&fi);
	event_add(&fi.ev, NULL);

	tree_init(&sessions);
}

void
filter_loop(void)
{
	/* notify smtpd of all registered hooks */
	imsg_compose(&fi.ibuf, FILTER_REGISTER, 0, 0, -1,
	    &fi.filtermask, sizeof fi.filtermask);
	event_set(&fi.ev, 0, EV_READ|EV_WRITE, filter_handler, NULL);
	event_add(&fi.ev, NULL);

	if (event_dispatch() < 0)
		errx(1, "event_dispatch");
}

void
filter_register_connect_callback(void (*cb)(uint64_t, struct filter_connect *, void *),
    void *cb_arg)
{
	filter_register_callback(FILTER_CONNECT, cb, cb_arg);
}

void
filter_register_helo_callback(void (*cb)(uint64_t, struct filter_helo *, void *),
    void *cb_arg)
{
	filter_register_callback(FILTER_HELO, cb, cb_arg);
}

void
filter_register_ehlo_callback(void (*cb)(uint64_t, struct filter_helo *, void *),
    void *cb_arg)
{
	filter_register_callback(FILTER_EHLO, cb, cb_arg);
}

void
filter_register_mail_callback(void (*cb)(uint64_t, struct filter_mail *, void *),
    void *cb_arg)
{
	filter_register_callback(FILTER_MAIL, cb, cb_arg);
}

void
filter_register_rcpt_callback(void (*cb)(uint64_t, struct filter_rcpt *, void *),
    void *cb_arg)
{
	filter_register_callback(FILTER_RCPT, cb, cb_arg);
}

void
filter_register_dataline_callback(void (*cb)(uint64_t, struct filter_dataline *, void *),
    void *cb_arg)
{
	filter_register_callback(FILTER_DATALINE, cb, cb_arg);
}

void
filter_register_quit_callback(void (*cb)(uint64_t, void *),
    void *cb_arg)
{
	filter_register_callback(FILTER_QUIT, cb, cb_arg);
}

void
filter_register_close_callback(void (*cb)(uint64_t, void *), void *cb_arg)
{
	filter_register_callback(FILTER_CLOSE, cb, cb_arg);
}

void
filter_register_rset_callback(void (*cb)(uint64_t, void *), void *cb_arg)
{
	filter_register_callback(FILTER_RSET, cb, cb_arg);
}

void
filter_accept(uint64_t id)
{
	struct session	*session = tree_xpop(&sessions, id);

	session->fm.code = 0;
	imsg_compose(&fi.ibuf, session->hook, 0, 0, -1, &session->fm,
	    sizeof session->fm);
	event_set(&fi.ev, 0, EV_READ|EV_WRITE, filter_handler, &fi);
	event_add(&fi.ev, NULL);
}

void
filter_reject(uint64_t id, uint32_t code, char *status)
{
	struct session	*session = tree_xpop(&sessions, id);

	/* This is NOT an acceptable code for a failure */
	if (code < 400 || code >= 600)
		code = 530;
	session->fm.code = code;
	strlcpy(session->fm.errorline, status, sizeof session->fm.errorline);
	imsg_compose(&fi.ibuf, session->hook, 0, 0, -1, &session->fm,
	    sizeof session->fm);
	event_set(&fi.ev, 0, EV_READ|EV_WRITE, filter_handler, &fi);
	event_add(&fi.ev, NULL);
}

static void
filter_register_callback(enum filter_type type, void *cb, void *cb_arg)
{
	switch (type) {
	case FILTER_CONNECT:
		fi.connect_cb = cb;
		fi.connect_cb_arg = cb_arg;
		break;
	case FILTER_HELO:
		fi.helo_cb = cb;
		fi.helo_cb_arg = cb_arg;
		break;
	case FILTER_EHLO:
		fi.ehlo_cb = cb;
		fi.ehlo_cb_arg = cb_arg;
		break;
	case FILTER_MAIL:
		fi.mail_cb = cb;
		fi.mail_cb_arg = cb_arg;
		break;
	case FILTER_RCPT:
		fi.rcpt_cb = cb;
		fi.rcpt_cb_arg = cb_arg;
		break;
	case FILTER_DATALINE:
		fi.dataline_cb = cb;
		fi.dataline_cb_arg = cb_arg;
		break;
	case FILTER_QUIT:
		fi.quit_cb = cb;
		fi.quit_cb_arg = cb_arg;
		break;
	case FILTER_CLOSE:
		fi.close_cb = cb;
		fi.close_cb_arg = cb_arg;
		break;
	case FILTER_RSET:
		fi.rset_cb = cb;
		fi.rset_cb_arg = cb_arg;
		break;
	default:
		errx(1, "filter_register_callback: unknown filter type");
	}

	fi.filtermask |= type;
}

static void
filter_handler(int fd, short event, void *p)
{
	struct imsg		imsg;
	ssize_t			n;
	short			evflags = EV_READ;
	struct session	       *session;

	if (event & EV_READ) {
		n = imsg_read(&fi.ibuf);
		if (n == -1)
			err(1, "imsg_read");
		if (n == 0) {
			event_del(&fi.ev);
			event_loopexit(NULL);
			return;
		}
	}

	if (event & EV_WRITE) {
		if (msgbuf_write(&fi.ibuf.w) == -1)
			err(1, "msgbuf_write");
		if (fi.ibuf.w.queued)
			evflags |= EV_WRITE;
	}

	for (;;) {
		n = imsg_get(&fi.ibuf, &imsg);
		if (n == -1)
			errx(1, "imsg_get");
		if (n == 0)
			break;

		session = calloc(1, sizeof *session);
		if (session == NULL)
			errx(1, "memory exhaustion");

		if ((imsg.hdr.len - IMSG_HEADER_SIZE)
		    != sizeof(session->fm))
			errx(1, "corrupted imsg");

		memcpy(&session->fm, imsg.data, sizeof (session->fm));
		if (session->fm.version != FILTER_API_VERSION)
			errx(1, "API version mismatch");

		tree_set(&sessions, session->fm.id, session);
		session->hook = imsg.hdr.type;
		
		switch (session->hook) {
		case FILTER_CONNECT:
			fi.connect_cb(session->fm.id, &session->fm.u.connect,
			    fi.connect_cb_arg);
			break;
		case FILTER_HELO:
			fi.helo_cb(session->fm.id, &session->fm.u.helo,
			    fi.helo_cb_arg);
			break;
		case FILTER_EHLO:
			fi.ehlo_cb(session->fm.id, &session->fm.u.helo,
			    fi.ehlo_cb_arg);
			break;
		case FILTER_MAIL:
			fi.mail_cb(session->fm.id, &session->fm.u.mail,
			    fi.mail_cb_arg);
			break;
		case FILTER_RCPT:
			fi.rcpt_cb(session->fm.id, &session->fm.u.rcpt,
			    fi.rcpt_cb_arg);
			break;
		case FILTER_DATALINE:
			fi.dataline_cb(session->fm.id, &session->fm.u.dataline,
			    fi.dataline_cb_arg);
			break;
		case FILTER_QUIT:
			fi.quit_cb(session->fm.id, fi.quit_cb_arg);
			break;
		case FILTER_CLOSE:
			fi.close_cb(session->fm.id, fi.close_cb_arg);
			break;
		case FILTER_RSET:
			fi.rset_cb(session->fm.id, fi.rset_cb_arg);
			break;

		default:
			errx(1, "unsupported imsg");
		}
		imsg_free(&imsg);
	}
	event_set(&fi.ev, 0, EV_READ|EV_WRITE, filter_handler, NULL);
	event_add(&fi.ev, NULL);
}
