/*
 * Frontend variables and functions.
 *
 * Copyright 2000-2010 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/time.h>

#include <types/global.h>

#include <proto/acl.h>
#include <proto/buffers.h>
#include <proto/fd.h>
#include <proto/frontend.h>
#include <proto/log.h>
#include <proto/hdr_idx.h>
#include <proto/proto_tcp.h>
#include <proto/proto_http.h>
#include <proto/proxy.h>
#include <proto/session.h>
#include <proto/stream_interface.h>
#include <proto/stream_sock.h>
#include <proto/task.h>


/* Retrieves the original destination address used by the client, and sets the
 * SN_FRT_ADDR_SET flag.
 */
void get_frt_addr(struct session *s)
{
	socklen_t namelen = sizeof(s->frt_addr);

	if (get_original_dst(s->si[0].fd, (struct sockaddr_in *)&s->frt_addr, &namelen) == -1)
		getsockname(s->si[0].fd, (struct sockaddr *)&s->frt_addr, &namelen);
	s->flags |= SN_FRT_ADDR_SET;
}

/* Finish a session accept() for a proxy (TCP or HTTP). It returns a negative
 * value in case of failure, a positive value in case of success, or zero if
 * it is a success but the session must be closed ASAP.
 */
int frontend_accept(struct session *s)
{
	int cfd = s->si[0].fd;

	tv_zero(&s->logs.tv_request);
	s->logs.t_queue = -1;
	s->logs.t_connect = -1;
	s->logs.t_data = -1;
	s->logs.t_close = 0;
	s->logs.bytes_in = s->logs.bytes_out = 0;
	s->logs.prx_queue_size = 0;  /* we get the number of pending conns before us */
	s->logs.srv_queue_size = 0; /* we will get this number soon */

	s->data_state  = DATA_ST_INIT;
	s->data_source = DATA_SRC_NONE;

	/* FIXME: the logs are horribly complicated now, because they are
	 * defined in <p>, <p>, and later <be> and <be>.
	 */
	if (s->logs.logwait & LW_REQ)
		s->do_log = http_sess_log;
	else
		s->do_log = tcp_sess_log;

	/* default error reporting function, may be changed by analysers */
	s->srv_error = default_srv_error;

	/* Adjust some socket options */
	if (unlikely(setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) == -1)) {
		Alert("accept(): cannot set the socket in non blocking mode. Giving up\n");
		goto out_delete_cfd;
	}

	if (s->fe->options & PR_O_TCP_CLI_KA)
		setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof(one));

	if (s->fe->options & PR_O_TCP_NOLING)
		setsockopt(cfd, SOL_SOCKET, SO_LINGER, (struct linger *) &nolinger, sizeof(struct linger));

	if (global.tune.client_sndbuf)
		setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &global.tune.client_sndbuf, sizeof(global.tune.client_sndbuf));

	if (global.tune.client_rcvbuf)
		setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &global.tune.client_rcvbuf, sizeof(global.tune.client_rcvbuf));

	if (s->fe->mode == PR_MODE_HTTP) {
		/* the captures are only used in HTTP frontends */
		if (unlikely(s->fe->nb_req_cap > 0 &&
			     (s->txn.req.cap = pool_alloc2(s->fe->req_cap_pool)) == NULL))
			goto out_delete_cfd;	/* no memory */

		if (unlikely(s->fe->nb_rsp_cap > 0 &&
			     (s->txn.rsp.cap = pool_alloc2(s->fe->rsp_cap_pool)) == NULL))
			goto out_free_reqcap;	/* no memory */
	}

	if (s->fe->acl_requires & ACL_USE_L7_ANY) {
		/* we have to allocate header indexes only if we know
		 * that we may make use of them. This of course includes
		 * (mode == PR_MODE_HTTP).
		 */
		s->txn.hdr_idx.size = MAX_HTTP_HDR;

		if (unlikely((s->txn.hdr_idx.v = pool_alloc2(s->fe->hdr_idx_pool)) == NULL))
			goto out_free_rspcap; /* no memory */

		/* and now initialize the HTTP transaction state */
		http_init_txn(s);
	}

	if ((s->fe->mode == PR_MODE_TCP || s->fe->mode == PR_MODE_HTTP)
	    && (s->fe->logfac1 >= 0 || s->fe->logfac2 >= 0)) {
		if (likely(s->fe->to_log)) {
			/* we have the client ip */
			if (s->logs.logwait & LW_CLIP)
				if (!(s->logs.logwait &= ~LW_CLIP))
					s->do_log(s);
		}
		else if (s->cli_addr.ss_family == AF_INET) {
			char pn[INET_ADDRSTRLEN], sn[INET_ADDRSTRLEN];

			if (!(s->flags & SN_FRT_ADDR_SET))
				get_frt_addr(s);

			if (inet_ntop(AF_INET, (const void *)&((struct sockaddr_in *)&s->frt_addr)->sin_addr,
				      sn, sizeof(sn)) &&
			    inet_ntop(AF_INET, (const void *)&((struct sockaddr_in *)&s->cli_addr)->sin_addr,
				      pn, sizeof(pn))) {
				send_log(s->fe, LOG_INFO, "Connect from %s:%d to %s:%d (%s/%s)\n",
					 pn, ntohs(((struct sockaddr_in *)&s->cli_addr)->sin_port),
					 sn, ntohs(((struct sockaddr_in *)&s->frt_addr)->sin_port),
					 s->fe->id, (s->fe->mode == PR_MODE_HTTP) ? "HTTP" : "TCP");
			}
		}
		else {
			char pn[INET6_ADDRSTRLEN], sn[INET6_ADDRSTRLEN];

			if (!(s->flags & SN_FRT_ADDR_SET))
				get_frt_addr(s);

			if (inet_ntop(AF_INET6, (const void *)&((struct sockaddr_in6 *)&s->frt_addr)->sin6_addr,
				      sn, sizeof(sn)) &&
			    inet_ntop(AF_INET6, (const void *)&((struct sockaddr_in6 *)&s->cli_addr)->sin6_addr,
				      pn, sizeof(pn))) {
				send_log(s->fe, LOG_INFO, "Connect from %s:%d to %s:%d (%s/%s)\n",
					 pn, ntohs(((struct sockaddr_in6 *)&s->cli_addr)->sin6_port),
					 sn, ntohs(((struct sockaddr_in6 *)&s->frt_addr)->sin6_port),
					 s->fe->id, (s->fe->mode == PR_MODE_HTTP) ? "HTTP" : "TCP");
			}
		}
	}

	if (unlikely((global.mode & MODE_DEBUG) && (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE)))) {
		int len;

		if (!(s->flags & SN_FRT_ADDR_SET))
			get_frt_addr(s);

		if (s->cli_addr.ss_family == AF_INET) {
			char pn[INET_ADDRSTRLEN];
			inet_ntop(AF_INET,
				  (const void *)&((struct sockaddr_in *)&s->cli_addr)->sin_addr,
				  pn, sizeof(pn));

			len = sprintf(trash, "%08x:%s.accept(%04x)=%04x from [%s:%d]\n",
				      s->uniq_id, s->fe->id, (unsigned short)s->listener->fd, (unsigned short)cfd,
				      pn, ntohs(((struct sockaddr_in *)&s->cli_addr)->sin_port));
		}
		else {
			char pn[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6,
				  (const void *)&((struct sockaddr_in6 *)(&s->cli_addr))->sin6_addr,
				  pn, sizeof(pn));

			len = sprintf(trash, "%08x:%s.accept(%04x)=%04x from [%s:%d]\n",
				      s->uniq_id, s->fe->id, (unsigned short)s->listener->fd, (unsigned short)cfd,
				      pn, ntohs(((struct sockaddr_in6 *)(&s->cli_addr))->sin6_port));
		}

		write(1, trash, len);
	}

	if (s->fe->mode == PR_MODE_HTTP)
		s->req->flags |= BF_READ_DONTWAIT; /* one read is usually enough */

	/* note: this should not happen anymore since there's always at least the switching rules */
	if (!s->req->analysers) {
		buffer_auto_connect(s->req);  /* don't wait to establish connection */
		buffer_auto_close(s->req);    /* let the producer forward close requests */
	}

	s->req->rto = s->fe->timeout.client;
	s->rep->wto = s->fe->timeout.client;

	fdtab[cfd].flags = FD_FL_TCP | FD_FL_TCP_NODELAY;
	if (s->fe->options & PR_O_TCP_NOLING)
		fdtab[cfd].flags |= FD_FL_TCP_NOLING;

	if (unlikely((s->fe->mode == PR_MODE_HTTP && (s->flags & SN_MONITOR)) ||
		     (s->fe->mode == PR_MODE_HEALTH && (s->fe->options & PR_O_HTTP_CHK)))) {
		/* Either we got a request from a monitoring system on an HTTP instance,
		 * or we're in health check mode with the 'httpchk' option enabled. In
		 * both cases, we return a fake "HTTP/1.0 200 OK" response and we exit.
		 */
		struct chunk msg;
		chunk_initstr(&msg, "HTTP/1.0 200 OK\r\n\r\n");
		stream_int_retnclose(&s->si[0], &msg); /* forge a 200 response */
		s->req->analysers = 0;
		s->task->expire = s->rep->wex;
		EV_FD_CLR(cfd, DIR_RD);
	}
	else if (unlikely(s->fe->mode == PR_MODE_HEALTH)) {  /* health check mode, no client reading */
		struct chunk msg;
		chunk_initstr(&msg, "OK\n");
		stream_int_retnclose(&s->si[0], &msg); /* forge an "OK" response */
		s->req->analysers = 0;
		s->task->expire = s->rep->wex;
		EV_FD_CLR(cfd, DIR_RD);
	}
	/* everything's OK, let's go on */
	return 1;

	/* Error unrolling */
 out_free_rspcap:
	pool_free2(s->fe->rsp_cap_pool, s->txn.rsp.cap);
 out_free_reqcap:
	pool_free2(s->fe->req_cap_pool, s->txn.req.cap);
 out_delete_cfd:
	fd_delete(cfd);
	return -1;
}

