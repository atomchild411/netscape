/* -*- Mode: C; tab-width: 4 -*-
 *   mkh2.c --- HTTP/2 for netlib, from nghttp2, behind HTTP/1.1 sockets.
 *
 * See mkh2.h.  A session holds the TLS socket and nghttp2's state; it is
 * driven ("pumped") whenever one of its streams is read, written or polled:
 * everything the socket has is read and handed to nghttp2, which files it
 * under its streams, and everything nghttp2 has to send is written.
 *
 * Flow control is by what the HTTP code reads: a stream's window opens
 * again only as its data is taken, so a stream nobody reads holds at most
 * one window (H2_STREAM_WINDOW) here.
 *
 * NS_H2_TRACE=/file logs sessions, streams and frames.
 */

#include "mkutils.h"
#include "mkh2.h"
#include "mktls.h"
#include "mkselect.h"

#include "prerror.h"
#include "prmem.h"
#include "plstr.h"
#include "prprf.h"
#include "prinrval.h"
#include "private/pprio.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <nghttp2/nghttp2.h>

#define H2_STREAM_WINDOW	(256 * 1024)
#define H2_SESSION_WINDOW	(4 * 1024 * 1024)
#define H2_MAX_SESSIONS		16
/* how long requests wait for a connection still being made */
#define H2_PENDING_SECONDS	20
#define H2_KNOWN_H1			64

typedef struct NetH2Session NetH2Session;
typedef struct NetH2Stream NetH2Stream;

struct NetH2Session {
	NetH2Session *next;
	char *host;
	PRFileDesc *sock;			/* the TLS socket, once adopted */
	PRInt32 osfd;				/* its descriptor (streams' duplicates) */
	nghttp2_session *ng;		/* NULL while pending */
	PRFileDesc *connecting;		/* pending: the connection being made
								 * (only compared, never used) */
	PRIntervalTime opened;
	NetH2Stream *streams;		/* the streams with a descriptor */
	XP_Bool dead;				/* the connection is gone */
	XP_Bool going_away;			/* GOAWAY: no new streams */
	XP_Bool write_blocked;		/* the socket took less than offered */
	PRIntervalTime idle_since;
};

struct NetH2Stream {
	NetH2Stream *next;			/* in the session's list */
	NetH2Session *session;
	PRFileDesc *fd;				/* the stream's descriptor (top layer) */
	int32 id;					/* 0 until the request goes out */

	/* the request, as the HTTP code writes it */
	char *req;
	int32 req_len, req_size;
	int32 head_len;				/* the request's head, once complete */
	int32 body_len;				/* Content-Length */
	int32 body_sent;
	XP_Bool complete;			/* all of the request is here */
	XP_Bool submitted;

	/* the response, as the HTTP code reads it */
	char *headers;				/* collected for the head */
	int32 headers_len, headers_size;
	int status;
	XP_Bool head_done;			/* the HTTP/1.1 head is in buf */
	int32 head_unread;			/* of it, not read yet (not DATA) */
	char *buf;
	int32 buf_start, buf_len, buf_size;
	XP_Bool closed;				/* the stream has ended */
	XP_Bool failed;				/* ...without all of its response */
};

static PRDescIdentity net_h2_identity = PR_INVALID_IO_LAYER;
static PRIOMethods net_h2_methods;
static NetH2Session *net_h2_sessions = NULL;
static nghttp2_session_callbacks *net_h2_callbacks = NULL;
static XP_Bool net_h2_kicking = FALSE;
static FILE *net_h2_logf = NULL;
static int net_h2_debug = -1;
/* hosts that answered a TLS connection with HTTP/1.1: no waiting for them */
static char *net_h2_known_h1[H2_KNOWN_H1];
static int net_h2_n_known_h1 = 0;

static void
net_h2_log(const char *fmt, ...)
{
	va_list ap;

	if (net_h2_debug < 0) {
		const char *e = getenv("NS_H2_TRACE");
		net_h2_debug = e && *e == '/';
		if (net_h2_debug && !(net_h2_logf = fopen(e, "a")))
			net_h2_debug = 0;
	}
	if (!net_h2_debug)
		return;
	va_start(ap, fmt);
	fprintf(net_h2_logf, "h2 %lu: ",
			(unsigned long) PR_IntervalToMilliseconds(PR_IntervalNow()));
	vfprintf(net_h2_logf, fmt, ap);
	fputc('\n', net_h2_logf);
	fflush(net_h2_logf);
	va_end(ap);
}

MODULE_PRIVATE XP_Bool
NET_H2_Enabled(void)
{
	const char *e = getenv("NS_HTTP2");

	return !(e && *e == '0');
}

/* Grow *BUF (of *SIZE) to hold NEED bytes. */
static XP_Bool
net_h2_reserve(char **buf, int32 *size, int32 need)
{
	char *b;
	int32 n;

	if (need <= *size)
		return TRUE;
	n = need * 2 + 1024;
	if (!(b = (char *) PR_Realloc(*buf, n)))
		return FALSE;
	*buf = b;
	*size = n;
	return TRUE;
}

static XP_Bool
net_h2_append(char **buf, int32 *len, int32 *size, const char *s, int32 n)
{
	if (!net_h2_reserve(buf, size, *len + n + 1))
		return FALSE;
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
	return TRUE;
}

/* The response bytes the HTTP code can read now. */
static void
net_h2_put(NetH2Stream *st, const char *s, int32 n)
{
	if (st->buf_start > 0 && st->buf_start == st->buf_len)
		st->buf_start = st->buf_len = 0;
	if (st->buf_start > 65536 && st->buf_start * 2 > st->buf_len) {
		memmove(st->buf, st->buf + st->buf_start, st->buf_len - st->buf_start);
		st->buf_len -= st->buf_start;
		st->buf_start = 0;
	}
	if (!net_h2_append(&st->buf, &st->buf_len, &st->buf_size, s, n))
		st->failed = st->closed = TRUE;
}

/* Is there something for the stream's reader: data, or its end? */
static XP_Bool
net_h2_ready(NetH2Stream *st)
{
	return st->buf_len > st->buf_start || st->closed;
}

/* While a stream holds what its reader has not taken, netlib has to come
 * back without waiting for the socket, which has nothing more to say. */
static void
net_h2_kick(void)
{
	NetH2Session *s;
	NetH2Stream *st;
	XP_Bool want = FALSE;

	for (s = net_h2_sessions; s && !want; s = s->next) {
		if (s->write_blocked && !s->dead && s->ng)
			want = TRUE;
		for (st = s->streams; st && !want; st = st->next)
			if (net_h2_ready(st))
				want = TRUE;
	}
	if (want && !net_h2_kicking)
		NET_SetCallNetlibAllTheTime(NULL, "mkh2");
	else if (!want && net_h2_kicking)
		NET_ClearCallNetlibAllTheTime(NULL, "mkh2");
	net_h2_kicking = want;
}

/* --- the session ------------------------------------------------------- */

static void
net_h2_session_free(NetH2Session *s)
{
	NetH2Session **p;

	for (p = &net_h2_sessions; *p; p = &(*p)->next)
		if (*p == s) {
			*p = s->next;
			break;
		}
	net_h2_log("%s: session closed", s->host);
	if (s->ng)
		nghttp2_session_del(s->ng);
	if (s->sock)
		PR_Close(s->sock);
	PR_FREEIF(s->host);
	PR_Free(s);
}

/* The connection is gone: streams still waiting end in failure. */
static void
net_h2_session_dead(NetH2Session *s, const char *why)
{
	NetH2Stream *st;

	if (s->dead)
		return;
	net_h2_log("%s: session dead (%s)", s->host, why);
	s->dead = TRUE;
	for (st = s->streams; st; st = st->next)
		if (!st->closed)
			st->failed = st->closed = TRUE;
}

/* Free the dead sessions no stream uses: only here, where no caller still
 * holds one. */
static void
net_h2_reap(void)
{
	NetH2Session *s, *next;

	for (s = net_h2_sessions; s; s = next) {
		next = s->next;
		if (s->dead && !s->streams)
			net_h2_session_free(s);
	}
}

static nghttp2_ssize
net_h2_send_cb(nghttp2_session *ng, const uint8_t *data, size_t length,
			   int flags, void *user_data)
{
	NetH2Session *s = (NetH2Session *) user_data;
	PRInt32 n = PR_Write(s->sock, data, (PRInt32) length);

	if (n < 0) {
		if (PR_GetError() == PR_WOULD_BLOCK_ERROR) {
			s->write_blocked = TRUE;
			return NGHTTP2_ERR_WOULDBLOCK;
		}
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}
	if ((size_t) n < length)
		s->write_blocked = TRUE;
	return n;
}

/* Read what the socket has, and write what nghttp2 has. */
static void
net_h2_pump(NetH2Session *s)
{
	uint8_t buf[16384];
	PRInt32 n;
	nghttp2_ssize rv;

	if (s->dead)
		return;
	if (!s->ng) {
		/* pending: the connection is still being made */
		if (PR_IntervalToSeconds(PR_IntervalNow() - s->opened)
			> H2_PENDING_SECONDS)
			net_h2_session_dead(s, "connection took too long");
		return;
	}
	for (;;) {
		n = PR_Read(s->sock, buf, sizeof buf);
		if (n > 0) {
			rv = nghttp2_session_mem_recv2(s->ng, buf, n);
			if (rv < 0) {
				net_h2_session_dead(s, nghttp2_strerror((int) rv));
				return;
			}
			continue;
		}
		if (n == 0) {
			net_h2_session_dead(s, "closed by the server");
			return;
		}
		if (PR_GetError() != PR_WOULD_BLOCK_ERROR) {
			net_h2_session_dead(s, "read error");
			return;
		}
		break;
	}
	s->write_blocked = FALSE;
	if (nghttp2_session_send(s->ng) < 0) {
		net_h2_session_dead(s, "send failed");
		return;
	}
	if (!nghttp2_session_want_read(s->ng) && !nghttp2_session_want_write(s->ng))
		net_h2_session_dead(s, "finished");
}

static NetH2Stream *
net_h2_stream_of(NetH2Session *s, int32_t id)
{
	return (NetH2Stream *) nghttp2_session_get_stream_user_data(s->ng, id);
}

/* Header names as HTTP/1.1 writes them: "Content-Type". */
static void
net_h2_add_header(NetH2Stream *st, const uint8_t *name, size_t namelen,
				  const uint8_t *value, size_t valuelen)
{
	int32 at = st->headers_len;
	size_t i;
	XP_Bool up = TRUE;

	if (!net_h2_append(&st->headers, &st->headers_len, &st->headers_size,
					   (const char *) name, (int32) namelen))
		return;
	for (i = 0; i < namelen; i++) {
		char *c = st->headers + at + i;
		if (up && *c >= 'a' && *c <= 'z')
			*c -= 'a' - 'A';
		up = *c == '-';
	}
	net_h2_append(&st->headers, &st->headers_len, &st->headers_size, ": ", 2);
	net_h2_append(&st->headers, &st->headers_len, &st->headers_size,
				  (const char *) value, (int32) valuelen);
	net_h2_append(&st->headers, &st->headers_len, &st->headers_size, "\r\n", 2);
}

static const char *
net_h2_reason(int status)
{
	switch (status) {
	case 200: return "OK";
	case 201: return "Created";
	case 204: return "No Content";
	case 206: return "Partial Content";
	case 301: return "Moved Permanently";
	case 302: return "Found";
	case 303: return "See Other";
	case 304: return "Not Modified";
	case 307: return "Temporary Redirect";
	case 308: return "Permanent Redirect";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 407: return "Proxy Authentication Required";
	case 429: return "Too Many Requests";
	case 500: return "Internal Server Error";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	default:  return "Status";
	}
}

static int
net_h2_header_cb(nghttp2_session *ng, const nghttp2_frame *frame,
				 const uint8_t *name, size_t namelen,
				 const uint8_t *value, size_t valuelen,
				 uint8_t flags, void *user_data)
{
	NetH2Session *s = (NetH2Session *) user_data;
	NetH2Stream *st;

	if (frame->hd.type != NGHTTP2_HEADERS
		|| !(st = net_h2_stream_of(s, frame->hd.stream_id))
		|| st->head_done)		/* trailers */
		return 0;
	if (namelen == 7 && !memcmp(name, ":status", 7)) {
		st->status = atoi((const char *) value);
		return 0;
	}
	if (namelen > 0 && name[0] == ':')
		return 0;
	net_h2_add_header(st, name, namelen, value, valuelen);
	return 0;
}

