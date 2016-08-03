/* Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <rpc/rpc.h>

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "xcom_common.h"
#include "x_platform.h"
#include "simset.h"
#include "xcom_vp.h"
#include "task.h"
#include "task_os.h"
#include "task_debug.h"
#include "node_no.h"
#include "server_struct.h"
#include "xcom_detector.h"
#include "site_struct.h"
#include "node_connection.h"
#include "xcom_transport.h"
#include "xcom_statistics.h"
#include "xcom_base.h"
#include "xcom_vp_str.h"
#include "xcom_msg_queue.h"
#include "xcom_memory.h"
#include "site_def.h"

#ifdef XCOM_HAVE_OPENSSL
#include "openssl/ssl.h"
#endif
#include "sock_probe.h"
#include "retry.h"
#ifdef XCOM_HAVE_OPENSSL
#include "xcom_ssl_transport.h"
#endif

#define MY_XCOM_PROTO x_1_1

xcom_proto const my_min_xcom_version = x_1_0; /* The minimum protocol version I am able to understand */
xcom_proto const my_xcom_version = MY_XCOM_PROTO; /* The maximun protocol version I am able to understand */

/* #define XCOM_ECM */

#define SERVER_MAX (2*NSERVERS)

/* Turn Nagle's algorithm on or off */
static int const NAGLE = 0;

extern int	xcom_shutdown;

static void shut_srv(server *s);

static int	xcom_port = 0; /* Port used by xcom */

static xcom_socket_accept_cb xcom_socket_accept_callback= NULL;

static int pm(int port)
{
	return port == xcom_port;
}

int set_xcom_socket_accept_cb(xcom_socket_accept_cb x)
{
  xcom_socket_accept_callback= x;
  return 1;
}

void init_xcom_transport(int listen_port)
{
    xcom_port = listen_port;
	if(get_port_matcher() == 0)
		set_port_matcher(pm);
}


void reset_srv_buf(srv_buf *sb)
{
	sb->start = 0;
	sb->n = 0;
}


/* Note that channel is alive */
static void alive(server *s)
{
	if (s) {
		s->active = task_now();
	}
}



static u_int srv_buf_capacity(srv_buf *sb)
{
	return sizeof(sb->buf);
}

static u_int srv_buf_free_space(srv_buf *sb)
{
	return sizeof(sb->buf) - sb->n;
}

static u_int srv_buf_buffered(srv_buf *sb)
{
	return sb->n - sb->start;
}

static char	*srv_buf_extract_ptr(srv_buf *sb)
{
	return & sb->buf[sb->start];
}


static char	*srv_buf_insert_ptr(srv_buf *sb)
{
	return & sb->buf[sb->n];
}


static inline void advance_extract_ptr(srv_buf *sb, u_int len)
{
	sb->start += len;
}

static u_int get_srv_buf(srv_buf *sb, char *data, u_int len)
{
	if(len > srv_buf_buffered(sb)){
		len = srv_buf_buffered(sb);
	}

	memcpy(data, srv_buf_extract_ptr(sb), len);
	advance_extract_ptr(sb, len);
	return len;
}

static inline void advance_insert_ptr(srv_buf *sb, u_int len)
{
	sb->n += len;
}

static u_int put_srv_buf(srv_buf *sb, char *data, u_int len)
{
	assert(sb->n + len <= sizeof(sb->buf));
	memcpy(srv_buf_insert_ptr(sb), data, len);
	advance_insert_ptr(sb, len);
	return len;
}


int	flush_srv_buf(server *s, ssize_t *ret)
{
	DECL_ENV
	    u_int buflen;
	END_ENV;

	TASK_BEGIN
	    ep->buflen = s->out_buf.n;
	reset_srv_buf(&s->out_buf);
	if (s->con.fd >= 0) {
		ssize_t	sent = 0;
		if (ep->buflen) {
			/* DBGOUT(FN; PTREXP(stack); NDBG(ep->buflen, u)); */
			/* LOCK_FD(s->con.fd, 'w'); */
			TASK_CALL(task_write(&s->con, s->out_buf.buf, ep->buflen, &sent));
			/* UNLOCK_FD(s->fd, 'w'); */
			if (sent <= 0) {
				shutdown_connection(&s->con);
			}
		}
		TASK_RETURN(sent);
	} else {
		TASK_FAIL;
	}

	FINALLY
	    TASK_END;
}

/* Send a message to server s */
static int	_send_msg(server *s, pax_msg *p, node_no to, ssize_t *ret)
{
	DECL_ENV
	    u_int	buflen;
	char	*buf;
	END_ENV;

	TASK_BEGIN
		p->to = to;
	MAY_DBG(FN; PTREXP(stack); PTREXP(s); PTREXP(p); NDBG(s->con.fd, d));
	MAY_DBG(FN;
			STREXP(s->srv);
			NDBG(s->port, d);
			NDBG(task_now(), f);
			COPY_AND_FREE_GOUT(dbg_pax_msg(p));
			);
	if (to == p->from) {
		MAY_DBG(FN;
				COPY_AND_FREE_GOUT(dbg_pax_msg(p)); );
		dispatch_op(find_site_def(p->synode), p, NULL);
		TASK_RETURN(sizeof(*p));
	} else {
		if (s->con.fd >= 0) {
			ssize_t	sent;
			/* LOCK_FD(s->con.fd, 'w'); */
			serialize_msg(p, s->con.x_proto, &ep->buflen, &ep->buf);
			if(ep->buflen){
				/* Not enough space? Flush the buffer */
				if (ep->buflen > srv_buf_free_space(&s->out_buf)) {
					TASK_CALL(flush_srv_buf(s, ret));
					if (s->con.fd < 0) {
						TASK_FAIL;
					}
					/* Still not enough? Message must be huge, send without buffering */
					if (ep->buflen > srv_buf_free_space(&s->out_buf)) {
						DBGOUT(FN; STRLIT("task_write"));
						TASK_CALL(task_write(&s->con, ep->buf, ep->buflen, &sent));
						if (s->con.fd < 0) {
							TASK_FAIL;
						}
					} else { /* Buffer the write */
						put_srv_buf(&s->out_buf, ep->buf, ep->buflen);
						sent = ep->buflen;
					}
				} else { /* Buffer the write */
					put_srv_buf(&s->out_buf, ep->buf, ep->buflen);
					sent = ep->buflen;
				}
				send_count[p->op]++;
				send_bytes[p->op] += ep->buflen;
				alive(s); /* Note activity */
				/* DBGOUT(STRLIT("sent message "); STRLIT(pax_op_to_str(p->op)); */
				/*        NDBG(p->from,d); NDBG(p->to,d); */
				/*        SYCEXP(p->synode);  */
				/*        BALCEXP(p->proposal)); */
				X_FREE(ep->buf);
				/* UNLOCK_FD(s->con.fd, 'w'); */
				if (sent <= 0) {
					shutdown_connection(&s->con);
				}
			}
			TASK_RETURN(sent);
		} else
			TASK_FAIL;
	}
	FINALLY
	    if (ep->buf)
			X_FREE(ep->buf);
	TASK_END;
}


void write_protoversion(unsigned char *buf, xcom_proto proto_vers)
{
	put_32(VERS_PTR(buf), proto_vers);
}

xcom_proto read_protoversion(unsigned char *p)
{
	return get_32(p);
}

int	check_protoversion(xcom_proto x_proto, xcom_proto negotiated)
{
	if(x_proto != negotiated){
		DBGOUT(FN; STRLIT(" found XCOM protocol version ");
			   NDBG(x_proto,d); STRLIT(" need version ");
			   NDBG(negotiated,d); );

		return 0;
	}
	return 1;
}