/* set test->i to the id of the frontend */
static int
acl_fetch_fe_id(struct proxy *px, struct session *l4, void *l7, int dir,
                struct acl_expr *expr, struct acl_test *test) {

	test->flags = ACL_TEST_F_READ_ONLY;

	test->i = l4->fe->uuid;

	return 1;
}

/* set test->i to the number of connections per second reaching the frontend */
static int
acl_fetch_fe_sess_rate(struct proxy *px, struct session *l4, void *l7, int dir,
                       struct acl_expr *expr, struct acl_test *test)
{
	test->flags = ACL_TEST_F_VOL_TEST;
	if (expr->arg_len) {
		/* another proxy was designated, we must look for it */
		for (px = proxy; px; px = px->next)
			if ((px->cap & PR_CAP_FE) && !strcmp(px->id, expr->arg.str))
				break;
	}
	if (!px)
		return 0;

	test->i = read_freq_ctr(&px->fe_sess_per_sec);
	return 1;
}

/* set test->i to the number of concurrent connections on the frontend */
static int
acl_fetch_fe_conn(struct proxy *px, struct session *l4, void *l7, int dir,
		  struct acl_expr *expr, struct acl_test *test)
{
	test->flags = ACL_TEST_F_VOL_TEST;
	if (expr->arg_len) {
		/* another proxy was designated, we must look for it */
		for (px = proxy; px; px = px->next)
			if ((px->cap & PR_CAP_FE) && !strcmp(px->id, expr->arg.str))
				break;
	}
	if (!px)
		return 0;

	test->i = px->feconn;
	return 1;
}


/* Note: must not be declared <const> as its list will be overwritten */
static struct acl_kw_list acl_kws = {{ },{
	{ "fe_id",        acl_parse_int, acl_fetch_fe_id,        acl_match_int, ACL_USE_NOTHING },
	{ "fe_sess_rate", acl_parse_int, acl_fetch_fe_sess_rate, acl_match_int, ACL_USE_NOTHING },
	{ "fe_conn",      acl_parse_int, acl_fetch_fe_conn,      acl_match_int, ACL_USE_NOTHING },
	{ NULL, NULL, NULL, NULL },
}};


__attribute__((constructor))
static void __frontend_init(void)
{
	acl_register_keywords(&acl_kws);
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */