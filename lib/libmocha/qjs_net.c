/* -*- Mode: C; tab-width: 4 -*-
 *   qjs_net.c --- libmocha on QuickJS: fetching for scripts.
 *
 * __ns.load(url, method, body, headers, callback) fetches URL through
 * netlib, as the page's own loads go (cache, cookies, proxies), and calls
 * callback(status, text, contentType, finalURL) from the event queue when
 * it is over (status 0: it failed).  XMLHttpRequest, fetch() and the
 * scripts a page inserts (SCRIPT SRC= made by script) are built on it, in
 * JavaScript (qjs_dom.js).
 *
 * The answer comes back as text: UTF-8 if it is UTF-8, else taken as
 * ISO 8859-1.
 */

#include "qjs.h"
#include "prprf.h"
#include "shist.h"
#include "shr_str.h"

#include <string.h>

typedef struct qjs_Load {
	struct qjs_Load	*next;
	MochaDecoder	*decoder;	/* NULL once the window's scripts went */
	JSContext		*cx;
	JSValue			cb;
	char			*buf;
	int32			len, size;
	XP_Bool			complete;
	int				status;
	char			*content_type;
	char			*address;
} qjs_Load;

static void
qjs_load_unlink(qjs_Load *l)
{
	qjs_Load **pp;

	if (!l->decoder)
		return;
	for (pp = (qjs_Load **) &l->decoder->qjs_loads; *pp; pp = &(*pp)->next)
		if (*pp == l) {
			*pp = l->next;
			break;
		}
	l->decoder = NULL;
}

static void
qjs_load_free(qjs_Load *l)
{
	XP_FREEIF(l->buf);
	XP_FREEIF(l->content_type);
	XP_FREEIF(l->address);
	XP_FREE(l);
}

/* The window's scripts are going: forget their callbacks (the loads finish
 * on their own and come to nothing). */
void
qjs_DropLoads(MochaDecoder *decoder)
{
	qjs_Load *l;

	while ((l = (qjs_Load *) decoder->qjs_loads) != NULL) {
		decoder->qjs_loads = l->next;
		JS_FreeValue(l->cx, l->cb);
		l->cb = JS_UNDEFINED;
		l->decoder = NULL;
	}
}

/* ---- the stream ------------------------------------------------------------- */

static int
qjs_load_write(NET_StreamClass *stream, const char *str, int32 len)
{
	qjs_Load *l = (qjs_Load *) stream->data_object;

	if (l->len + len > l->size) {
		int32 size = (l->len + len) * 2 + 4096;
		char *b = (char *) XP_REALLOC(l->buf, size);
		if (!b)
			return MK_OUT_OF_MEMORY;
		l->buf = b;
		l->size = size;
	}
	XP_MEMCPY(l->buf + l->len, str, len);
	l->len += len;
	return len;
}

static unsigned int
qjs_load_write_ready(NET_StreamClass *stream)
{
	return MAX_WRITE_READY;
}

static void
qjs_load_complete(NET_StreamClass *stream)
{
	qjs_Load *l = (qjs_Load *) stream->data_object;

	l->complete = TRUE;
}

static void
qjs_load_abort(NET_StreamClass *stream, int status)
{
}

NET_StreamClass *
LM_LoadConverter(FO_Present_Types format_out, void *data_object,
				 URL_Struct *url_struct, MWContext *context)
{
	qjs_Load *l = (qjs_Load *) url_struct->fe_data;

	if (!l)
		return NULL;
	if (url_struct->content_type)
		StrAllocCopy(l->content_type, url_struct->content_type);
	l->status = url_struct->server_status;
	return NET_NewStream("JavaScript load", (MKStreamWriteFunc) qjs_load_write,
						 (MKStreamCompleteFunc) qjs_load_complete,
						 (MKStreamAbortFunc) qjs_load_abort,
						 (MKStreamWriteReadyFunc) qjs_load_write_ready,
						 l, context);
}

/* ---- the answer ---------------------------------------------------------- */

static XP_Bool
qjs_is_utf8(const unsigned char *s, int32 len)
{
	int32 i = 0;

	while (i < len) {
		unsigned char c = s[i];
		int n;
		if (c < 0x80) {
			i++;
			continue;
		}
		if ((c & 0xe0) == 0xc0)
			n = 1;
		else if ((c & 0xf0) == 0xe0)
			n = 2;
		else if ((c & 0xf8) == 0xf0)
			n = 3;
		else
			return FALSE;
		if (i + n >= len)
			return FALSE;
		for (i++; n > 0; n--, i++)
			if (i >= len || (s[i] & 0xc0) != 0x80)
				return FALSE;
	}
	return TRUE;
}

static JSValue
qjs_load_text(JSContext *cx, qjs_Load *l)
{
	const unsigned char *s = (const unsigned char *) l->buf;
	int32 len = l->len, i, k;
	char *u;
	JSValue v;

	if (!s)
		return JS_NewString(cx, "");
	/* a byte order mark is not text */
	if (len >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) {
		s += 3;
		len -= 3;
	}
	if (qjs_is_utf8(s, len))
		return JS_NewStringLen(cx, (const char *) s, len);
	u = (char *) XP_ALLOC(len * 2 + 1);
	if (!u)
		return JS_ThrowOutOfMemory(cx);
	for (i = k = 0; i < len; i++) {
		if (s[i] < 0x80) {
			u[k++] = s[i];
		} else {
			u[k++] = 0xc0 | (s[i] >> 6);
			u[k++] = 0x80 | (s[i] & 0x3f);
		}
	}
	v = JS_NewStringLen(cx, u, k);
	XP_FREE(u);
	return v;
}