/* Send a protocol negotiation message on connection con */
int	send_proto(connection_descriptor *con, xcom_proto x_proto, x_msg_type x_type, unsigned int tag, ssize_t *ret)
{
	DECL_ENV
	char	buf[MSG_HDR_SIZE];
	END_ENV;

	TASK_BEGIN
		if (con->fd >= 0) {
			con->snd_tag = tag;
			write_protoversion(VERS_PTR((unsigned char*) ep->buf), x_proto);
			put_header_1_0((unsigned char*) ep->buf, 0, x_type, tag);

			{
				ssize_t	sent;

				TASK_CALL(task_write(con, ep->buf, MSG_HDR_SIZE, &sent));
				if (con->fd < 0) {
					TASK_FAIL;
				}
				if (sent <= 0) {
					shutdown_connection(con);
				}
				TASK_RETURN(sent);
			}
		} else {
			TASK_FAIL;
		}
	FINALLY

		TASK_END;
}

int apply_xdr(xcom_proto x_proto, gpointer buff, size_t bufflen,
			   xdrproc_t xdrfunc, void *xdrdata,
			   enum xdr_op op)
{
	XDR xdr;
	int	MY_ATTRIBUTE ((unused)) s = 0;

	xdr.x_ops = NULL;
	xdrmem_create(&xdr, buff, bufflen, op);
	/*
	  Mac OSX changed the xdrproc_t prototype to take
	  three parameters instead of two.

	  The argument is that it has the potential to break
	  the ABI due to compiler optimizations.

	  The recommended value for the third parameter is
	  0 for those that are not making use of it (which
	  is the case). This will keep this code cross-platform
	  and cross-version compatible.
	*/
	if (xdr.x_ops){
		xdr.x_public = (caddr_t)&x_proto; /* Supply protocol version in user field of xdr */
		s = xdrfunc(&xdr, xdrdata, 0);
	}
	xdr_destroy(&xdr);
	return s;
}


#if TASK_DBUG_ON
static void dump_header(char *buf)
{
	char	*end = buf + MSG_HDR_SIZE;
	GET_GOUT;
	STRLIT("message header ");
	PTREXP(buf);
	while (buf < end) {
		NPUT(*buf, x);
		buf++;
	}
  PRINT_GOUT;
  FREE_GOUT;
}
#endif

void dbg_app_data(app_data_ptr a);

/* Ugly trick to see if _RPC_RPC_H is empty */
#if defined(_RPC_RPC_H) && (314 - _RPC_RPC_H - 314 == 628)
#define OLD_XDR
#endif

#ifdef HAVE___CONST
#define const __const
#else
#ifdef OLD_XDR
#define const
#endif
#endif

/* ARGSUSED */
static bool_t
x_putlong (XDR *xdrs,  const long *longp)
{
  xdrs->x_handy += BYTES_PER_XDR_UNIT;
  return TRUE;
}

/* ARGSUSED */
#ifdef OLD_XDR
static bool_t
x_putbytes (XDR *xdrs, const char *bp, int len)
{
  xdrs->x_handy += len;
  return TRUE;
}
#else
static bool_t
x_putbytes (XDR *xdrs, const char *bp, u_int len)
{
  xdrs->x_handy += len;
  return TRUE;
}

#endif


static u_int
x_getpostn (const XDR *xdrs)
{
  return xdrs->x_handy;
}

/* ARGSUSED */
static bool_t
x_setpostn (XDR *xdrs, u_int len)
{
  /* This is not allowed */
  return FALSE;
}

#ifdef HAVE_RPC_INLINE_T
#define INLINE_T rpc_inline_t
#else
#define INLINE_T int32_t
#endif

#ifdef OLD_XDR
static INLINE_T *
x_inline (XDR *xdrs, int len)
#else
static INLINE_T *
x_inline (XDR *xdrs, u_int len)
#endif
{
	if (len == 0)
		return NULL;
	if (xdrs->x_op != XDR_ENCODE)
		return NULL;
	if ((u_int)len < (u_int) (long int) xdrs->x_base) {
		/* x_private was already allocated */
		xdrs->x_handy += len;
		return (INLINE_T * ) xdrs->x_private;
	} else {
		/* Free the earlier space and allocate new area */
		free (xdrs->x_private);
		if ((xdrs->x_private = (caddr_t) malloc (len)) == NULL) {
			xdrs->x_base = 0;
			return NULL;
		}
		xdrs->x_base = (void * ) (long) len;
		xdrs->x_handy += len;
		return (INLINE_T * ) xdrs->x_private;
	}
}
#undef INLINE_T

static int
harmless (void)
{
  /* Always return FALSE/NULL, as the case may be */
  return 0;
}

static void
x_destroy (XDR *xdrs)
{
  xdrs->x_handy = 0;
  xdrs->x_base = 0;
  if (xdrs->x_private)
    {
      free (xdrs->x_private);
      xdrs->x_private = NULL;
    }
  return;
}

static bool_t
x_putint32 (XDR *xdrs, const int32_t *int32p)
{
  xdrs->x_handy += BYTES_PER_XDR_UNIT;
  return TRUE;
}

static unsigned long
xdr_proto_sizeof (xcom_proto x_proto, xdrproc_t func, void *data)
{
  XDR x;
  struct xdr_ops ops;
  bool_t stat;
  /* to stop ANSI-C compiler from complaining */
  typedef bool_t (*dummyfunc1) (XDR *, long *);
  typedef bool_t (*dummyfunc3) (XDR *, int32_t *);

#ifdef OLD_XDR
  typedef bool_t (*dummyfunc2) (XDR *, caddr_t, int);
#else
  typedef bool_t (*dummyfunc2) (XDR *, caddr_t, u_int);
#endif

  ops.x_putlong = x_putlong;
  ops.x_putbytes = x_putbytes;
  ops.x_inline = x_inline;
  ops.x_getpostn = x_getpostn;
  ops.x_setpostn = x_setpostn;
  ops.x_destroy = x_destroy;

#ifdef HAVE_XDR_OPS_X_PUTINT32
  ops.x_putint32 = x_putint32;
#endif
  /* the other harmless ones */
  ops.x_getlong = (dummyfunc1) harmless;
  ops.x_getbytes = (dummyfunc2) harmless;
#ifdef HAVE_XDR_OPS_X_GETINT32
  ops.x_getint32 = (dummyfunc3) harmless;
#endif
  x.x_op = XDR_ENCODE;
  x.x_ops = &ops;
  x.x_handy = 0;
  x.x_private = (caddr_t) NULL;
  x.x_base = (caddr_t) 0;
  x.x_public = (caddr_t)&x_proto;

  /*
    Mac OSX changed the xdrproc_t prototype to take
    three parameters instead of two.

    The argument is that it has the potential to break
    the ABI due to compiler optimizations.

    The recommended value for the third parameter is
    0 for those that are not making use of it (which
    is the case). This will keep this code cross-platform
    and cross-version compatible.
  */
  stat = func (&x, data, 0);
  free (x.x_private);
  return stat == TRUE ? x.x_handy : 0;
}

#ifdef OLD_XDR
#undef const
#endif


