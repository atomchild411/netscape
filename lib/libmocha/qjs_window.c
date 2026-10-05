/* -*- Mode: C; tab-width: 4 -*-
 *   qjs_window.c --- libmocha on QuickJS: a window's globals.
 *
 * The window's objects (window, document, location, navigator, history,
 * screen, console, timers, dialogs) are made by a JavaScript prelude
 * (qjs_prelude below) from a few natives that only C can provide, kept on a
 * hidden global (__ns).  The document and the DOM are qjs_dom.c's.
 */

#include "qjs.h"

#include "fe_proto.h"
#include "shist.h"
#include "prprf.h"
#include "gui.h"

#include <string.h>

/* network/protocol/http/cookies.h */
extern char *NET_GetCookie(MWContext *context, char *address);
extern void NET_SetCookieString(MWContext *context, char *cur_url,
								char *set_cookie_header);

/* lib/layout/layscrip.c */
extern int LO_QJSDocWrite(MWContext *context, NET_StreamClass *stream,
						  char *str, int32 len, int32 doc_id,
						  XP_Bool self_modifying);

static MochaDecoder *
qjs_decoder(JSContext *cx)
{
	return (MochaDecoder *) JS_GetContextOpaque(cx);
}

static const char *
qjs_doc_url(MochaDecoder *d)
{
	History_entry *he;

	if (d->url_struct && d->url_struct->address)
		return LM_StripWysiwygURLPrefix(d->url_struct->address);
	he = SHIST_GetCurrent(&d->window_context->hist);
	return he ? LM_StripWysiwygURLPrefix(he->address) : "";
}

/* ---- natives -------------------------------------------------------------- */

/* resolve(url, base): URL made absolute against BASE (default: the
 * document's URL) */
static JSValue
ns_resolve(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	const char *rel, *base = NULL;
	char *abs;
	JSValue v;

	if (argc < 1 || !d || !(rel = JS_ToCString(cx, argv[0])))
		return JS_EXCEPTION;
	if (argc > 1 && JS_IsString(argv[1]))
		base = JS_ToCString(cx, argv[1]);
	abs = NET_MakeAbsoluteURL((char *) (base && *base ? base : qjs_doc_url(d)),
							  (char *) rel);
	v = JS_NewString(cx, abs ? abs : rel);
	XP_FREEIF(abs);
	JS_FreeCString(cx, rel);
	if (base)
		JS_FreeCString(cx, base);
	return v;
}

/* ---- ES modules ----------------------------------------------------------
 *
 * QuickJS loads a module's imports synchronously, through the runtime's
 * loader; netlib fetches asynchronously.  So qjs_dom.js fetches a module's
 * whole graph first (its static imports, and literal dynamic ones) into
 * __ns_modsrc (URL -> source); then evalModule runs the entry and the
 * loader below serves the imports from there.  Specifiers are URLs relative
 * to the importing module, or names in the page's import map
 * (__ns_importmap).
 */

static JSValue
qjs_global_fn(JSContext *cx, const char *name)
{
	JSValue g = JS_GetGlobalObject(cx), f = JS_GetPropertyStr(cx, g, name);

	JS_FreeValue(cx, g);
	return f;
}

char *
qjs_ModuleNormalize(JSContext *cx, const char *base, const char *name,
					void *opaque)
{
	char *abs, *r;

	if (name[0] != '.' && name[0] != '/' && !XP_STRCHR(name, ':')) {
		/* a bare specifier: the import map */
		JSValue f = qjs_global_fn(cx, "__ns_importmap"), v = JS_UNDEFINED;
		if (JS_IsFunction(cx, f)) {
			JSValue a = JS_NewString(cx, name);
			v = JS_Call(cx, f, JS_UNDEFINED, 1, &a);
			JS_FreeValue(cx, a);
		}
		JS_FreeValue(cx, f);
		if (JS_IsString(v)) {
			const char *m = JS_ToCString(cx, v);
			r = m ? js_strdup(cx, m) : NULL;
			if (m)
				JS_FreeCString(cx, m);
			JS_FreeValue(cx, v);
			return r;
		}
		JS_FreeValue(cx, v);
		JS_ThrowTypeError(cx, "module specifier '%s' is not in the import map", name);
		return NULL;
	}
	abs = NET_MakeAbsoluteURL((char *) base, (char *) name);
	r = js_strdup(cx, abs ? abs : name);
	XP_FREEIF(abs);
	return r;
}