static int
net_h2_frame_cb(nghttp2_session *ng, const nghttp2_frame *frame,
				void *user_data)
{
	NetH2Session *s = (NetH2Session *) user_data;
	NetH2Stream *st;
	char line[64];

	if (frame->hd.type == NGHTTP2_GOAWAY) {
		net_h2_log("%s: GOAWAY last %d error %u", s->host,
				   frame->goaway.last_stream_id, frame->goaway.error_code);
		s->going_away = TRUE;
		return 0;
	}
	if (frame->hd.type != NGHTTP2_HEADERS
		|| !(st = net_h2_stream_of(s, frame->hd.stream_id)) || st->head_done)
		return 0;
	if (st->status >= 100 && st->status < 200) {
		/* an interim response (103 Early Hints): the real one follows */
		st->headers_len = 0;
		st->status = 0;
		return 0;
	}
	net_h2_log("%s: stream %d status %d", s->host, st->id, st->status);
	PR_snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", st->status,
				net_h2_reason(st->status));
	net_h2_put(st, line, (int32) strlen(line));
	if (st->headers_len)
		net_h2_put(st, st->headers, st->headers_len);
	net_h2_put(st, "Connection: close\r\n\r\n", 21);
	st->head_unread = (int32) strlen(line) + st->headers_len + 21;
	PR_FREEIF(st->headers);
	st->headers = NULL;
	st->headers_len = st->headers_size = 0;
	st->head_done = TRUE;
	return 0;
}

static int
net_h2_data_cb(nghttp2_session *ng, uint8_t flags, int32_t stream_id,
			   const uint8_t *data, size_t len, void *user_data)
{
	NetH2Session *s = (NetH2Session *) user_data;
	NetH2Stream *st = net_h2_stream_of(s, stream_id);

	if (!st || !st->head_done) {
		/* nobody reads it: give the window back now */
		nghttp2_session_consume(ng, stream_id, len);
		return 0;
	}
	net_h2_put(st, (const char *) data, (int32) len);
	return 0;
}

static int
net_h2_close_cb(nghttp2_session *ng, int32_t stream_id, uint32_t error_code,
				void *user_data)
{
	NetH2Session *s = (NetH2Session *) user_data;
	NetH2Stream *st = net_h2_stream_of(s, stream_id);

	if (!st)
		return 0;
	net_h2_log("%s: stream %d closed, error %u", s->host, stream_id,
			   error_code);
	st->closed = TRUE;
	if (error_code != NGHTTP2_NO_ERROR || !st->head_done)
		st->failed = TRUE;
	nghttp2_session_set_stream_user_data(ng, stream_id, NULL);
	return 0;
}

static XP_Bool
net_h2_init(void)
{
	if (net_h2_callbacks)
		return TRUE;
	if (nghttp2_session_callbacks_new(&net_h2_callbacks) != 0)
		return FALSE;
	nghttp2_session_callbacks_set_send_callback2(net_h2_callbacks,
												 net_h2_send_cb);
	nghttp2_session_callbacks_set_on_header_callback(net_h2_callbacks,
													 net_h2_header_cb);
	nghttp2_session_callbacks_set_on_frame_recv_callback(net_h2_callbacks,
														 net_h2_frame_cb);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
		net_h2_callbacks, net_h2_data_cb);
	nghttp2_session_callbacks_set_on_stream_close_callback(net_h2_callbacks,
														   net_h2_close_cb);
	return TRUE;
}

/* --- the request -------------------------------------------------------- */

static nghttp2_ssize
net_h2_body_cb(nghttp2_session *ng, int32_t stream_id, uint8_t *buf,
			   size_t length, uint32_t *data_flags,
			   nghttp2_data_source *source, void *user_data)
{
	NetH2Stream *st = (NetH2Stream *) source->ptr;
	int32 left = st->body_len - st->body_sent;

	if ((size_t) left > length)
		left = (int32) length;
	memcpy(buf, st->req + st->head_len + st->body_sent, left);
	st->body_sent += left;
	if (st->body_sent >= st->body_len)
		*data_flags |= NGHTTP2_DATA_FLAG_EOF;
	return left;
}

/* A header that is about the HTTP/1.1 connection, not the request. */
static XP_Bool
net_h2_hop_header(const char *name, int32 len)
{
	static const char *const hop[] = {
		"connection", "keep-alive", "proxy-connection", "transfer-encoding",
		"upgrade", "host", "te", NULL
	};
	int i;

	for (i = 0; hop[i]; i++)
		if ((int32) strlen(hop[i]) == len && !PL_strncasecmp(name, hop[i], len))
			return TRUE;
	return FALSE;
}

/* The request's head is complete: send it as HEADERS (and its body, once
 * all of it is here). */