static int serialize(void *p, xcom_proto x_proto, u_int *out_len, xdrproc_t xdrfunc, char **out_buf)
{
	unsigned char	*buf = NULL;
	size_t msg_buflen = 0;
	size_t tot_buflen = 0;
	unsigned int	tag = 0;
	x_msg_type x_type = x_normal;
	int retval = 0;

	/* Find length of serialized message */
	msg_buflen = xdr_proto_sizeof(x_proto, xdrfunc, p);
	assert(msg_buflen > 0);
	tot_buflen = SERIALIZED_BUFLEN(msg_buflen);
	MAY_DBG(FN; NDBG(msg_buflen, d); NDBG(tot_buflen, d));
	
	/* Allocate space for version number, length field, type, tag, and serialized message */
	buf = calloc(1, tot_buflen);
	if (buf) {
		/* Write protocol version */
		write_protoversion(buf, x_proto);
		
		/* Serialize message */
		retval = apply_xdr(x_proto, MSG_PTR(buf), msg_buflen,
						   xdrfunc,
						   p, XDR_ENCODE);
		if(retval){
			/* Serialize header into buf */
			put_header_1_0(buf, msg_buflen, x_type, tag);
		}
	}
	*out_len = tot_buflen;
	*out_buf = (char * )buf;
	MAY_DBG(FN; NDBG(*out_len, u); PTREXP(*out_buf);
			dump_header(*out_buf));
	return retval;
}


/* Version 1 has no new messages, only modified, so all should be sent */
static inline int old_proto_knows(xcom_proto x_proto, pax_op op)
{
	return 1;
}

int serialize_msg(pax_msg *p, xcom_proto x_proto, u_int *buflen, char **buf)
{
	*buflen = 0;
	*buf = 0;

	return old_proto_knows(x_proto, p->op) &&
		serialize((void * )p, x_proto, buflen, (xdrproc_t)xdr_pax_msg, buf);
}

int deserialize_msg(pax_msg *p, xcom_proto x_proto,  char *buf, size_t buflen)
{
	int apply_ok = apply_xdr(x_proto, buf, buflen,
						   (xdrproc_t)xdr_pax_msg,
						   (void * )p, XDR_DECODE);
	if(!apply_ok){
		my_xdr_free((xdrproc_t)xdr_pax_msg,
					(char * )p);
	}
	return apply_ok;
}

/* Better checksum */
static uint32_t crc_table[256];

void init_crc32c()
{
	uint32_t i;
	for (i = 0; i < 256; i++) {
		int	j;
		uint32_t c = i;
		for (j = 0; j < 8; j++) {
			c = (c & 1) ? (0x82F63B78 ^ (c >> 1)) : (c >> 1);
		}
		crc_table[i] = c;
	}
}


#define CRC32CSTART 0xFFFFFFFF

uint32_t crc32c_hash(char *buf, char *end)
{
	uint32_t c = CRC32CSTART;
	unsigned char	*p = (unsigned char*)buf;
	unsigned char	*e = (unsigned char*)end;
	for (; p < e; p++) {
		c = crc_table[(c ^ (*p)) & 0xFF] ^ (c >> 8);
	}
	return c ^ 0xFFFFFFFF;
}


/* {{{ Paxos servers (nodes) */

/* Array of servers, only maxservers entries actually used */
static server *all_servers[SERVER_MAX];
static int	maxservers = 0;

/* Create a new server */
static server *
mksrv(char *srv, int port)
{
	server * s;

	s = calloc(1, sizeof (* s));

	DBGOUT(FN; PTREXP(s); STREXP(srv));
	if (s == 0) {
		g_critical("out of memory");
		abort();
	}
	s->garbage = 0;
	s->refcnt = 0;
	s->srv = srv;
	s->port = port;
	reset_connection(&s->con);
	s->active = 0.0;
	s->detected = 0.0;
	channel_init(&s->outgoing, type_hash("msg_link"));
	DBGOUT(FN; STREXP(srv); NDBG(port,d));
	if (xcom_mynode_match(srv, port)) { /* Short-circuit local messages */
		DBGOUT(FN; STRLIT("creating local sender"); STREXP(srv); NDBG(port,d));
		s->sender = task_new(local_sender_task, void_arg(s), "local_sender_task", XCOM_THREAD_DEBUG);
	}else{
		s->sender = task_new(sender_task, void_arg(s), "sender_task", XCOM_THREAD_DEBUG);
		DBGOUT(FN; STRLIT("creating sender and reply_handler"); STREXP(srv); NDBG(port,d));
		s->reply_handler = task_new(reply_handler_task, void_arg(s), "reply_handler_task", XCOM_THREAD_DEBUG);
	}
	reset_srv_buf(&s->out_buf);
	return s;
}


static server *addsrv(char *srv, int port)
{
	server * s = mksrv(srv, port);
	assert(all_servers[maxservers] == 0);
	assert(maxservers < SERVER_MAX);
	all_servers[maxservers] = s;
	MAY_DBG(FN; PTREXP(all_servers[maxservers]); STREXP(all_servers[maxservers]->srv); NDBG(all_servers[maxservers]->port, d); NDBG(maxservers, d));
	maxservers++;
	return s;
}


static void rmsrv(int i)
{
	assert(all_servers[i]);
	assert(maxservers > 0);
	assert(i < maxservers);
	MAY_DBG(FN; PTREXP(all_servers[i]); STREXP(all_servers[i]->srv); NDBG(all_servers[i]->port, d); NDBG(i, d));
	maxservers--;
	all_servers[i] = all_servers[maxservers];
	all_servers[maxservers] = 0;
}


static void	init_collect()
{
	int	i;

	for (i = 0; i < maxservers; i++) {
		assert(all_servers[i]);
		all_servers[i]->garbage = 1;
	}
}


extern void	get_all_site_defs(site_def ***s, uint32_t *n);

static void mark_site_servers(site_def *site)
{
	u_int i;
	for (i = 0; i < get_maxnodes(site); i++) {
		server * s = site->servers[i];
		assert(s);
		s->garbage = 0;
	}
}


static void	mark()
{
	site_def * *site;
	uint32_t	n;
	uint32_t	i;

	get_all_site_defs(&site, &n);

	for (i = 0; i < n; i++) {
		if (site[i]) {
			mark_site_servers(site[i]);
		}
	}
}


static void	sweep()
{
	int	i = 0;
	while (i < maxservers) {
		server *s = all_servers[i];
		assert(s);
		if (s->garbage) {
			DBGOUT(FN; STREXP(s->srv));
			shut_srv(s);
			rmsrv(i);
		} else {
			i++;
		}
	}
}


void garbage_collect_servers()
{
	DBGOUT(FN);
	init_collect();
	mark();
	sweep();
}


/* Free a server */
static void freesrv(server *s)
{
	X_FREE(s->srv);
	X_FREE(s);
}


double	server_active(site_def const *s, node_no i)
{
	if (s->servers[i])
		return s->servers[i]->active;
	else
		return 0.0;
}


/* Shutdown server */
static void shut_srv(server *s)
{
	if (!s)
		return;
	DBGOUT(FN; PTREXP(s); STREXP(s->srv));

	shutdown_connection(&s->con);

	/* Tasks will free the server object when they terminate */
	if (s->sender)
		task_terminate(s->sender);
	if (s->reply_handler)
		task_terminate(s->reply_handler);
}


int	srv_ref(server *s)
{
	assert(s->refcnt >= 0);
	s->refcnt++;
	return s->refcnt;
}


int	srv_unref(server *s)
{
	assert(s->refcnt >= 0);
	s->refcnt--;
	if (s->refcnt == 0) {
		freesrv(s);
		return 0;
	}
	return s->refcnt;
}


/* }}} */