JSModuleDef *
qjs_ModuleLoad(JSContext *cx, const char *name, void *opaque)
{
	JSValue map = qjs_global_fn(cx, "__ns_modsrc"), src, fn, meta;
	JSModuleDef *m;
	const char *text;
	size_t len;

	src = JS_IsObject(map) ? JS_GetPropertyStr(cx, map, name) : JS_UNDEFINED;
	JS_FreeValue(cx, map);
	if (!JS_IsString(src)) {
		JS_FreeValue(cx, src);
		JS_ThrowReferenceError(cx, "module '%s' was not loaded", name);
		return NULL;
	}
	text = JS_ToCStringLen(cx, &len, src);
	JS_FreeValue(cx, src);
	if (!text)
		return NULL;
	fn = JS_Eval(cx, text, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	JS_FreeCString(cx, text);
	if (JS_IsException(fn))
		return NULL;
	m = (JSModuleDef *) JS_VALUE_GET_PTR(fn);
	meta = JS_GetImportMeta(cx, m);
	if (JS_IsObject(meta)) {
		JS_SetPropertyStr(cx, meta, "url", JS_NewString(cx, name));
		JS_SetPropertyStr(cx, meta, "main", JS_FALSE);
	}
	JS_FreeValue(cx, meta);
	JS_FreeValue(cx, fn);
	return m;
}

/* evalModule(source, url): run a module (its imports must be in
 * __ns_modsrc); a promise for its evaluation */
static JSValue
ns_eval_module(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *src, *url;
	size_t len;
	JSValue v;

	if (argc < 2 || !(src = JS_ToCStringLen(cx, &len, argv[0])))
		return JS_EXCEPTION;
	if (!(url = JS_ToCString(cx, argv[1]))) {
		JS_FreeCString(cx, src);
		return JS_EXCEPTION;
	}
	v = JS_Eval(cx, src, len, url, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	JS_FreeCString(cx, src);
	if (!JS_IsException(v)) {
		JSValue meta = JS_GetImportMeta(cx, (JSModuleDef *) JS_VALUE_GET_PTR(v));
		if (JS_IsObject(meta)) {
			JS_SetPropertyStr(cx, meta, "url", JS_NewString(cx, url));
			JS_SetPropertyStr(cx, meta, "main", JS_TRUE);
		}
		JS_FreeValue(cx, meta);
		v = JS_EvalFunction(cx, v);
	}
	JS_FreeCString(cx, url);
	return v;
}

/* evalScript(source, url): run a script the page inserted, in the window */
static JSValue
ns_eval_script(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	const char *src, *url = NULL;
	size_t len;
	XP_Bool ok;

	if (argc < 1 || !d || !(src = JS_ToCStringLen(cx, &len, argv[0])))
		return JS_EXCEPTION;
	if (argc > 1 && JS_IsString(argv[1]))
		url = JS_ToCString(cx, argv[1]);
	ok = qjs_Evaluate(d, src, len, url ? url : "<inserted script>", 1, NULL);
	JS_FreeCString(cx, src);
	if (url)
		JS_FreeCString(cx, url);
	return JS_NewBool(cx, ok);
}

static JSValue
ns_write(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	const char *s;
	size_t len;
	char *text;

	if (argc < 1 || !d)
		return JS_UNDEFINED;
	s = JS_ToCStringLen(cx, &len, argv[0]);
	if (!s)
		return JS_EXCEPTION;
	if (!d->stream) {
		qjs_Log("JavaScript: document.write after the page loaded: ignored");
	} else {
		text = qjs_ToDocumentCharset(d->window_context, s, len, &len);
		if (text) {
			LO_QJSDocWrite(d->window_context, d->stream, text, (int32) len,
						   d->doc_id, TRUE);
			XP_FREE(text);
		}
	}
	JS_FreeCString(cx, s);
	return JS_UNDEFINED;
}

static char *
qjs_arg_doc(JSContext *cx, MochaDecoder *d, JSValueConst v)
{
	size_t len;
	const char *s = JS_ToCStringLen(cx, &len, v);
	char *t;

	if (!s)
		return NULL;
	t = qjs_ToDocumentCharset(d->window_context, s, len, NULL);
	JS_FreeCString(cx, s);
	return t;
}

static JSValue
ns_alert(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	char *msg;

	if (!d || argc < 1)
		return JS_UNDEFINED;
	msg = qjs_arg_doc(cx, d, argv[0]);
	if (msg) {
		FE_Alert(d->window_context, msg);
		XP_FREE(msg);
	}
	return JS_UNDEFINED;
}

static JSValue
ns_confirm(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	char *msg;
	Bool ok = FALSE;

	if (!d || argc < 1)
		return JS_FALSE;
	msg = qjs_arg_doc(cx, d, argv[0]);
	if (msg) {
		ok = FE_Confirm(d->window_context, msg);
		XP_FREE(msg);
	}
	return JS_NewBool(cx, ok);
}

static JSValue
ns_prompt(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	char *msg, *dflt = NULL, *answer;
	JSValue rv = JS_NULL;

	if (!d || argc < 1)
		return JS_NULL;
	msg = qjs_arg_doc(cx, d, argv[0]);
	if (argc > 1 && !JS_IsUndefined(argv[1]))
		dflt = qjs_arg_doc(cx, d, argv[1]);
	answer = FE_Prompt(d->window_context, msg ? msg : "", dflt ? dflt : "");
	if (answer) {
		char *u = qjs_FromDocumentCharset(d->window_context, answer,
										  XP_STRLEN(answer));
		rv = JS_NewString(cx, u ? u : "");
		XP_FREEIF(u);
		XP_FREE(answer);
	}
	XP_FREEIF(msg);
	XP_FREEIF(dflt);
	return rv;
}

static JSValue
ns_log(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *s = argc > 0 ? JS_ToCString(cx, argv[0]) : NULL;

	qjs_Log("console: %s", s ? s : "");
	if (s)
		JS_FreeCString(cx, s);
	return JS_UNDEFINED;
}

static JSValue
ns_url(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);

	return JS_NewString(cx, d ? qjs_doc_url(d) : "");
}

static JSValue
ns_referrer(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	const char *r = (d && d->url_struct && d->url_struct->referer) ?
		d->url_struct->referer : "";

	return JS_NewString(cx, r);
}

static JSValue
ns_title(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	char *u;
	JSValue rv;

	if (!d || !d->window_context->title)
		return JS_NewString(cx, "");
	u = qjs_FromDocumentCharset(d->window_context, d->window_context->title,
								XP_STRLEN(d->window_context->title));
	rv = JS_NewString(cx, u ? u : "");
	XP_FREEIF(u);
	return rv;
}

static JSValue
ns_cookie(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	char *c;
	JSValue rv;

	if (!d)
		return JS_NewString(cx, "");
	c = NET_GetCookie(d->window_context, (char *) qjs_doc_url(d));
	rv = JS_NewString(cx, c ? c : "");
	XP_FREEIF(c);
	return rv;
}

static JSValue
ns_set_cookie(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	const char *s;

	if (!d || argc < 1)
		return JS_UNDEFINED;
	s = JS_ToCString(cx, argv[0]);
	if (s) {
		char *url = XP_STRDUP(qjs_doc_url(d)), *c = XP_STRDUP(s);
		if (url && c)
			NET_SetCookieString(d->window_context, url, c);
		XP_FREEIF(url);
		XP_FREEIF(c);
		JS_FreeCString(cx, s);
	}
	return JS_UNDEFINED;
}

/* Navigate (location.href = ..., assign, replace, reload), later: not from
 * inside layout or the script. */
typedef struct {
	MWContext	*context;
	int32		doc_id;
	char		*url;
	NET_ReloadMethod reload;
} qjs_Navigation;

static void
qjs_navigate_later(void *arg)
{
	qjs_Navigation *n = (qjs_Navigation *) arg;

	if (n->doc_id == XP_DOCID(n->context)) {
		URL_Struct *u = NET_CreateURLStruct(n->url, n->reload);
		if (u)
			FE_GetURL(n->context, u);
	}
	XP_FREE(n->url);
	XP_FREE(n);
}

static JSValue
ns_navigate(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	const char *s;
	qjs_Navigation *n;
	int32 reload = 0;

	if (!d || argc < 1)
		return JS_UNDEFINED;
	if (argc > 1)
		JS_ToInt32(cx, &reload, argv[1]);
	s = JS_ToCString(cx, argv[0]);
	if (!s)
		return JS_EXCEPTION;
	n = XP_NEW_ZAP(qjs_Navigation);
	if (n) {
		n->context = d->window_context;
		n->doc_id = XP_DOCID(d->window_context);
		n->url = NET_MakeAbsoluteURL((char *) qjs_doc_url(d), (char *) s);
		n->reload = reload ? NET_SUPER_RELOAD : NET_DONT_RELOAD;
		if (n->url)
			qjs_Later(qjs_navigate_later, n);
		else
			XP_FREE(n);
	}
	JS_FreeCString(cx, s);
	return JS_UNDEFINED;
}

static JSValue
ns_history_go(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	int32 n = 0;

	if (!d || argc < 1)
		return JS_UNDEFINED;
	JS_ToInt32(cx, &n, argv[0]);
	if (n < 0)
		FE_BackCommand(d->window_context);
	else if (n > 0)
		FE_ForwardCommand(d->window_context);
	return JS_UNDEFINED;
}

static JSValue
ns_history_length(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);

	return JS_NewInt32(cx, d ? d->window_context->hist.num_entries : 0);
}