static XP_Bool
net_h2_submit(NetH2Stream *st)
{
	NetH2Session *s = st->session;
	nghttp2_nv nv[64];
	char *names[64];
	int n = 0, i;
	char *line, *end, *sp, *method, *path, *authority = NULL;
	nghttp2_data_provider2 body;
	int32_t id;

	/* "GET /path HTTP/1.1" */
	line = st->req;
	end = strstr(line, "\r\n");
	*end = '\0';
	method = line;
	if (!(sp = strchr(method, ' ')))
		return FALSE;
	*sp = '\0';
	path = sp + 1;
	if ((sp = strchr(path, ' ')))
		*sp = '\0';
	if (!PL_strncasecmp(path, "http", 4) && (sp = strstr(path, "://"))
		&& (sp = strchr(sp + 3, '/')))
		path = sp;			/* an absolute URL: its path */

#define NV(nm, vl) do { \
		nv[n].name = (uint8_t *) (nm); nv[n].namelen = strlen(nm); \
		nv[n].value = (uint8_t *) (vl); nv[n].valuelen = strlen(vl); \
		nv[n].flags = NGHTTP2_NV_FLAG_NONE; names[n] = NULL; n++; } while (0)
	NV(":method", method);
	NV(":scheme", "https");
	NV(":path", path);
	n++;					/* :authority, below */

	for (line = end + 2; line < st->req + st->head_len - 2 && n < 63;
		 line = end + 2) {
		char *colon, *value, *lower;
		int32 namelen;

		end = strstr(line, "\r\n");
		*end = '\0';
		if (!(colon = strchr(line, ':')))
			continue;
		namelen = colon - line;
		for (value = colon + 1; *value == ' ' || *value == '\t'; value++)
			;
		if (namelen == 4 && !PL_strncasecmp(line, "host", 4))
			authority = value;
		if (net_h2_hop_header(line, namelen))
			continue;
		if (!(lower = PL_strndup(line, namelen)))
			break;
		for (i = 0; i < namelen; i++)
			lower[i] = tolower((unsigned char) lower[i]);
		nv[n].name = (uint8_t *) lower;
		nv[n].namelen = namelen;
		nv[n].value = (uint8_t *) value;
		nv[n].valuelen = strlen(value);
		nv[n].flags = NGHTTP2_NV_FLAG_NONE;
		names[n] = lower;
		n++;
	}
	if (!authority)
		authority = s->host;
	nv[3].name = (uint8_t *) ":authority";
	nv[3].namelen = 10;
	nv[3].value = (uint8_t *) authority;
	nv[3].valuelen = strlen(authority);
	nv[3].flags = NGHTTP2_NV_FLAG_NONE;
	names[3] = NULL;
#undef NV

	body.source.ptr = st;
	body.read_callback = net_h2_body_cb;
	id = nghttp2_submit_request2(s->ng, NULL, nv, n,
								 st->body_len > 0 ? &body : NULL, st);
	net_h2_log("%s: stream %d %s %s", s->host, id, method, path);
	for (i = 0; i < n; i++)
		PR_FREEIF(names[i]);
	if (id < 0)
		return FALSE;
	st->id = id;
	st->submitted = TRUE;
	return TRUE;
}

/* --- the stream's layer -------------------------------------------------- */

static NetH2Stream *
net_h2_stream(PRFileDesc *fd)
{
	PRFileDesc *layer;

	if (!fd || net_h2_identity == PR_INVALID_IO_LAYER)
		return NULL;
	layer = PR_GetIdentitiesLayer(fd, net_h2_identity);
	return layer ? (NetH2Stream *) layer->secret : NULL;
}

static PRInt32 PR_CALLBACK
net_h2_read(PRFileDesc *fd, void *buf, PRInt32 amount)
{
	NetH2Stream *st = (NetH2Stream *) fd->secret;
	PRInt32 n, body;

	if (st->session && net_h2_ready(st) == FALSE)
		net_h2_pump(st->session);
	n = st->buf_len - st->buf_start;
	if (n > 0) {
		if (n > amount)
			n = amount;
		memcpy(buf, st->buf + st->buf_start, n);
		st->buf_start += n;
		/* the body's bytes the server may send again (the head we made
		 * is not DATA; once the stream has closed, this still opens the
		 * connection's window) */
		body = n;
		if (st->head_unread > 0) {
			PRInt32 h = body < st->head_unread ? body : st->head_unread;
			st->head_unread -= h;
			body -= h;
		}
		if (body > 0 && st->session && !st->session->dead && st->id > 0) {
			nghttp2_session_consume(st->session->ng, st->id, body);
			net_h2_pump(st->session);
		}
		net_h2_kick();
		return n;
	}
	net_h2_kick();
	if (st->failed) {
		PR_SetError(PR_CONNECT_RESET_ERROR, ECONNRESET);
		return -1;
	}
	if (st->closed)
		return 0;
	PR_SetError(PR_WOULD_BLOCK_ERROR, EWOULDBLOCK);
	return -1;
}