/* Listen for connections on socket and create a handler task */
int	tcp_server(task_arg arg)
{
	DECL_ENV
	    int	fd;
	int	cfd;
	int refused;
	END_ENV;
	TASK_BEGIN
	    ep->fd = get_int_arg(arg);
	ep->refused= 0;
	unblock_fd(ep->fd);
	DBGOUT(FN; NDBG(ep->fd, d); );
	do {
		TASK_CALL(accept_tcp(ep->fd, &ep->cfd));
                /* Callback to check that the file descriptor is accepted. */
                if (xcom_socket_accept_callback && !xcom_socket_accept_callback(ep->cfd))
                {
                  shut_close_socket(&ep->cfd);
                  ep->cfd= -1;
                  ep->refused= 1;
                  TASK_YIELD;
                  continue;
                }
                ep->refused= 0;
		DBGOUT(FN; NDBG(ep->cfd, d); );
		task_new(acceptor_learner_task, int_arg(ep->cfd), "acceptor_learner_task", XCOM_THREAD_DEBUG);
	} while (!xcom_shutdown && (ep->cfd >= 0 || ep->refused));
	FINALLY
	assert(ep->fd >= 0);
	shut_close_socket(&ep->fd);
	TASK_END;
}

#ifdef XCOM_HAVE_OPENSSL
#define SSL_CONNECT(con, hostname) {									\
		con.ssl_fd = SSL_new(client_ctx);								\
		SSL_set_fd(con.ssl_fd, con.fd);									\
		ret.val = SSL_connect(con.ssl_fd);								\
		ret.funerr = to_ssl_err(SSL_get_error(con.ssl_fd, ret.val));	\
		while (ret.val != SSL_SUCCESS && can_retry(ret.funerr)) {		\
			if (from_ssl_err(ret.funerr) == SSL_ERROR_WANT_READ){		\
				wait_io(stack, con.fd, 'r');							\
			}else if (from_ssl_err(ret.funerr) == SSL_ERROR_WANT_WRITE){ \
				wait_io(stack, con.fd, 'w');							\
			}else{														\
				break;													\
			}															\
			TASK_YIELD;													\
			SET_OS_ERR(0);												\
			if (con.fd < 0) {											\
				ssl_free_con(&con);										\
				close_connection(&con);									\
				TERMINATE;												\
			}															\
																		\
			ret.val = SSL_connect(con.ssl_fd);							\
			ret.funerr = to_ssl_err(SSL_get_error(con.ssl_fd, ret.val)); \
		}																\
            															\
		if (ret.val != SSL_SUCCESS) {									\
			ssl_free_con(&con);											\
			close_connection(&con);										\
			TERMINATE;													\
		}else{															\
			if (ssl_verify_server_cert(con.ssl_fd, hostname))			\
			{															\
				ssl_free_con(&con);										\
				close_connection(&con);									\
				TERMINATE;												\
			}															\
			set_connected(&con, CON_FD);										\
		}																\
	}
#endif

/* Try to connect to another node */
static int	dial(server *s)
{
	DECL_ENV
	    int	dummy;
	END_ENV;

	TASK_BEGIN
	    DBGOUT(FN; STRLIT(" dial "); NPUT(get_nodeno(get_site_def()), u); STRLIT(s->srv); NDBG(s->port, d));
	TASK_CALL(connect_tcp(s->srv, s->port, &s->con.fd));
	/* DBGOUT(FN; NDBG(s->con.fd,d);); */
	if (s->con.fd < 0) {
		DBGOUT(FN; STRLIT("could not dial "); STRLIT(s->srv); NDBG(s->port, d); );
	} else {
		if (NAGLE == 0) {
			set_nodelay(s->con.fd);
		}

		unblock_fd(s->con.fd);
#ifdef XCOM_HAVE_OPENSSL
		if (xcom_use_ssl()) {
			result ret = {0,0};
			SSL_CONNECT(s->con, s->srv);
		}
#endif
		DBGOUT(FN; STRLIT("connected to "); STRLIT(s->srv); NDBG(s->con.fd, d); NDBG(s->port, d));
		set_connected(&s->con, CON_FD);
		alive(s);
	}
	FINALLY
	    TASK_END;
}


/* Send message by putting it in the server queue */
int	send_msg(server *s, node_no from, node_no to, uint32_t group_id, pax_msg *p)
{
	assert(p);
	assert(s);
	 {
		msg_link * link = msg_link_new(p, to);
		alive(s); /* Note activity */
		MAY_DBG(FN; PTREXP(&s->outgoing);
		    COPY_AND_FREE_GOUT(dbg_msg_link(link));
		    );
		p->from = from;
		p->to = to;
		p->group_id = group_id;
		p->max_synode = get_max_synode();
 		MAY_DBG(FN; PTREXP(p); STREXP(s->srv); NDBG(p->from, d); NDBG(p->to, d); NDBG(p->group_id, u));
		channel_put(&s->outgoing, &link->l);
	}
	return 0;
}


static inline int	_send_server_msg(site_def const *s, node_no to, pax_msg *p)
{
	assert(s);
	assert(s->servers[to]);
	if (s->servers[to] && p) {
		send_msg(s->servers[to], s->nodeno, to, get_group_id(s), p);
	}
	return 0;
}


int send_server_msg(site_def const *s, node_no to, pax_msg *p)
{
	return _send_server_msg(s, to, p);
}

static inline int send_loop(site_def const *s, node_no max, pax_msg *p, const char *dbg MY_ATTRIBUTE((unused)))
{
	int	retval = 0;
	assert(s);
	if (s) {
		node_no i = 0;
		for (i = 0; i < max; i++) {
			MAY_DBG(FN; STRLIT(dbg); STRLIT(" "); NDBG(i, u); NDBG(max, u); PTREXP(p));
			retval = _send_server_msg(s, i, p);
		}
	}
	return retval;
}


/* Send to all servers in site */
int	send_to_all_site(site_def const *s, pax_msg *p, const char *dbg)
{
	int	retval = 0;
	retval = send_loop(s, get_maxnodes(s), p, dbg);
	return retval;
}

/* Send to all servers */
int	send_to_all(pax_msg *p, const char *dbg)
{
	return send_to_all_site(find_site_def(p->synode), p, dbg);
}


static inline int send_other_loop(site_def const *s, pax_msg *p, const char *dbg MY_ATTRIBUTE((unused)))
{
	int	retval = 0;
	node_no i = 0;
#ifdef MAXACCEPT
	node_no max = MIN(get_maxnodes(s), MAXACCEPT);
#else
	node_no max;
	assert(s);
	max = get_maxnodes(s);
#endif
	for (i = 0; i < max; i++) {
		if (i != s->nodeno) {
			MAY_DBG(FN; STRLIT(dbg); STRLIT(" "); NDBG(i, u); NDBG(max, u); PTREXP(p));
			retval = _send_server_msg(s, i, p);
		}
	}
	return retval;
}


/* Send to other servers */
int	send_to_others(site_def const *s, pax_msg *p, const char *dbg)
{
	int	retval = 0;
	retval = send_other_loop(s, p, dbg);
	return retval;
}

/* Send to some other live server, round robin */
int	send_to_someone(site_def const *s, pax_msg *p, const char *dbg MY_ATTRIBUTE((unused)))
{
	int	retval = 0;
	static node_no i = 0;
	node_no prev = 0;
#ifdef MAXACCEPT
	node_no max = MIN(get_maxnodes(s), MAXACCEPT);
#else
	node_no max;
	assert(s);
	max = get_maxnodes(s);
#endif
	/* DBGOUT(FN; NDBG(max,u); NDBG(s->maxnodes,u)); */
	assert(max > 0);
	prev = i % max;
	i = (i + 1) % max;
	while (i != prev) {
		/* DBGOUT(FN; NDBG(i,u); NDBG(prev,u)); */
		if (i != s->nodeno && !may_be_dead(s->detected, i, task_now())) {
			MAY_DBG(FN; STRLIT(dbg); NDBG(i, u); NDBG(max, u); PTREXP(p));
			retval = _send_server_msg(s, i, p);
			break;
		}
		i = (i + 1) % max;
	}
	return retval;
}