static void
qjs_load_deliver(void *arg)
{
	qjs_Load *l = (qjs_Load *) arg;
	MochaDecoder *decoder = l->decoder;

	if (decoder && decoder->js_context == l->cx) {
		JSContext *cx = l->cx;
		JSValue cb = l->cb, args[4], rv;
		int status = l->complete ? (l->status > 0 ? l->status : 200) : 0;

		qjs_load_unlink(l);
		args[0] = JS_NewInt32(cx, status);
		args[1] = l->complete ? qjs_load_text(cx, l) : JS_NewString(cx, "");
		args[2] = JS_NewString(cx, l->content_type ? l->content_type : "");
		args[3] = JS_NewString(cx, l->address ? l->address : "");
		rv = qjs_Call(cx, cb, JS_UNDEFINED, 4, args);
		JS_FreeValue(cx, rv);
		JS_FreeValue(cx, args[0]);
		JS_FreeValue(cx, args[1]);
		JS_FreeValue(cx, args[2]);
		JS_FreeValue(cx, args[3]);
		JS_FreeValue(cx, cb);
	}
	qjs_load_free(l);
}

static void
qjs_load_exit(URL_Struct *url_struct, int status, MWContext *context)
{
	qjs_Load *l = (qjs_Load *) url_struct->fe_data;

	url_struct->fe_data = NULL;
	if (l) {
		if (url_struct->address)
			StrAllocCopy(l->address, url_struct->address);
		if (!l->status)
			l->status = url_struct->server_status;
		if (status < 0 && !l->len)
			l->complete = FALSE;
		qjs_Later(qjs_load_deliver, l);
	}
	NET_FreeURLStruct(url_struct);
}

/* load(url, method, body, headers, callback) */
JSValue
qjs_ns_load(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *decoder = (MochaDecoder *) JS_GetContextOpaque(cx);
	const char *url, *method = NULL;
	URL_Struct *us;
	qjs_Load *l;
	XP_Bool post = FALSE;

	if (!decoder || !decoder->window_context || argc < 5 ||
		!JS_IsFunction(cx, argv[4]))
		return JS_ThrowTypeError(cx, "load: bad arguments");
	if (!(url = JS_ToCString(cx, argv[0])))
		return JS_EXCEPTION;
	if (JS_IsString(argv[1]) && (method = JS_ToCString(cx, argv[1])) != NULL)
		post = !XP_STRCASECMP(method, "POST") || !XP_STRCASECMP(method, "PUT");
	us = NET_CreateURLStruct(url, post ? NET_SUPER_RELOAD : NET_DONT_RELOAD);
	JS_FreeCString(cx, url);
	if (method)
		JS_FreeCString(cx, method);
	if (!us)
		return JS_ThrowOutOfMemory(cx);
	l = XP_NEW_ZAP(qjs_Load);
	if (!l) {
		NET_FreeURLStruct(us);
		return JS_ThrowOutOfMemory(cx);
	}
	if (post) {
		size_t blen = 0;
		const char *body = JS_IsString(argv[2]) ? JS_ToCStringLen(cx, &blen, argv[2]) : NULL;
		us->method = URL_POST_METHOD;
		if (body) {
			us->post_data = (char *) XP_ALLOC(blen + 1);
			if (us->post_data) {
				XP_MEMCPY(us->post_data, body, blen);
				us->post_data[blen] = '\0';
				us->post_data_size = (int32) blen;
			}
			JS_FreeCString(cx, body);
		}
		/* an empty body still ends the request's head (NET_WritePostData) */
		if (!us->post_data) {
			us->post_data = XP_STRDUP("");
			us->post_data_size = 0;
		}
	}
	if (JS_IsString(argv[3])) {
		const char *h = JS_ToCString(cx, argv[3]);
		if (h) {
			if (*h)
				StrAllocCopy(us->post_headers, h);
			JS_FreeCString(cx, h);
		}
	}
	if (post) {
		/* the body's length, which the page cannot set: servers (and
		 * HTTP/2) want it; NET_WritePostData ends the head after these */
		char len[48];
		PR_snprintf(len, sizeof len, "Content-Length: %ld\r\n",
					(long) us->post_data_size);
		StrAllocCat(us->post_headers, len);
	}
	{
		History_entry *he = SHIST_GetCurrent(&decoder->window_context->hist);
		if (he && he->address)
			StrAllocCopy(us->referer, he->address);
	}
	l->decoder = decoder;
	l->cx = cx;
	l->cb = JS_DupValue(cx, argv[4]);
	l->next = (qjs_Load *) decoder->qjs_loads;
	decoder->qjs_loads = l;
	us->fe_data = l;
	/* POSTs, and URLs too long for the cache's index, are not cached */
	if (NET_GetURL(us, post || XP_STRLEN(us->address) > 1000 ?
					   FO_QJSLOAD : FO_CACHE_AND_QJSLOAD,
				   decoder->window_context, qjs_load_exit) < 0) {
		/* the exit function has been called */
	}
	return JS_UNDEFINED;
}