static PRInt32 PR_CALLBACK
net_h2_write(PRFileDesc *fd, const void *buf, PRInt32 amount)
{
	NetH2Stream *st = (NetH2Stream *) fd->secret;
	char *head_end, *cl;

	if (!st->session || st->session->dead || st->closed) {
		PR_SetError(PR_CONNECT_RESET_ERROR, ECONNRESET);
		return -1;
	}
	if (st->submitted)
		return amount;		/* nothing more is sent on a stream */
	if (!net_h2_append(&st->req, &st->req_len, &st->req_size,
					   (const char *) buf, amount)) {
		PR_SetError(PR_OUT_OF_MEMORY_ERROR, 0);
		return -1;
	}
	if (!st->head_len) {
		if (!(head_end = strstr(st->req, "\r\n\r\n")))
			return amount;
		st->head_len = head_end + 4 - st->req;
		/* Content-Length: what of the request is still to come */
		for (cl = st->req; (cl = strstr(cl, "\r\n")) && cl < head_end; cl += 2)
			if (!PL_strncasecmp(cl + 2, "content-length:", 15))
				st->body_len = atol(cl + 17);
	}
	if (st->req_len - st->head_len < st->body_len)
		return amount;
	st->complete = TRUE;
	if (!st->session->ng)
		return amount;		/* sent once the connection is up */
	if (!net_h2_submit(st)) {
		st->failed = st->closed = TRUE;
		PR_SetError(PR_CONNECT_RESET_ERROR, ECONNRESET);
		return -1;
	}
	net_h2_pump(st->session);
	net_h2_kick();
	return amount;
}

static PRInt32 PR_CALLBACK
net_h2_recv(PRFileDesc *fd, void *buf, PRInt32 amount, PRIntn flags,
			PRIntervalTime timeout)
{
	return net_h2_read(fd, buf, amount);
}

static PRInt32 PR_CALLBACK
net_h2_send(PRFileDesc *fd, const void *buf, PRInt32 amount, PRIntn flags,
			PRIntervalTime timeout)
{
	return net_h2_write(fd, buf, amount);
}

static PRInt32 PR_CALLBACK
net_h2_available(PRFileDesc *fd)
{
	NetH2Stream *st = (NetH2Stream *) fd->secret;

	return st->buf_len - st->buf_start;
}

static PRInt16 PR_CALLBACK
net_h2_poll(PRFileDesc *fd, PRInt16 in_flags, PRInt16 *out_flags)
{
	NetH2Stream *st = (NetH2Stream *) fd->secret;

	*out_flags = 0;
	if ((in_flags & PR_POLL_READ) && net_h2_ready(st)) {
		*out_flags = PR_POLL_READ;
		return in_flags;
	}
	/* writes are taken whole: the stream is always writable */
	if (in_flags & PR_POLL_WRITE) {
		*out_flags = PR_POLL_WRITE;
		return in_flags;
	}
	return (fd->lower->methods->poll)(fd->lower, in_flags, out_flags);
}

static PRStatus PR_CALLBACK
net_h2_close(PRFileDesc *fd)
{
	NetH2Stream *st = (NetH2Stream *) fd->secret;
	PRStatus status;

	if (st) {
		NetH2Session *s = st->session;

		if (s) {
			NetH2Stream **p;

			for (p = &s->streams; *p; p = &(*p)->next)
				if (*p == st) {
					*p = st->next;
					break;
				}
			net_h2_log("%s: stream %d done", s->host, st->id);
			s->idle_since = PR_IntervalNow();
			if (!s->dead && st->id > 0) {
				/* what was received but not read, then the stream */
				int32 unread = st->buf_len - st->buf_start - st->head_unread;

				if (unread > 0)
					nghttp2_session_consume(s->ng, st->id, unread);
				if (!st->closed) {
					nghttp2_session_set_stream_user_data(s->ng, st->id, NULL);
					nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE,
											  st->id, NGHTTP2_CANCEL);
				}
				net_h2_pump(s);
			}
		}
		PR_FREEIF(st->req);
		PR_FREEIF(st->headers);
		PR_FREEIF(st->buf);
		PR_Free(st);
		fd->secret = NULL;
	}
	net_h2_reap();
	net_h2_kick();
	status = (fd->lower->methods->close)(fd->lower);
	fd->dtor(fd);
	return status;
}