#ifdef MAXACCEPT
/* Send to all acceptors */
int	send_to_acceptors(pax_msg *p, const char *dbg)
{
	site_def const *s = find_site_def(p->synode);
	int	retval = 0;
	int	i;
	retval = send_loop(s, MIN(MAXACCEPT, s->maxnodes), p, dbg);
	return retval;
}


#else
/* Send to all acceptors */
int	send_to_acceptors(pax_msg *p, const char *dbg)
{
	return send_to_all(p, dbg);
}


#endif

/* Used by :/int.*read_msg */
static int	read_bytes(connection_descriptor const * rfd, char *p, size_t n, ssize_t *ret)
{
	DECL_ENV
	    size_t left;
	char	*bytes;
	END_ENV;

	    ssize_t	nread = 0;

	TASK_BEGIN

	    ep->left = n;
	ep->bytes = (char *)p;
	while (ep->left > 0) {
		MAY_DBG(FN; NDBG(rfd->fd, d); NDBG(nread, d); NDBG(ep->left, d));
		TASK_CALL(task_read(rfd, ep->bytes, ep->left, &nread));
		MAY_DBG(FN; NDBG(rfd->fd, d); NDBG(nread, d); NDBG(ep->left, d));
		if (nread == 0) {
			TASK_RETURN(0);
		} else if (nread < 0) {
			DBGOUT(FN; NDBG(nread, d));
			TASK_FAIL;
		} else {
			ep->bytes += nread;
			ep->left -= nread;
		}
	}
	assert(ep->left == 0);
	TASK_RETURN(n);
	FINALLY
	    TASK_END;
}

static int	buffered_read_bytes(connection_descriptor const * rfd, srv_buf *buf, char *p, int n, ssize_t *ret)
{
	DECL_ENV
	    int	left;
	char	*bytes;
	END_ENV;
	int nget = 0;

	TASK_BEGIN
	ep->left = n;
	ep->bytes = (char *)p;

	/* First, try to get bytes from buffer */
	nget = get_srv_buf(buf, ep->bytes, n);
	ep->bytes += nget;
	ep->left -= nget;

	if((u_int)ep->left >= srv_buf_capacity(buf)){
		/* Too big, do direct read of rest */
		TASK_CALL(read_bytes(rfd, ep->bytes, ep->left, ret));
		if(*ret <= 0){
			TASK_FAIL;
		}
		ep->left -= *ret;
	}else{
		/* Buffered read makes sense */
		while(ep->left > 0){
			ssize_t	nread = 0;
			/* Buffer is empty, reset and read */
			reset_srv_buf(buf);
			MAY_DBG(FN; NDBG(rfd->fd, d); NDBG(nread, d););
			TASK_CALL(task_read(rfd, srv_buf_insert_ptr(buf), srv_buf_free_space(buf), &nread));
			MAY_DBG(FN; NDBG(rfd->fd, d); NDBG(nread, d););
			if (nread == 0) {
				TASK_RETURN(0);
			} else if (nread < 0) {
				DBGOUT(FN; NDBG(nread, d));
				TASK_FAIL;
			}else{
				advance_insert_ptr(buf, nread); /* Update buffer to reflect number of bytes read */
				nget = get_srv_buf(buf, ep->bytes, ep->left);
				ep->bytes += nget;
				ep->left -= nget;
			}
		}
	}
	assert(ep->left == 0);
	TASK_RETURN(n);
	FINALLY
	    TASK_END;
}

void get_header_1_0(unsigned char header_buf[], size_t *msgsize, x_msg_type *x_type, unsigned int *tag)
{
	*msgsize = get_32(LENGTH_PTR(header_buf));
	*x_type = header_buf[X_TYPE];
	*tag = get_16(X_TAG_PTR(header_buf));
}

void put_header_1_0(unsigned char header_buf[], size_t msgsize, x_msg_type x_type, unsigned int tag)
{
	put_32(LENGTH_PTR(header_buf), msgsize);
	header_buf[X_TYPE] = x_type;
	put_16(X_TAG_PTR(header_buf), tag);
}

/* See also :/static .*read_bytes */
int	read_msg(connection_descriptor * rfd, pax_msg *p, ssize_t *ret)
{
	int deserialize_ok = 0;

	DECL_ENV
	    ssize_t	n;
	char	*bytes;
	unsigned char	header_buf[MSG_HDR_SIZE];
	xcom_proto x_version;
	size_t	msgsize;
	x_msg_type x_type;
	unsigned int tag;
	END_ENV;

	TASK_BEGIN
	do{
		ep->bytes = NULL;
		/* Read length field, protocol version, and checksum */
		ep->n = 0;
		TASK_CALL(read_bytes(rfd, (char*)ep->header_buf, MSG_HDR_SIZE, &ep->n));

		if (ep->n != MSG_HDR_SIZE) {
			DBGOUT(FN; NDBG(ep->n, d));
			TASK_FAIL;
		}

		/* Check the protocol version before doing anything else */
		ep->x_version = read_protoversion(VERS_PTR(ep->header_buf));
		get_header_1_0(ep->header_buf, &ep->msgsize, & ep->x_type, &ep->tag);
		if(ep->x_type == x_version_req){
			/* Negotiation request. See what we can offer */
			rfd->x_proto = negotiate_protocol(ep->x_version);
			DBGOUT(STRLIT("incoming connection will use protcol version ");
			   NDBG(rfd->x_proto,u); STRLIT(xcom_proto_to_str(rfd->x_proto)));
			if(rfd->x_proto > my_xcom_version)
				TASK_FAIL;
			set_connected(rfd, CON_PROTO);
			TASK_CALL(send_proto(rfd, rfd->x_proto,  x_version_reply, ep->tag, ret));
		} else if (ep->x_type == x_version_reply){
			/* Mark connection with negotiated protocol version */
			if(rfd->snd_tag == ep->tag){
				rfd->x_proto = ep->x_version;
				DBGOUT(STRLIT("peer connection will use protcol version ");
					   NDBG(rfd->x_proto,u); STRLIT(xcom_proto_to_str(rfd->x_proto)));

				if(rfd->x_proto > my_xcom_version || rfd->x_proto == x_unknown_proto)
					TASK_FAIL;

				set_connected(rfd, CON_PROTO);
			}
		}
	}while(ep->x_type != x_normal);

#ifdef XCOM_PARANOID
	assert(check_protoversion(ep->x_version, rfd->x_proto));
#endif
	if (!check_protoversion(ep->x_version, rfd->x_proto)) {
		TASK_FAIL;
	}

	/* OK, we can grok this version */

	/* Allocate buffer space for message */
	ep->bytes = calloc(1, ep->msgsize);
	if(!ep->bytes){
		TASK_FAIL;
	}

	/* Read message */
	ep->n = 0;
	TASK_CALL(read_bytes(rfd, ep->bytes, ep->msgsize, &ep->n));

	if (ep->n > 0) {
		/* Deserialize message */
		deserialize_ok = deserialize_msg(p, rfd->x_proto, ep->bytes, ep->msgsize);
		MAY_DBG(FN; STRLIT(" deserialized message"));
	}
	/* Deallocate buffer */
	X_FREE(ep->bytes);
	if (ep->n <= 0 || !deserialize_ok) {
		DBGOUT(FN; NDBG(ep->n, d); NDBG(deserialize_ok,d));
		TASK_FAIL;
	}
	TASK_RETURN(ep->n);
	FINALLY
		TASK_END;
}

