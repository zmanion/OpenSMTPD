/*	$OpenBSD: mfa.c,v 1.73 2012/11/12 14:58:53 eric Exp $	*/

/*
 * Copyright (c) 2008 Gilles Chehade <gilles@openbsd.org>
 * Copyright (c) 2008 Pierre-Yves Ritschard <pyr@openbsd.org>
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
#include <sys/wait.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smtpd.h"
#include "log.h"

static void mfa_imsg(struct imsgev *, struct imsg *);
static void mfa_shutdown(void);
static void mfa_sig_handler(int, short, void *);
static void mfa_test_connect(struct envelope *);
static void mfa_test_helo(struct envelope *);
static void mfa_test_mail(struct envelope *);
static void mfa_test_rcpt(struct envelope *);
static void mfa_test_rcpt_resume(struct submit_status *);
static void mfa_test_dataline(struct submit_status *);
static void mfa_test_quit(struct envelope *);
static void mfa_test_close(struct envelope *);
static void mfa_test_rset(struct envelope *);
static int mfa_strip_source_route(char *, size_t);

static void
mfa_imsg(struct imsgev *iev, struct imsg *imsg)
{
	struct filter *filter;

	if (iev->proc == PROC_SMTP) {
		switch (imsg->hdr.type) {
		case IMSG_MFA_CONNECT:
			mfa_test_connect(imsg->data);
			return;
		case IMSG_MFA_HELO:
			mfa_test_helo(imsg->data);
			return;
		case IMSG_MFA_MAIL:
			mfa_test_mail(imsg->data);
			return;
		case IMSG_MFA_RCPT:
			mfa_test_rcpt(imsg->data);
			return;
		case IMSG_MFA_DATALINE:
			mfa_test_dataline(imsg->data);
			return;
		case IMSG_MFA_QUIT:
			mfa_test_quit(imsg->data);
			return;
		case IMSG_MFA_CLOSE:
			mfa_test_close(imsg->data);
			return;
		case IMSG_MFA_RSET:
			mfa_test_rset(imsg->data);
			return;
		}
	}

	if (iev->proc == PROC_LKA) {
		switch (imsg->hdr.type) {
		case IMSG_LKA_MAIL:
			imsg_compose_event(env->sc_ievs[PROC_SMTP],
			    IMSG_MFA_MAIL, 0, 0, -1, imsg->data,
			    sizeof(struct submit_status));
			return;
		case IMSG_LKA_RCPT:
			imsg_compose_event(env->sc_ievs[PROC_SMTP],
			    IMSG_MFA_RCPT, 0, 0, -1, imsg->data,
			    sizeof(struct submit_status));
			return;

		case IMSG_LKA_RULEMATCH:
			mfa_test_rcpt_resume(imsg->data);
			return;
		}
	}

	if (iev->proc == PROC_PARENT) {
		switch (imsg->hdr.type) {
		case IMSG_CONF_START:
			dict_init(&env->sc_filters);
			return;

		case IMSG_CONF_FILTER:
			filter = xmemdup(imsg->data, sizeof *filter,
			    "mfa_imsg");
			dict_set(&env->sc_filters, filter->name, filter);
			return;

		case IMSG_CONF_END:
			mfa_session_filters_init();
			return;

		case IMSG_CTL_VERBOSE:
			log_verbose(*(int *)imsg->data);
			return;
		}
	}

	errx(1, "mfa_imsg: unexpected %s imsg", imsg_to_str(imsg->hdr.type));
}

static void
mfa_sig_handler(int sig, short event, void *p)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		mfa_shutdown();
		break;

	case SIGCHLD:
		fatalx("unexpected SIGCHLD");
		break;

	default:
		fatalx("mfa_sig_handler: unexpected signal");
	}
}

static void
mfa_shutdown(void)
{
	pid_t pid;

	do {
		pid = waitpid(WAIT_MYPGRP, NULL, 0);
	} while (pid != -1 || (pid == -1 && errno == EINTR));

	log_info("info: mail filter exiting");
	_exit(0);
}


pid_t
mfa(void)
{
	pid_t		 pid;
	struct passwd	*pw;

	struct event	 ev_sigint;
	struct event	 ev_sigterm;
	struct event	 ev_sigchld;

	struct peer peers[] = {
		{ PROC_PARENT,	imsg_dispatch },
		{ PROC_SMTP,	imsg_dispatch },
		{ PROC_LKA,	imsg_dispatch },
		{ PROC_CONTROL,	imsg_dispatch }
	};

	switch (pid = fork()) {
	case -1:
		fatal("mfa: cannot fork");
	case 0:
		break;
	default:
		return (pid);
	}

	purge_config(PURGE_EVERYTHING);

	if ((env->sc_pw =  getpwnam(SMTPD_FILTER_USER)) == NULL)
		if ((env->sc_pw =  getpwnam(SMTPD_USER)) == NULL)
			fatalx("unknown user " SMTPD_FILTER_USER);
	pw = env->sc_pw;

	smtpd_process = PROC_MFA;
	setproctitle("%s", env->sc_title[smtpd_process]);

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("mfa: cannot drop privileges");

	imsg_callback = mfa_imsg;
	event_init();

	tree_init(&env->mfa_sessions);

	signal_set(&ev_sigint, SIGINT, mfa_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, mfa_sig_handler, NULL);
	signal_set(&ev_sigchld, SIGCHLD, mfa_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal_add(&ev_sigchld, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	config_pipes(peers, nitems(peers));
	config_peers(peers, nitems(peers));

	imsgproc_init();
	if (event_dispatch() < 0)
		fatal("event_dispatch");
	mfa_shutdown();

	return (0);
}

static void
mfa_test_connect(struct envelope *e)
{
	struct submit_status	 ss;

	ss.id = e->session_id;
	ss.envelope = *e;

	mfa_session(&ss, FILTER_CONNECT);
}

static void
mfa_test_helo(struct envelope *e)
{
	struct submit_status	 ss;

	ss.id = e->session_id;
	ss.envelope = *e;

	mfa_session(&ss, FILTER_HELO);
}

static void
mfa_test_mail(struct envelope *e)
{
	struct submit_status	 ss;

	ss.id = e->session_id;
	ss.u.maddr = e->sender;

	if (mfa_strip_source_route(ss.u.maddr.user, sizeof(ss.u.maddr.user)))
		goto refuse;

	if (! valid_localpart(ss.u.maddr.user) ||
	    ! valid_domainpart(ss.u.maddr.domain)) {
		/*
		 * "MAIL FROM:<>" is the exception we allow.
		 */
		if (!(ss.u.maddr.user[0] == '\0' &&
			ss.u.maddr.domain[0] == '\0'))
			goto refuse;
	}

	mfa_session(&ss, FILTER_MAIL);
	return;