/* A new stream on S: a duplicate of the session's socket, under our layer. */
static PRFileDesc *
net_h2_new_stream(NetH2Session *s)
{
	NetH2Stream *st;
	PRFileDesc *fd, *layer;
	int osfd = s->osfd, dupfd;

	if (osfd < 0 || (dupfd = dup(osfd)) < 0)
		return NULL;
	if (!(fd = PR_ImportTCPSocket(dupfd))) {
		close(dupfd);
		return NULL;
	}
	st = PR_NEWZAP(NetH2Stream);
	layer = PR_CreateIOLayerStub(net_h2_identity, &net_h2_methods);
	if (!st || !layer) {
		PR_FREEIF(st);
		if (layer)
			layer->dtor(layer);
		PR_Close(fd);
		return NULL;
	}
	layer->secret = (PRFilePrivate *) st;
	if (PR_PushIOLayer(fd, PR_TOP_IO_LAYER, layer) != PR_SUCCESS) {
		layer->secret = NULL;
		layer->dtor(layer);
		PR_Free(st);
		PR_Close(fd);
		return NULL;
	}
	st->session = s;
	st->fd = fd;
	st->next = s->streams;
	s->streams = st;
	return fd;
}

static int
net_h2_active(NetH2Session *s)
{
	NetH2Stream *st;
	int n = 0;

	for (st = s->streams; st; st = st->next)
		n++;
	return n;
}

MODULE_PRIVATE PRFileDesc *
NET_H2_StreamFor(const char *host)
{
	NetH2Session *s, *next;

	if (!host || !net_h2_callbacks)
		return NULL;
	net_h2_reap();
	for (s = net_h2_sessions; s; s = next) {
		next = s->next;
		if (PL_strcasecmp(s->host, host) || s->dead || s->going_away)
			continue;
		/* has the server closed it while it was idle? */
		net_h2_pump(s);
		if (s->dead || s->going_away)
			continue;
		if (s->ng && (uint32) net_h2_active(s)
			>= nghttp2_session_get_remote_settings(
				s->ng, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS))
			continue;
		net_h2_log("%s: new stream (%d active)", s->host, net_h2_active(s));
		return net_h2_new_stream(s);
	}
	return NULL;
}

/* Too many sessions: close the ones idle longest. */
static void
net_h2_trim(void)
{
	NetH2Session *s, *oldest;
	int count;

	for (;;) {
		count = 0;
		oldest = NULL;
		for (s = net_h2_sessions; s; s = s->next) {
			count++;
			if (s->ng && !s->streams && (!oldest || (PRInt32) (s->idle_since
												- oldest->idle_since) < 0))
				oldest = s;
		}
		if (count <= H2_MAX_SESSIONS || !oldest)
			return;
		nghttp2_session_terminate_session(oldest->ng, NGHTTP2_NO_ERROR);
		nghttp2_session_send(oldest->ng);
		net_h2_session_free(oldest);
	}
}

static XP_Bool
net_h2_layer_init(void)
{
	if (net_h2_identity == PR_INVALID_IO_LAYER) {
		net_h2_identity = PR_GetUniqueIdentity("netlib HTTP/2");
		net_h2_methods = *PR_GetDefaultIOMethods();
		net_h2_methods.file_type = PR_DESC_LAYERED;
		net_h2_methods.close = net_h2_close;
		net_h2_methods.read = net_h2_read;
		net_h2_methods.write = net_h2_write;
		net_h2_methods.recv = net_h2_recv;
		net_h2_methods.send = net_h2_send;
		net_h2_methods.available = net_h2_available;
		net_h2_methods.poll = net_h2_poll;
	}
	return net_h2_init();
}

static NetH2Session *
net_h2_pending_of(PRFileDesc *sock)
{
	NetH2Session *s;

	for (s = net_h2_sessions; s; s = s->next)
		if (!s->ng && !s->dead && s->connecting == sock)
			return s;
	return NULL;
}

static XP_Bool
net_h2_is_known_h1(const char *host)
{
	int i;

	for (i = 0; i < net_h2_n_known_h1; i++)
		if (!PL_strcasecmp(net_h2_known_h1[i], host))
			return TRUE;
	return FALSE;
}