int	buffered_read_msg(connection_descriptor *rfd, srv_buf *buf, pax_msg *p, ssize_t *ret)
{
	int deserialize_ok = 0;

	DECL_ENV
	    ssize_t	n;
	char	*bytes;
	unsigned char	header_buf[MSG_HDR_SIZE];
	xcom_proto x_version;
	size_t	msgsize;
	x_msg_type x_type;
	unsigned int tag;
#ifdef NOTDEF
	unsigned int	check;
#endif
	END_ENV;

	TASK_BEGIN
	do{
		ep->bytes = NULL;
		/* Read length field, protocol version, and checksum */
		ep->n = 0;
		TASK_CALL(buffered_read_bytes(rfd, buf, (char*)ep->header_buf, MSG_HDR_SIZE, &ep->n));

		if (ep->n != MSG_HDR_SIZE) {
			DBGOUT(FN; NDBG(ep->n, d));
			TASK_FAIL;
		}

		/* Check the protocol version before doing anything else */
		ep->x_version = read_protoversion(VERS_PTR(ep->header_buf));
		get_header_1_0(ep->header_buf, &ep->msgsize, & ep->x_type, &ep->tag);
		if(ep->x_type == x_version_req){
			/* Negotiation request. See what we can offer */
			rfd->x_proto = negotiate_protocol(ep->x_version);
			DBGOUT(STRLIT("incoming connection will use protcol version ");
			   NDBG(rfd->x_proto,u); STRLIT(xcom_proto_to_str(rfd->x_proto)));
			if(rfd->x_proto > my_xcom_version)
				TASK_FAIL;
			set_connected(rfd, CON_PROTO);
			TASK_CALL(send_proto(rfd, rfd->x_proto,  x_version_reply, ep->tag, ret));
		} else if (ep->x_type == x_version_reply){
			/* Mark connection with negotiated protocol version */
			if(rfd->snd_tag == ep->tag){
				rfd->x_proto = ep->x_version;
				DBGOUT(STRLIT("peer connection will use protcol version ");
					   NDBG(rfd->x_proto,u); STRLIT(xcom_proto_to_str(rfd->x_proto)));
				if(rfd->x_proto > my_xcom_version || rfd->x_proto == x_unknown_proto)
					TASK_FAIL;

				set_connected(rfd, CON_PROTO);
			}
		}
	}while(ep->x_type != x_normal);

#ifdef XCOM_PARANOID
	assert(check_protoversion(ep->x_version, rfd->x_proto));
#endif
	if (!check_protoversion(ep->x_version, rfd->x_proto)) {
		TASK_FAIL;
	}

	/* OK, we can grok this version */

	/* Allocate buffer space for message */
	ep->bytes = calloc(1, ep->msgsize);
	if(!ep->bytes){
		TASK_FAIL;
	}
	/* Read message */
	ep->n = 0;
	TASK_CALL(buffered_read_bytes(rfd, buf, ep->bytes, ep->msgsize, &ep->n));

	if (ep->n > 0) {
		/* Deserialize message */
		deserialize_ok = deserialize_msg(p, rfd->x_proto, ep->bytes, ep->msgsize);
		MAY_DBG(FN; STRLIT(" deserialized message"));
	}
	/* Deallocate buffer */
	X_FREE(ep->bytes);
	if (ep->n <= 0 || !deserialize_ok) {
		DBGOUT(FN; NDBG(ep->n, d); NDBG(deserialize_ok,d));
		TASK_FAIL;
	}
	TASK_RETURN(ep->n);
	FINALLY
		TASK_END;
}

int	recv_proto(connection_descriptor const * rfd, xcom_proto *x_proto, x_msg_type *x_type, unsigned int *tag, ssize_t *ret)
{
	DECL_ENV
	    ssize_t	n;
	unsigned char	header_buf[MSG_HDR_SIZE];
	size_t	msgsize;
	END_ENV;

	TASK_BEGIN

	/* Read length field, protocol version, and checksum */
	ep->n = 0;
	TASK_CALL(read_bytes(rfd, (char*)ep->header_buf, MSG_HDR_SIZE, &ep->n));

	if (ep->n != MSG_HDR_SIZE) {
		DBGOUT(FN; NDBG(ep->n, d));
		TASK_FAIL;
	}

	*x_proto = read_protoversion(VERS_PTR(ep->header_buf));
	get_header_1_0(ep->header_buf, &ep->msgsize, x_type, tag);
	TASK_RETURN(ep->n);
	FINALLY
	    TASK_END;
}


/* }}} */

/* {{{ Sender task */

inline int tag_check(unsigned int tag1, unsigned int tag2)
{
	return (tag1 & 0xffff) == (tag2 & 0xffff);
}

static inline unsigned int incr_tag(unsigned int tag)
{
	++tag;
	return tag & 0xffff;
}

static void start_protocol_negotiation(channel *outgoing)
{
	msg_link * link = msg_link_new(0, -1);
	MAY_DBG(FN; PTREXP(outgoing);
			COPY_AND_FREE_GOUT(dbg_msg_link(link));
			);
	channel_put_front(outgoing, &link->l);
}

#define TAG_START 313

/* Fetch messages from queue and send to other server.  Having a
   separate queue and task for doing this simplifies the logic since we
   never need to wait to send. */
int	sender_task(task_arg arg)
{
	DECL_ENV
	    server * s;
	msg_link * link;
	unsigned int tag;
	END_ENV;

	TASK_BEGIN

	    ep->s = (server * )get_void_arg(arg);
	ep->link = NULL;
	ep->tag = TAG_START;
	srv_ref(ep->s);

	for(;;) {
		/* Loop until connected */
		while (!is_connected(&ep->s->con)) {
			TASK_CALL(dial(ep->s));
			if (ep->s->con.fd < 0) {
				TASK_DELAY(1.000);
			}
			empty_msg_channel(&ep->s->outgoing);
		}

		reset_srv_buf(&ep->s->out_buf);

		/* We are ready to start sending messages.
		   Insert a message in the input queue to negotiate the protocol.
		*/
		start_protocol_negotiation(&ep->s->outgoing);
		while (is_connected(&ep->s->con)) {

			ssize_t	ret;
			assert(!ep->link);
			if (0 && link_empty(&ep->s->outgoing.data)) {
				TASK_DELAY(0.1 * my_drand48());
			}
			/*      FWD_ITER(&ep->s->outgoing.data, msg_link,
              DBGOUT(FN; PTREXP(link_iter));
              );
      */
			if (link_empty(&ep->s->outgoing.data)) {
				TASK_CALL(flush_srv_buf(ep->s, &ret));
			}
			CHANNEL_GET(&ep->s->outgoing, &ep->link, msg_link);
			 {
				ssize_t	ret;
				/* DBGOUT(FN; PTREXP(stack); PTREXP(ep->link)); */
				MAY_DBG(FN; PTREXP(&ep->s->outgoing);
				    COPY_AND_FREE_GOUT(dbg_msg_link(ep->link));
				    );
				MAY_DBG(FN; STRLIT(" extracted ");
						COPY_AND_FREE_GOUT(dbg_linkage(&ep->link->l));
				    );

				/* If ep->link->p is 0, it is a protocol (re)negotiation request */
				if(ep->link->p){
					if(ep->s->con.x_proto != get_latest_common_proto()){ /* See if we need renegotiation */
						channel_put_front(&ep->s->outgoing, &ep->link->l); /* Push message back in queue, will be handled after negotiation */
						start_protocol_negotiation(&ep->s->outgoing);
					}else{
						ADD_X_EV(seconds(), ep->link->p->synode.group_id, ep->link->p->synode.msgno,
								 ep->link->p->synode.node, ep->link->p->to, ep->link->p->op,__FILE__, __LINE__, "sender_task sending");
						TASK_CALL(_send_msg(ep->s, ep->link->p, ep->link->to, &ret));
						if(ret < 0){
							goto next;
						}
						ADD_X_EV(seconds(), ep->link->p->synode.group_id, ep->link->p->synode.msgno,
								 ep->link->p->synode.node, ep->link->p->to, ep->link->p->op,__FILE__, __LINE__, "sender_task sent to");
					}
				} else {
					set_connected(&ep->s->con, CON_FD);
					/* Send protocol negotiation request */
					do{
						TASK_CALL(send_proto(&ep->s->con, my_xcom_version,  x_version_req, ep->tag, &ret));
						if(!is_connected(&ep->s->con)){
							goto next;
						}
						ep->tag = incr_tag(ep->tag);
					}while(ret < 0);
					G_DEBUG("sent negotiation request for protocol %d",my_xcom_version);

					/* Wait until negotiation done.
					   reply_handler_task will catch reply and change state */
					while(!proto_done(&ep->s->con)){
						TASK_DELAY(0.1);
						if(!is_connected(&ep->s->con)){
							goto next;
						}
					}
					G_DEBUG("will use protocol %d",ep->s->con.x_proto);
				}
			}
		next:
			msg_link_delete(&ep->link);
			/* TASK_YIELD; */
		}
	}
	FINALLY
	    empty_msg_channel(&ep->s->outgoing);
	ep->s->sender = NULL;
	srv_unref(ep->s);
	if (ep->link)
		msg_link_delete(&ep->link);
	TASK_END;
}