refuse:
	imsg_compose_event(env->sc_ievs[PROC_SMTP], IMSG_MFA_MAIL, 0, 0, -1,
	    &ss, sizeof(ss));
	return;
}

static void
mfa_test_rcpt(struct envelope *e)
{
	struct submit_status	 ss;

	ss.id = e->session_id;
	ss.u.maddr = e->rcpt;
	ss.ss = e->ss;
	ss.envelope = *e;
	ss.envelope.dest = e->rcpt;
	ss.flags = e->flags;

	mfa_strip_source_route(ss.u.maddr.user, sizeof(ss.u.maddr.user));

	if (! valid_localpart(ss.u.maddr.user) ||
	    ! valid_domainpart(ss.u.maddr.domain))
		goto refuse;

	mfa_session(&ss, FILTER_RCPT);
	return;

refuse:
	imsg_compose_event(env->sc_ievs[PROC_SMTP], IMSG_MFA_RCPT, 0, 0, -1,
	    &ss, sizeof(ss));
}

static void
mfa_test_rcpt_resume(struct submit_status *ss)
{
	if (ss->code != 250) {
		imsg_compose_event(env->sc_ievs[PROC_SMTP], IMSG_MFA_RCPT, 0, 0,
		    -1, ss, sizeof(*ss));
		return;
	}

	ss->envelope.dest = ss->u.maddr;
	imsg_compose_event(env->sc_ievs[PROC_LKA], IMSG_LKA_RCPT, 0, 0, -1,
	    ss, sizeof(*ss));
}

static void
mfa_test_dataline(struct submit_status *ss)
{
	ss->code = 250;

	mfa_session(ss, FILTER_DATALINE);
}

static void
mfa_test_quit(struct envelope *e)
{
	struct submit_status	 ss;

	ss.id = e->session_id;
	ss.envelope = *e;

	mfa_session(&ss, FILTER_QUIT);
}

static void
mfa_test_close(struct envelope *e)
{
	struct submit_status	 ss;

	ss.id = e->session_id;
	ss.envelope = *e;

	mfa_session(&ss, FILTER_CLOSE);
}

static void
mfa_test_rset(struct envelope *e)
{
	struct submit_status	 ss;

	ss.id = e->session_id;
	ss.envelope = *e;

	mfa_session(&ss, FILTER_RSET);
}

static int
mfa_strip_source_route(char *buf, size_t len)
{
	char *p;

	p = strchr(buf, ':');
	if (p != NULL) {
		p++;
		memmove(buf, p, strlen(p) + 1);
		return 1;
	}

	return 0;
}