static JSValue
ns_agent(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	JSValue o = JS_NewObject(cx);

	JS_SetPropertyStr(cx, o, "appName", JS_NewString(cx, XP_AppName ? XP_AppName : "Netscape"));
	JS_SetPropertyStr(cx, o, "appCodeName", JS_NewString(cx, XP_AppCodeName ? XP_AppCodeName : "Mozilla"));
	JS_SetPropertyStr(cx, o, "appVersion", JS_NewString(cx, XP_AppVersion ? XP_AppVersion : "5.0"));
	JS_SetPropertyStr(cx, o, "language", JS_NewString(cx, XP_AppLanguage ? XP_AppLanguage : "en"));
	JS_SetPropertyStr(cx, o, "platform", JS_NewString(cx, XP_AppPlatform ? XP_AppPlatform : "IRIX"));
	return o;
}

static JSValue
ns_window_size(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	int32 w = 800, h = 600;
	JSValue o = JS_NewObject(cx);
	Chrome chrome;

	if (d) {
		XP_MEMSET(&chrome, 0, sizeof chrome);
		FE_QueryChrome(d->window_context, &chrome);
		if (chrome.w_hint > 0 && chrome.h_hint > 0) {
			w = chrome.w_hint;
			h = chrome.h_hint;
		}
	}
	JS_SetPropertyStr(cx, o, "w", JS_NewInt32(cx, w));
	JS_SetPropertyStr(cx, o, "h", JS_NewInt32(cx, h));
	return o;
}