MODULE_PRIVATE XP_Bool
NET_H2_Connecting(PRFileDesc *sock, const char *host)
{
	NetH2Session *s;
	PRInt32 osfd;

	if (!sock || !host || !NET_H2_Enabled() || net_h2_is_known_h1(host)
		|| !net_h2_layer_init())
		return FALSE;
	for (s = net_h2_sessions; s; s = s->next)
		if (!s->dead && !s->going_away && !PL_strcasecmp(s->host, host))
			return FALSE;	/* one is enough to wait for */
	if ((osfd = PR_FileDesc2NativeHandle(sock)) < 0
		|| !(s = PR_NEWZAP(NetH2Session)))
		return FALSE;
	s->host = PL_strdup(host);
	s->connecting = sock;
	s->osfd = osfd;
	s->opened = s->idle_since = PR_IntervalNow();
	s->next = net_h2_sessions;
	net_h2_sessions = s;
	net_h2_log("%s: connecting", s->host);
	return TRUE;
}

MODULE_PRIVATE void
NET_H2_NotH2(PRFileDesc *sock, const char *host)
{
	NetH2Session *s = net_h2_pending_of(sock);

	if (host && !net_h2_is_known_h1(host)) {
		if (net_h2_n_known_h1 == H2_KNOWN_H1) {
			PR_Free(net_h2_known_h1[0]);
			memmove(net_h2_known_h1, net_h2_known_h1 + 1,
					(H2_KNOWN_H1 - 1) * sizeof net_h2_known_h1[0]);
			net_h2_n_known_h1--;
		}
		net_h2_known_h1[net_h2_n_known_h1++] = PL_strdup(host);
	}
	if (s) {
		s->connecting = NULL;
		net_h2_session_dead(s, "the server speaks HTTP/1.1");
		net_h2_reap();
		net_h2_kick();
	}
}

MODULE_PRIVATE void
NET_H2_Abandon(PRFileDesc *sock)
{
	NetH2Session *s = net_h2_pending_of(sock);

	if (s) {
		s->connecting = NULL;
		net_h2_session_dead(s, "the connection failed");
		net_h2_reap();
		net_h2_kick();
	}
}

MODULE_PRIVATE PRFileDesc *
NET_H2_Adopt(PRFileDesc *sock, const char *host)
{
	NetH2Session *s;
	NetH2Stream *st;
	nghttp2_option *opt = NULL;
	nghttp2_settings_entry iv[3];
	PRFileDesc *fd;

	if (!net_h2_layer_init()) {
		NET_H2_Abandon(sock);
		PR_Close(sock);
		return NULL;
	}
	/* the session requests are already waiting on, or a new one */
	if (!(s = net_h2_pending_of(sock))) {
		if (!(s = PR_NEWZAP(NetH2Session))) {
			PR_Close(sock);
			return NULL;
		}
		s->host = PL_strdup(host);
		s->opened = PR_IntervalNow();
		s->next = net_h2_sessions;
		net_h2_sessions = s;
	}
	s->connecting = NULL;
	s->sock = sock;
	s->osfd = PR_FileDesc2NativeHandle(sock);
	s->idle_since = PR_IntervalNow();
	/* windows open as the HTTP code reads (nghttp2_session_consume) */
	nghttp2_option_new(&opt);
	if (opt)
		nghttp2_option_set_no_auto_window_update(opt, 1);
	if (nghttp2_session_client_new2(&s->ng, net_h2_callbacks, s, opt) != 0) {
		s->ng = NULL;
		if (opt)
			nghttp2_option_del(opt);
		net_h2_session_dead(s, "no session");
		net_h2_reap();		/* closes SOCK, if no stream waits */
		return NULL;
	}
	if (opt)
		nghttp2_option_del(opt);
	iv[0].settings_id = NGHTTP2_SETTINGS_ENABLE_PUSH;
	iv[0].value = 0;
	iv[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
	iv[1].value = H2_STREAM_WINDOW;
	iv[2].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
	iv[2].value = 100;
	nghttp2_submit_settings(s->ng, NGHTTP2_FLAG_NONE, iv, 3);
	nghttp2_session_set_local_window_size(s->ng, NGHTTP2_FLAG_NONE, 0,
										  H2_SESSION_WINDOW);
	net_h2_log("%s: session opened (%d waiting)", s->host, net_h2_active(s));
	net_h2_trim();

	/* the requests that waited for the connection go out with this one */
	for (st = s->streams; st; st = st->next)
		if (st->complete && !st->submitted && !st->closed
			&& !net_h2_submit(st))
			st->failed = st->closed = TRUE;
	if (!(fd = net_h2_new_stream(s))) {
		net_h2_session_dead(s, "no stream");
		net_h2_reap();
		return NULL;
	}
	net_h2_pump(s);
	net_h2_kick();
	return fd;
}

MODULE_PRIVATE XP_Bool
NET_H2_IsStream(PRFileDesc *fd)
{
	return net_h2_stream(fd) != NULL;
}