/* Fetch messages from queue and send to self.
   Having a separate mechanism for internal communication
   avoids SSL blocking when trying to connect to same thread. */
int	local_sender_task(task_arg arg)
{
	DECL_ENV
	    server * s;
	msg_link * link;
	END_ENV;

	TASK_BEGIN

	    ep->s = (server * )get_void_arg(arg);
	ep->link = NULL;
	srv_ref(ep->s);

	reset_srv_buf(&ep->s->out_buf);

	while (!xcom_shutdown) {

		assert(!ep->link);
		CHANNEL_GET(&ep->s->outgoing, &ep->link, msg_link);
		 {
			/* DBGOUT(FN; PTREXP(stack); PTREXP(ep->link)); */
			MAY_DBG(FN; PTREXP(&ep->s->outgoing);
					COPY_AND_FREE_GOUT(dbg_msg_link(ep->link));
			    );
			MAY_DBG(FN; STRLIT(" extracted ");
					COPY_AND_FREE_GOUT(dbg_linkage(&ep->link->l));
			    );
			assert(ep->link->p);
			dispatch_op(find_site_def(ep->link->p->synode), ep->link->p, NULL);
		}
		msg_link_delete(&ep->link);
	}
	FINALLY
	    empty_msg_channel(&ep->s->outgoing);
	ep->s->sender = NULL;
	srv_unref(ep->s);
	if (ep->link)
		msg_link_delete(&ep->link);
	TASK_END;
}


/* }}} */

static int	end_token(char *a)
{
	int	i = 0;
	while (a[i] != 0 && a[i] != ':') {
		i++;
	}
	return(i);
}


static char	*token_copy(char *a, int i)
{
	char	*ret;
	ret = calloc(1, (size_t)(i + 1));
	if(!ret)
		return ret;
	ret[i--] = 0;
	while (i >= 0) {
		ret[i] = a[i];
		i--;
	}
	return ret;
}


/* Get host name from host:port string */
static char	*get_name(char *a)
{
	int	i = end_token(a);
	return token_copy(a, i);
}

/* Get host name from host:port string */
char	*xcom_get_name(char *a)
{
	return get_name(a);
}


/* Get port from host:port string */
static int	get_port(char *a)
{
	int	i = end_token(a);
	if (a[i] != 0) {
		return atoi(a+i+1);
	} else {
		return 0;
	}
}


int	xcom_get_port(char *a)
{
	return get_port(a);
}


static server *find_server(server *table[], int n, char *name, int port)
{
	int	i;
	for (i = 0; i < n; i++) {
		server * s = table[i];
		if (s && strcmp(s->srv, name) == 0 && s->port == port) /* FIXME should use IP address */
			return s;
	}
	return 0;
}


void update_servers(site_def *s)
{
	u_int	n;

	if (s) {
		u_int i = 0;
		n = s->nodes.node_list_len;

		DBGOUT(FN; NDBG(get_maxnodes(s), u); NDBG(n, d); PTREXP(s));

		for (i = 0; i < n; i++) {
			char	*addr = s->nodes.node_list_val[i].address;
			char	*name = get_name(addr);
			int	port = get_port(addr);
			server * sp = find_server(all_servers, maxservers, name, port);

			if (sp) {
				DBGOUT(FN; STRLIT("re-using server "); NDBG(i, d); STREXP(name));
				free(name);
				s->servers[i] = sp;
			} else { /* No server? Create one */
				DBGOUT(FN; STRLIT("creating new server "); NDBG(i, d); STREXP(name));
				s->servers[i] = addsrv(name, port ? port : xcom_port);
			}
		}
		/* Zero the rest */
		for (i = n; i < NSERVERS; i++) {
			s->servers[i] = 0;
		}
	}
}


/* Remove tcp connections which seem to be idle */
int	tcp_reaper_task(task_arg arg MY_ATTRIBUTE((unused)))
{
	DECL_ENV
	    int	dummy;
	END_ENV;
	TASK_BEGIN
	    while (!xcom_shutdown) {
		int i;
		double	now = task_now();
		for (i = 0; i < maxservers; i++) {
			server * s = all_servers[i];
			if (s && s->con.fd != -1 && (s->active + 10.0) < now) {
				shutdown_connection(&s->con);
			}
		}
		TASK_DELAY(1.0);
	}
	FINALLY
	    TASK_END;
}


server *get_server(site_def const *s, node_no i)
{
	assert(s);
	return s->servers[i];
}

#define TERMINATE_CLIENT(ep) {						\
		if (ep->s->crash_on_error)				\
			abort();							\
		TERMINATE;								\
	}


/*
One-shot task to send a message to any xcom node via the client interface.
The sender need not be part of any group.
Any tcp connection may be used, as long as the message is a pax_msg
serialized with serialize_msg. Doing it this way is simply the most
convenient way of sending something to a specific address/port without blocking
the task system. Error handling is very rudimentary.
*/
/* Try to connect to another node */
static int	client_dial(char *srv, int port, connection_descriptor *con)
{
	DECL_ENV
	    int	dummy;
	END_ENV;

	TASK_BEGIN
	    DBGOUT(FN; STRLIT(" dial "); NPUT(get_nodeno(get_site_def()), u); STRLIT(srv); NDBG(port, d));
	TASK_CALL(connect_tcp(srv, port, &con->fd));
	/* DBGOUT(FN; NDBG(con->fd,d);); */
	if (con->fd < 0) {
		DBGOUT(FN; STRLIT("could not dial "); STRLIT(srv); NDBG(port, d); );
	} else {
		if (NAGLE == 0) {
			set_nodelay(con->fd);
		}

		unblock_fd(con->fd);
#ifdef XCOM_HAVE_OPENSSL
		if (xcom_use_ssl()) {
			result ret = {0,0};
			SSL_CONNECT((*con), srv);
		}
#endif
		DBGOUT(FN; STRLIT("connected to "); STRLIT(srv); NDBG(con->fd, d); NDBG(port, d));
		set_connected(con, CON_FD);
	}
	FINALLY
	    TASK_END;
}