/* ---- timers --------------------------------------------------------------- */

typedef struct qjs_Timer {
	struct qjs_Timer *next;
	MochaDecoder	*decoder;
	JSContext		*cx;
	JSValue			fn;
	int32			id;
	uint32			ms;
	XP_Bool			repeat;
	void			*fe_timer;
} qjs_Timer;

static int32 qjs_next_timer_id = 1;

static void
qjs_timer_free(qjs_Timer *t)
{
	qjs_Timer **pp;

	for (pp = (qjs_Timer **) &t->decoder->timeouts; *pp; pp = &(*pp)->next)
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	JS_FreeValue(t->cx, t->fn);
	XP_FREE(t);
}

static void qjs_timer_fire(void *closure);

static void
qjs_timer_arm(qjs_Timer *t)
{
	t->fe_timer = FE_SetTimeout(qjs_timer_fire, t, t->ms ? t->ms : 1);
}

static void
qjs_timer_fire(void *closure)
{
	qjs_Timer *t = (qjs_Timer *) closure;
	JSContext *cx = t->cx;
	JSValue fn = JS_DupValue(cx, t->fn), rv;
	XP_Bool repeat = t->repeat;

	t->fe_timer = NULL;
	if (repeat)
		qjs_timer_arm(t);
	rv = qjs_Call(cx, fn, JS_UNDEFINED, 0, NULL);
	JS_FreeValue(cx, rv);
	JS_FreeValue(cx, fn);
	/* the call may have cleared it (clearTimeout) or dropped the context */
	if (!repeat) {
		qjs_Timer *p;
		for (p = (qjs_Timer *) t->decoder->timeouts; p; p = p->next)
			if (p == t) {
				qjs_timer_free(t);
				break;
			}
	}
}

static JSValue
ns_timer(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	qjs_Timer *t;
	int32 ms = 0, repeat = 0;

	if (!d || argc < 1 || !JS_IsFunction(cx, argv[0]))
		return JS_NewInt32(cx, 0);
	if (argc > 1)
		JS_ToInt32(cx, &ms, argv[1]);
	if (argc > 2)
		JS_ToInt32(cx, &repeat, argv[2]);
	t = XP_NEW_ZAP(qjs_Timer);
	if (!t)
		return JS_NewInt32(cx, 0);
	t->decoder = d;
	t->cx = cx;
	t->fn = JS_DupValue(cx, argv[0]);
	t->id = qjs_next_timer_id++;
	t->ms = ms < 0 ? 0 : ms;
	t->repeat = repeat != 0;
	if (t->repeat && t->ms < 10)
		t->ms = 10;
	t->next = (qjs_Timer *) d->timeouts;
	d->timeouts = (JSTimeout *) t;
	qjs_timer_arm(t);
	return JS_NewInt32(cx, t->id);
}

static JSValue
ns_clear_timer(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *d = qjs_decoder(cx);
	qjs_Timer *t;
	int32 id = 0;

	if (!d || argc < 1)
		return JS_UNDEFINED;
	JS_ToInt32(cx, &id, argv[0]);
	for (t = (qjs_Timer *) d->timeouts; t; t = t->next)
		if (t->id == id) {
			if (t->fe_timer)
				FE_ClearTimeout(t->fe_timer);
			qjs_timer_free(t);
			break;
		}
	return JS_UNDEFINED;
}

void
qjs_ClearTimeouts(MochaDecoder *decoder)
{
	while (decoder->timeouts) {
		qjs_Timer *t = (qjs_Timer *) decoder->timeouts;
		if (t->fe_timer)
			FE_ClearTimeout(t->fe_timer);
		qjs_timer_free(t);
	}
}

static const JSCFunctionListEntry ns_functions[] = {
	JS_CFUNC_DEF("write", 1, ns_write),
	JS_CFUNC_DEF("alert", 1, ns_alert),
	JS_CFUNC_DEF("confirm", 1, ns_confirm),
	JS_CFUNC_DEF("prompt", 2, ns_prompt),
	JS_CFUNC_DEF("log", 1, ns_log),
	JS_CFUNC_DEF("url", 0, ns_url),
	JS_CFUNC_DEF("referrer", 0, ns_referrer),
	JS_CFUNC_DEF("title", 0, ns_title),
	JS_CFUNC_DEF("cookie", 0, ns_cookie),
	JS_CFUNC_DEF("setCookie", 1, ns_set_cookie),
	JS_CFUNC_DEF("navigate", 2, ns_navigate),
	JS_CFUNC_DEF("historyGo", 1, ns_history_go),
	JS_CFUNC_DEF("historyLength", 0, ns_history_length),
	JS_CFUNC_DEF("agent", 0, ns_agent),
	JS_CFUNC_DEF("windowSize", 0, ns_window_size),
	JS_CFUNC_DEF("timer", 3, ns_timer),
	JS_CFUNC_DEF("resolve", 2, ns_resolve),
	JS_CFUNC_DEF("evalScript", 2, ns_eval_script),
	JS_CFUNC_DEF("evalModule", 2, ns_eval_module),
	JS_CFUNC_DEF("load", 5, qjs_ns_load),
	JS_CFUNC_DEF("clearTimer", 1, ns_clear_timer),
};