int	client_task(task_arg arg)
{
	DECL_ENV
	    envelope * s;
	u_int	buflen;
	char	*buf;
	connection_descriptor c_descriptor;
	xcom_proto x_proto;
	x_msg_type x_type;
	unsigned int	tag;
	END_ENV;

	TASK_BEGIN

	    ep->s = (envelope * )get_void_arg(arg);
	ep->c_descriptor.fd = -1;
#ifdef XCOM_HAVE_OPENSSL
	ep->c_descriptor.ssl_fd = 0;
#endif
	ep->buf = 0;
	ep->x_proto = my_xcom_version;

	/* Loop until connected */
	while (!is_connected(&ep->c_descriptor)) {
		TASK_CALL(client_dial(ep->s->srv, ep->s->port, &ep->c_descriptor));
		if (ep->c_descriptor.fd < 0) {
			TASK_DELAY(1.000);
		}
	}


#ifdef XCOM_HAVE_OPENSSL
	if (xcom_use_ssl()) {
		result ret = {
			0, 0		};
		SSL_CONNECT(ep->c_descriptor, ep->s->srv);
	}
#endif
	 {
		ssize_t	sent;
		ssize_t	n;
		/* Send protocol negotiation request */
		DBGOUT(FN);
		TASK_CALL(send_proto(&ep->c_descriptor, my_xcom_version,  x_version_req, TAG_START, &sent));
		if (sent < 0) {
			TERMINATE_CLIENT(ep);
		}

		DBGOUT(FN);
		/* Wait for answer and read protocol version */
		TASK_CALL(recv_proto(&ep->c_descriptor, &ep->x_proto, &ep->x_type, &ep->tag, &n));
		if (n < 0) {
			TERMINATE_CLIENT(ep);
		}

		DBGOUT(FN);
		if (ep->tag == TAG_START && ep->x_type == x_version_reply) {
			DBGOUT(STRLIT("client task will use protcol version ");
			    NDBG(ep->x_proto, u); STRLIT(xcom_proto_to_str(ep->x_proto)));
			if (ep->x_proto == x_unknown_proto) {
				TERMINATE_CLIENT(ep);
			}

			DBGOUT(FN);
			ep->c_descriptor.x_proto = ep->x_proto;
			/* Send message */
			serialize_msg(ep->s->p, ep->c_descriptor.x_proto, &ep->buflen, &ep->buf);
			if (ep->buflen) {
				DBGOUT(FN);
				TASK_CALL(task_write(&ep->c_descriptor, ep->buf, ep->buflen, &sent));
				if (ep->buflen != sent) {
					DBGOUT(FN; STRLIT("write failed "); STRLIT(ep->s->srv); NDBG(ep->s->port, d);
					    NDBG(ep->buflen, d); NDBG(sent, d));
					TERMINATE_CLIENT(ep);
				}
			}
		} else {
			DBGOUT(FN);
			TERMINATE_CLIENT(ep);
		}
	}

	FINALLY
	    shutdown_connection(&ep->c_descriptor);
	X_FREE(ep->buf);
	free(ep->s->srv);
	XCOM_XDR_FREE(xdr_pax_msg, ep->s->p);
	free(ep->s);
	TASK_END;
}

#ifdef XCOM_HAVE_OPENSSL
void ssl_free_con(connection_descriptor *con)
{
	SSL_free(con->ssl_fd);
	con->ssl_fd= NULL;
}

void ssl_shutdown_con(connection_descriptor *con)
{
	if(con->fd >= 0 && con->ssl_fd != NULL) {
		SSL_shutdown(con->ssl_fd);
		ssl_free_con(con);
	}
}
#endif

void close_connection(connection_descriptor *con)
{
	shut_close_socket(&con->fd);
	con->fd = -1;
	set_connected(con, CON_NULL);
}

void shutdown_connection(connection_descriptor *con)
{
	/* printstack(1); */
#ifdef XCOM_HAVE_OPENSSL
	ssl_shutdown_con(con);
#endif
	close_connection(con);
}

void reset_connection(connection_descriptor *con)
{
	con->fd = -1;
#ifdef XCOM_HAVE_OPENSSL
	con->ssl_fd = 0;
#endif
	set_connected(con, CON_NULL);
}

/* The protocol version used by the group as a whole is the minimum of the
 maximum protocol versions in the config. */
xcom_proto common_xcom_version(site_def const *site)
{
	u_int i;
	xcom_proto min_proto = my_xcom_version;
	for(i = 0; i < site->nodes.node_list_len; i++){
		min_proto = MIN(min_proto, site->nodes.node_list_val[i].proto.max_proto);
	}
	return min_proto;
}

static xcom_proto latest_common_proto = MY_XCOM_PROTO;

xcom_proto set_latest_common_proto(xcom_proto x_proto)
{
	return latest_common_proto = x_proto;
}

xcom_proto get_latest_common_proto()
{
	return latest_common_proto;
}

/* See which protocol we can use.
   Needs to be redefined as the protocol changes */
xcom_proto negotiate_protocol(xcom_proto proto_vers)
{
	/* Ensure that protocol will not be greater than
	my_xcom_version */
	if(proto_vers < my_min_xcom_version){
		return x_unknown_proto;
	}else if(proto_vers > my_xcom_version){
		return my_xcom_version;
	}else{
		return proto_vers;
	}
}

/*
   Encode and decode node_address with protocol version 0.
   This version is frozen forever, so having a handcrafted (in reality mostly copied)
   xdr function here is OK.
*/
bool_t xdr_node_address_with_1_0 (XDR *xdrs, node_address *objp)
{
	if (!xdr_string (xdrs, &objp->address, ~0))
		return FALSE;
	if (!xdr_blob (xdrs, &objp->uuid))
		return FALSE;
	if (xdrs->x_op == XDR_DECODE) {
		objp->proto.min_proto = x_1_0; /* A node which speaks protocol version 0 only supports version 0 */
		objp->proto.max_proto = x_1_0;
	}
	return TRUE;
}

/* Encode and decode a node_list while respecting protocol version */
bool_t xdr_node_list_1_1(XDR *xdrs, node_list_1_1 *objp)
{
	xcom_proto vx = *((xcom_proto*)xdrs->x_public);
	/* Select protocol encode/decode based on the x_public field of the xdr struct */
	switch(vx){
	case x_1_0:
		return xdr_array (xdrs, (char **)&objp->node_list_val, (u_int *) &objp->node_list_len, NSERVERS,
		sizeof (node_address), (xdrproc_t) xdr_node_address_with_1_0);
	case x_1_1:
		return xdr_array (xdrs, (char **)&objp->node_list_val, (u_int *) &objp->node_list_len, NSERVERS,
		sizeof (node_address), (xdrproc_t) xdr_node_address);
	default:
		return FALSE;
	}
}

/* Encode and decode a application data with added check that there is enough data when decoding */
bool_t xdr_checked_data(XDR *xdrs, checked_data *objp)
{
	/* Sanity check. x_handy is number of remaining bytes */
	if(xdrs->x_op == XDR_DECODE && (objp->data_len + 4 )> xdrs->x_handy)
		return FALSE;
	return xdr_bytes(xdrs, (char **)&objp->data_val, (u_int *) &objp->data_len, 0xffffffff);
}