/* ---- the prelude: the window's objects, in JavaScript ---------------------- */

static const char qjs_prelude[] =
"(function(g, ns) {\n"
"'use strict';\n"
"function listeners(o) {\n"
"  var l = {};\n"
"  o.addEventListener = function(t, f) { (l[t] = l[t] || []).push(f); };\n"
"  o.removeEventListener = function(t, f) {\n"
"    var a = l[t]; if (a) { var i = a.indexOf(f); if (i >= 0) a.splice(i, 1); } };\n"
"  o.dispatchEvent = function(e) {\n"
"    var t = typeof e === 'string' ? e : e.type, a = (l[t] || []).slice();\n"
"    var h = o['on' + t];\n"
"    if (typeof h === 'function') a.unshift(h);\n"
"    for (var i = 0; i < a.length; i++) {\n"
"      try { typeof a[i] === 'function' ? a[i].call(o, e) : a[i].handleEvent(e); }\n"
"      catch (x) { ns.log('uncaught in ' + t + ' listener: ' + x + (x && x.stack ? '\\n' + x.stack : '')); }\n"
"    }\n"
"    return true;\n"
"  };\n"
"}\n"
"function parse(u) {\n"
"  var m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)?(\\/\\/([^\\/?#:]*)(:([0-9]*))?)?([^?#]*)(\\?[^#]*)?(#.*)?$/.exec(u) || [];\n"
"  return { protocol: m[1] || '', hostname: m[3] || '', port: m[5] || '',\n"
"           pathname: m[6] || '', search: m[7] || '', hash: m[8] || '' };\n"
"}\n"
"g.window = g.self = g.top = g.parent = g.frames = g.globalThis = g;\n"
"g.opener = null; g.closed = false; g.name = '';\n"
"listeners(g);\n"
"var location = {};\n"
"function locprop(name) {\n"
"  Object.defineProperty(location, name, { enumerable: true,\n"
"    get: function() { return parse(ns.url())[name]; } });\n"
"}\n"
"['protocol', 'hostname', 'port', 'pathname', 'search', 'hash'].forEach(locprop);\n"
"Object.defineProperty(location, 'href', { enumerable: true,\n"
"  get: function() { return ns.url(); }, set: function(u) { ns.navigate(String(u), 0); } });\n"
"Object.defineProperty(location, 'host', { enumerable: true, get: function() {\n"
"  var p = parse(ns.url()); return p.hostname + (p.port ? ':' + p.port : ''); } });\n"
"Object.defineProperty(location, 'origin', { enumerable: true, get: function() {\n"
"  var p = parse(ns.url()); return p.protocol + '//' + location.host; } });\n"
"location.assign = function(u) { ns.navigate(String(u), 0); };\n"
"location.replace = function(u) { ns.navigate(String(u), 0); };\n"
"location.reload = function() { ns.navigate(ns.url(), 1); };\n"
"location.toString = function() { return ns.url(); };\n"
"Object.defineProperty(g, 'location', { enumerable: true,\n"
"  get: function() { return location; }, set: function(u) { ns.navigate(String(u), 0); } });\n"
"var a = ns.agent();\n"
"g.navigator = { appName: a.appName, appCodeName: a.appCodeName,\n"
"  appVersion: a.appVersion, userAgent: a.appCodeName + '/' + a.appVersion,\n"
"  platform: a.platform, language: a.language, languages: [a.language],\n"
"  cookieEnabled: true, onLine: true, vendor: '', product: 'Gecko',\n"
"  plugins: [], mimeTypes: [], maxTouchPoints: 0, hardwareConcurrency: 1,\n"
"  javaEnabled: function() { return false; } };\n"
"g.screen = { width: 1280, height: 1024, availWidth: 1280, availHeight: 1024,\n"
"  colorDepth: 24, pixelDepth: 24 };\n"
"Object.defineProperty(g, 'innerWidth', { get: function() { return ns.windowSize().w; } });\n"
"Object.defineProperty(g, 'innerHeight', { get: function() { return ns.windowSize().h; } });\n"
"g.outerWidth = 1280; g.outerHeight = 1024; g.devicePixelRatio = 1;\n"
"g.scrollX = g.scrollY = g.pageXOffset = g.pageYOffset = 0;\n"
"g.scrollTo = g.scroll = g.scrollBy = function() {};\n"
"g.focus = g.blur = g.print = g.stop = function() {};\n"
"g.history = { back: function() { ns.historyGo(-1); }, forward: function() { ns.historyGo(1); },\n"
"  go: function(n) { ns.historyGo(n | 0); }, pushState: function() {}, replaceState: function() {},\n"
"  state: null };\n"
"Object.defineProperty(g.history, 'length', { get: function() { return ns.historyLength(); } });\n"
"g.alert = function(m) { ns.alert(String(m === undefined ? '' : m)); };\n"
"g.confirm = function(m) { return ns.confirm(String(m === undefined ? '' : m)); };\n"
"g.prompt = function(m, d) { return ns.prompt(String(m === undefined ? '' : m), d); };\n"
"function cb(f, args) {\n"
"  if (typeof f === 'function') return function() { f.apply(g, args); };\n"
"  var src = String(f); return function() { (0, eval)(src); };\n"
"}\n"
"g.setTimeout = function(f, ms) { return ns.timer(cb(f, [].slice.call(arguments, 2)), ms | 0, 0); };\n"
"g.setInterval = function(f, ms) { return ns.timer(cb(f, [].slice.call(arguments, 2)), ms | 0, 1); };\n"
"g.clearTimeout = g.clearInterval = function(id) { ns.clearTimer(id | 0); };\n"
"g.requestAnimationFrame = function(f) { return ns.timer(function() { f(Date.now()); }, 16, 0); };\n"
"g.cancelAnimationFrame = g.clearTimeout;\n"
"g.queueMicrotask = function(f) { Promise.resolve().then(f); };\n"
"function out(level) { return function() {\n"
"  ns.log(level + [].map.call(arguments, function(x) {\n"
"    try { return typeof x === 'string' ? x : JSON.stringify(x); } catch (e) { return String(x); }\n"
"  }).join(' ')); }; }\n"
"g.console = { log: out(''), info: out(''), debug: out(''), warn: out('warning: '),\n"
"  error: out('error: '), trace: out('trace: '), dir: out(''), table: out(''),\n"
"  group: function() {}, groupEnd: function() {}, time: function() {}, timeEnd: function() {},\n"
"  assert: function(c) { if (!c) out('assertion failed: ').apply(null, [].slice.call(arguments, 1)); } };\n"
"function storage() { var s = {};\n"
"  return { getItem: function(k) { return k in s ? s[k] : null; },\n"
"    setItem: function(k, v) { s[k] = String(v); }, removeItem: function(k) { delete s[k]; },\n"
"    clear: function() { s = {}; }, key: function(i) { return Object.keys(s)[i] || null; },\n"
"    get length() { return Object.keys(s).length; } }; }\n"
"g.localStorage = storage(); g.sessionStorage = storage();\n"
"var b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';\n"
"g.btoa = function(s) { s = String(s); var o = '', i;\n"
"  for (i = 0; i < s.length; i += 3) {\n"
"    var n = (s.charCodeAt(i) << 16) | ((s.charCodeAt(i + 1) || 0) << 8) | (s.charCodeAt(i + 2) || 0);\n"
"    o += b64[n >> 18 & 63] + b64[n >> 12 & 63] +\n"
"      (i + 1 < s.length ? b64[n >> 6 & 63] : '=') + (i + 2 < s.length ? b64[n & 63] : '='); }\n"
"  return o; };\n"
"g.atob = function(s) { s = String(s).replace(/[^A-Za-z0-9+\\/]/g, ''); var o = '', i, n;\n"
"  for (i = 0; i < s.length; i += 4) {\n"
"    n = (b64.indexOf(s[i]) << 18) | (b64.indexOf(s[i + 1]) << 12) |\n"
"      ((b64.indexOf(s[i + 2]) & 63) << 6) | (b64.indexOf(s[i + 3]) & 63);\n"
"    o += String.fromCharCode(n >> 16 & 255);\n"
"    if (s[i + 2] !== undefined) o += String.fromCharCode(n >> 8 & 255);\n"
"    if (s[i + 3] !== undefined) o += String.fromCharCode(n & 255); }\n"
"  return o; };\n"
"})(globalThis, globalThis.__ns);\n";

void
qjs_InitWindow(JSContext *cx, MochaDecoder *decoder)
{
	JSValue global = JS_GetGlobalObject(cx), ns, v;

	ns = JS_NewObject(cx);
	JS_SetPropertyFunctionList(cx, ns, ns_functions,
							   (int) (sizeof ns_functions / sizeof ns_functions[0]));
	JS_DefinePropertyValueStr(cx, global, "__ns", ns, 0);	/* hidden */
	v = JS_Eval(cx, qjs_prelude, sizeof qjs_prelude - 1, "<netscape>",
				JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(v))
		qjs_ReportException(cx);
	JS_FreeValue(cx, v);
	qjs_InitDom(cx, ns);
	JS_FreeValue(cx, global);
}

/* ---- events from layout and the front end ---------------------------------- */

static void
qjs_fire(JSContext *cx, const char *type)
{
	JSValue global = JS_GetGlobalObject(cx);
	JSValue fire = JS_GetPropertyStr(cx, global, "__ns_fire");
	JSValue arg = JS_NewString(cx, type), rv;

	if (JS_IsFunction(cx, fire)) {
		rv = qjs_Call(cx, fire, global, 1, &arg);
		JS_FreeValue(cx, rv);
	}
	JS_FreeValue(cx, arg);
	JS_FreeValue(cx, fire);
	JS_FreeValue(cx, global);
}

void
qjs_SendLoadEvent(MWContext *context, int32 type, JSBool resize_reload)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, FALSE);

	if (decoder && !decoder->js_context && type == EVENT_LOAD &&
		!resize_reload && qjs_DomHasModules(decoder))
		qjs_GetContext(decoder);
	if (!decoder || !decoder->js_context)
		return;
	switch (type) {
	case EVENT_LOAD:
		if (resize_reload) {
			qjs_DomRelaidOut(decoder);
			return;
		}
		if (decoder->load_event_sent)
			return;
		decoder->load_event_sent = TRUE;
		qjs_fire(decoder->js_context, "load");
		qjs_DomLoaded(decoder);
		break;
	case EVENT_UNLOAD:
		if (!resize_reload)
			qjs_fire(decoder->js_context, "unload");
		break;
	default:
		break;
	}
}

/* <BODY onload=... onunload=...>: window.onload and window.onunload. */
static void
qjs_set_handler(JSContext *cx, const char *name, const char *src)
{
	char *code;
	JSValue fn, global;

	code = PR_smprintf("(function(event) {\n%s\n})", src);
	if (!code)
		return;
	fn = JS_Eval(cx, code, XP_STRLEN(code), "<body handler>", JS_EVAL_TYPE_GLOBAL);
	PR_smprintf_free(code);
	if (JS_IsException(fn)) {
		qjs_ReportException(cx);
		return;
	}
	global = JS_GetGlobalObject(cx);
	JS_SetPropertyStr(cx, global, name, fn);
	JS_FreeValue(cx, global);
}

void
qjs_SetWindowHandlers(MWContext *context, char *onload, char *onunload)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, TRUE);
	JSContext *cx = qjs_GetContext(decoder);
	char *u;

	if (!cx)
		return;
	if (onload && (u = qjs_FromDocumentCharset(context, onload, XP_STRLEN(onload)))) {
		qjs_set_handler(cx, "onload", u);
		XP_FREE(u);
	}
	if (onunload && (u = qjs_FromDocumentCharset(context, onunload, XP_STRLEN(onunload)))) {
		qjs_set_handler(cx, "onunload", u);
		XP_FREE(u);
	}
}

/* Clicks, key presses, form submits: to their elements (qjs_dom.c).
 * TRUE: go on with the default action. */
JSBool
qjs_SendEvent(MWContext *context, LO_Element *element, JSEvent *event)
{
	JSBool ok = qjs_DomEvent(context, element, event);

	/* mouse over a link: the status line shows its URL unless a handler
	 * said otherwise */
	return event->type == EVENT_MOUSEOVER ? FALSE : ok;
}
