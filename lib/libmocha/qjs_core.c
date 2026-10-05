/* -*- Mode: C; tab-width: 4 -*-
 *   qjs_core.c --- libmocha on QuickJS: the entry points.  See qjs.h.
 *
 * Layout, netlib and the front end call these (libevent.h, libmocha.h) as
 * they called the 1998 engine.  That engine ran JavaScript on a thread of
 * its own and answered through events posted back; here everything runs on
 * the main thread, and the answers layout waits for (a script has run: go
 * on laying out) still arrive later, from the main event queue, so layout's
 * block/unblock sequence is the same.
 *
 * Not here: Java (LiveConnect, applets), plugins, layers, signed scripts,
 * the JavaScript debugger, software update.  Their entry points do nothing.
 */

#include "qjs.h"

#include "prefapi.h"
#include "prmem.h"
#include "prprf.h"
#include "plevent.h"
#include "libi18n.h"
#include "intl_csi.h"
#include "shist.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* layout (lib/layout/layout.h) */
extern void lo_ScriptEvalExitFn(void *data, char *str, size_t len,
								char *wysiwyg_url, char *base_href, Bool valid);

extern PREventQueue *mozilla_event_queue;

char js_content_type[] = APPLICATION_JAVASCRIPT;
char js_language_name[] = "JavaScript";
char lm_unknown_origin_str[] = "[unknown origin]";

static XP_Bool lm_enabled = TRUE;
static const char lm_pref_enabled[] = "javascript.enabled";

/* ---- logging ------------------------------------------------------------ */

void
qjs_Log(const char *fmt, ...)
{
	static FILE *f;
	static int checked;
	va_list ap;

	if (!checked) {
		const char *e = getenv("NETSCAPE_JS_LOG");
		checked = 1;
		if (e && *e == '/')
			f = fopen(e, "a");
		else if (e)
			f = stderr;
	}
	if (!f)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
}

void
qjs_ReportException(JSContext *cx)
{
	JSValue exc = JS_GetException(cx);
	const char *msg = JS_ToCString(cx, exc);
	const char *stack = NULL;
	JSValue sv = JS_UNDEFINED;

	if (JS_IsError(cx, exc)) {
		sv = JS_GetPropertyStr(cx, exc, "stack");
		if (!JS_IsUndefined(sv))
			stack = JS_ToCString(cx, sv);
	}
	qjs_Log("JavaScript error: %s%s%s", msg ? msg : "?",
			stack ? "\n" : "", stack ? stack : "");
	if (stack)
		JS_FreeCString(cx, stack);
	if (msg)
		JS_FreeCString(cx, msg);
	JS_FreeValue(cx, sv);
	JS_FreeValue(cx, exc);
}

/* ---- the main event queue ------------------------------------------------ */

typedef struct {
	PLEvent		event;
	void		(*fn)(void *);
	void		*arg;
} qjs_LaterEvent;

static void *
qjs_later_handler(PLEvent *e)
{
	qjs_LaterEvent *le = (qjs_LaterEvent *) e;

	le->fn(le->arg);
	return NULL;
}

static void
qjs_later_destructor(PLEvent *e)
{
	XP_FREE(e);
}

void
qjs_Later(void (*fn)(void *), void *arg)
{
	qjs_LaterEvent *le = XP_NEW_ZAP(qjs_LaterEvent);

	if (!le) {
		fn(arg);
		return;
	}
	PL_InitEvent(&le->event, NULL, qjs_later_handler, qjs_later_destructor);
	le->fn = fn;
	le->arg = arg;
	PL_PostEvent(mozilla_event_queue, &le->event);
}

/* ---- text encodings ------------------------------------------------------- */

static int16
qjs_DocCSID(MWContext *context)
{
	INTL_CharSetInfo c = LO_GetDocumentCharacterSetInfo(context);
	int16 csid = c ? INTL_GetCSIWinCSID(c) : CS_DEFAULT;

	return csid ? csid : CS_LATIN1;
}

static char *
qjs_Convert(int16 from, int16 to, const char *text, size_t len, size_t *out_len)
{
	char *copy, *conv;

	copy = (char *) XP_ALLOC(len + 1);
	if (!copy)
		return NULL;
	XP_MEMCPY(copy, text, len);
	copy[len] = '\0';
	if (from == to || (from == CS_ASCII && to == CS_UTF8)) {
		if (out_len)
			*out_len = len;
		return copy;
	}
	conv = (char *) INTL_ConvertLineWithoutAutoDetect(from, to,
										(unsigned char *) copy, len);
	if (conv && conv != copy) {
		XP_FREE(copy);
		copy = conv;
	}
	if (out_len)
		*out_len = XP_STRLEN(copy);
	return copy;
}

char *
qjs_ToDocumentCharset(MWContext *context, const char *utf8, size_t len,
					  size_t *out_len)
{
	return qjs_Convert(CS_UTF8, qjs_DocCSID(context), utf8, len, out_len);
}

char *
qjs_FromDocumentCharset(MWContext *context, const char *text, size_t len)
{
	return qjs_Convert(qjs_DocCSID(context), CS_UTF8, text, len, NULL);
}

/* ---- windows and their contexts ------------------------------------------- */

/* A script that runs this long is stopped (QuickJS's interrupt handler). */
#define QJS_SCRIPT_SECONDS	10

static struct timeval qjs_deadline;
static XP_Bool qjs_running;

static int
qjs_interrupt(JSRuntime *rt, void *opaque)
{
	struct timeval now;

	if (!qjs_running)
		return 0;
	gettimeofday(&now, NULL);
	if (now.tv_sec > qjs_deadline.tv_sec ||
		(now.tv_sec == qjs_deadline.tv_sec && now.tv_usec > qjs_deadline.tv_usec)) {
		qjs_Log("JavaScript: a script ran for more than %d seconds: stopped",
				QJS_SCRIPT_SECONDS);
		return 1;
	}
	return 0;
}

static JSRuntime *
qjs_Runtime(void)
{
	static XP_Bool set;
	JSRuntime *rt = PREF_GetJSRuntime();

	if (rt && !set) {
		JS_SetInterruptHandler(rt, qjs_interrupt, NULL);
		JS_SetModuleLoaderFunc(rt, qjs_ModuleNormalize, qjs_ModuleLoad, NULL);
		set = TRUE;
	}
	return rt;
}

MochaDecoder *
qjs_GetDecoder(MWContext *context, XP_Bool create)
{
	MochaDecoder *decoder;

	if (!context)
		return NULL;
	decoder = (MochaDecoder *) context->mocha_decoder;
	if (decoder || !create)
		return decoder;
	decoder = XP_NEW_ZAP(MochaDecoder);
	if (!decoder)
		return NULL;
	decoder->window_context = context;
	decoder->stream_owner = LO_DOCUMENT_LAYER_ID;
	decoder->active_layer_id = LO_DOCUMENT_LAYER_ID;
	decoder->doc_id = XP_DOCID(context);
	context->mocha_decoder = decoder;
	return decoder;
}

JSContext *
qjs_GetContext(MochaDecoder *decoder)
{
	JSRuntime *rt;
	JSContext *cx;

	if (!decoder)
		return NULL;
	if (decoder->js_context)
		return decoder->js_context;
	rt = qjs_Runtime();
	if (!rt)
		return NULL;
	cx = JS_NewContext(rt);
	if (!cx)
		return NULL;
	JS_SetContextOpaque(cx, decoder);
	decoder->js_context = cx;
	decoder->window_context->mocha_context = cx;
	qjs_InitWindow(cx, decoder);
	return cx;
}

extern void qjs_ClearTimeouts(MochaDecoder *decoder);

/* A context dropped while its scripts ran (a script whose document went
 * from under it): freed once they have returned. */
typedef struct {
	JSContext	*cx;
	void		*dom;
} qjs_DeadContext;

static void
qjs_free_dead(void *arg)
{
	qjs_DeadContext *dead = (qjs_DeadContext *) arg;

	if (qjs_running) {
		qjs_Later(qjs_free_dead, dead);
		return;
	}
	JS_FreeContext(dead->cx);
	JS_RunGC(qjs_Runtime());
	qjs_DomFreeDetached(dead->dom);
	XP_FREE(dead);
}

void
qjs_DropContext(MochaDecoder *decoder)
{
	JSContext *cx;

	if (!decoder)
		return;
	if ((cx = decoder->js_context) != NULL) {
		qjs_ClearTimeouts(decoder);
		qjs_DropLoads(decoder);
		/* the DOM's node objects first: they hold the tree's nodes */
		qjs_DomDropObjects(decoder);
		decoder->js_context = NULL;
		if (decoder->window_context)
			decoder->window_context->mocha_context = NULL;
		if (qjs_running) {
			qjs_DeadContext *dead = XP_NEW_ZAP(qjs_DeadContext);
			qjs_Log("JavaScript: the document went while a script ran");
			/* its natives see no window from now on */
			JS_SetContextOpaque(cx, NULL);
			if (dead) {
				dead->cx = cx;
				dead->dom = qjs_DomDetach(decoder);
				qjs_Later(qjs_free_dead, dead);
			}
			return;
		}
		JS_FreeContext(cx);
		JS_RunGC(qjs_Runtime());
	}
	qjs_DomFree(decoder);
}

static void
qjs_run_jobs(void)
{
	JSContext *jcx;

	while (JS_ExecutePendingJob(qjs_Runtime(), &jcx) > 0)
		;
}

JSValue
qjs_Call(JSContext *cx, JSValueConst fn, JSValueConst this_val, int argc,
		 JSValueConst *argv)
{
	XP_Bool nested = qjs_running;
	JSValue rv;

	if (!nested) {
		gettimeofday(&qjs_deadline, NULL);
		qjs_deadline.tv_sec += QJS_SCRIPT_SECONDS;
		qjs_running = TRUE;
	}
	qjs_DomSyncNames(cx);
	rv = JS_Call(cx, fn, this_val, argc, (JSValue *) argv);
	if (JS_IsException(rv))
		qjs_ReportException(cx);
	if (!nested) {
		qjs_run_jobs();
		qjs_running = FALSE;
	}
	return rv;
}

XP_Bool
qjs_Evaluate(MochaDecoder *decoder, const char *src, size_t len,
			 const char *filename, int lineno, char **result)
{
	JSContext *cx = qjs_GetContext(decoder);
	JSValue v;
	XP_Bool ok = TRUE, nested = qjs_running;

	if (result)
		*result = NULL;
	if (!cx)
		return FALSE;
	if (!nested) {
		gettimeofday(&qjs_deadline, NULL);
		qjs_deadline.tv_sec += QJS_SCRIPT_SECONDS;
		qjs_running = TRUE;
	}
	qjs_DomSyncNames(cx);
	v = JS_Eval(cx, src, len, filename ? filename : "<script>",
				JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(v)) {
		qjs_ReportException(cx);
		ok = FALSE;
	} else if (result && !JS_IsUndefined(v)) {
		const char *s = JS_ToCString(cx, v);
		if (s) {
			*result = XP_STRDUP(s);
			JS_FreeCString(cx, s);
		}
	}
	JS_FreeValue(cx, v);
	/* promise jobs (async functions, then()) */
	if (!nested) {
		qjs_run_jobs();
		qjs_running = FALSE;
	}
	return ok;
}

/* ---- enabled? ---------------------------------------------------------------- */

static int PR_CALLBACK
lm_enabled_changed(const char *pref, void *data)
{
	XP_Bool b = TRUE;

	PREF_GetBoolPref(lm_pref_enabled, &b);
	lm_enabled = b;
	return PREF_NOERROR;
}

void
LM_InitMocha(void)
{
	XP_Bool b = TRUE;

	if (PREF_GetBoolPref(lm_pref_enabled, &b) == PREF_NOERROR)
		lm_enabled = b;
	PREF_RegisterCallback(lm_pref_enabled, lm_enabled_changed, NULL);
	/* scripts by URL (SCRIPT SRC=) come through netlib's mocha converter */
	NET_RegisterContentTypeConverter(js_content_type, FO_PRESENT, 0,
									 NET_CreateMochaConverter);
	NET_RegisterContentTypeConverter("text/javascript", FO_PRESENT, 0,
									 NET_CreateMochaConverter);
	NET_RegisterContentTypeConverter("application/javascript", FO_PRESENT, 0,
									 NET_CreateMochaConverter);
}

void
LM_FinishMocha(void)
{
}

JSBool
LM_GetMochaEnabled(void)
{
	return lm_enabled;
}

void
LM_ForceJSEnabled(MWContext *cx)
{
	if (cx)
		cx->forceJSEnabled = PR_TRUE;
}

JSBool
LM_CanDoJS(MWContext *context)
{
	if (!context)
		return FALSE;
	if (!context->forceJSEnabled && (!lm_enabled || EDT_IS_EDITOR(context)))
		return FALSE;
	switch (context->type) {
	case MWContextBrowser:
	case MWContextDialog:
	case MWContextPane:
		return qjs_Runtime() != NULL;
	default:
		return FALSE;
	}
}

JSBool
LM_IsActive(MWContext *context)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, FALSE);

	return decoder && decoder->js_context != NULL;
}

/* ---- the JS lock: one thread, nothing to lock ------------------------------ */

void LM_LockJS(void) { }
void LM_UnlockJS(void) { }

JSBool
LM_AttemptLockJS(JSLockReleaseFunc fn, void *data)
{
	return TRUE;
}

JSBool
LM_ClearAttemptLockJS(JSLockReleaseFunc fn, void *data)
{
	return FALSE;
}

PRBool
LM_HandOffJSLock(PRThread *oldOwner, PRThread *newOwner)
{
	return PR_TRUE;
}

void LM_JSLockSetContext(MWContext *context) { }
MWContext *LM_JSLockGetContext(void) { return NULL; }

/* ---- decoders ------------------------------------------------------------- */

MochaDecoder *
LM_GetMochaDecoder(MWContext *context)
{
	MochaDecoder *decoder;

	if (!LM_CanDoJS(context))
		return NULL;
	decoder = qjs_GetDecoder(context, TRUE);
	if (decoder)
		decoder->forw_count++;
	return decoder;
}

void
LM_PutMochaDecoder(MochaDecoder *decoder)
{
	if (decoder && decoder->forw_count > 0)
		decoder->forw_count--;
}

const char *
LM_GetSourceURL(MochaDecoder *decoder)
{
	if (!decoder)
		return NULL;
	if (decoder->source_url)
		return decoder->source_url;
	return decoder->url_struct ? decoder->url_struct->address : NULL;
}

void
LM_SetActiveLayer(MWContext *context, int32 layer_id)
{
}

int32
LM_GetActiveLayer(MWContext *context)
{
	return LO_DOCUMENT_LAYER_ID;
}

/* ---- streams: document.write's destination -------------------------------- */

JSBool
LM_SetDecoderStream(MWContext *context, NET_StreamClass *stream,
					URL_Struct *url_struct, JSBool free_stream_on_close)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, TRUE);

	if (!decoder)
		return FALSE;
	decoder->stream = stream;
	decoder->free_stream_on_close = (PRPackedBool) free_stream_on_close;
	decoder->stream_owner = LO_DOCUMENT_LAYER_ID;
	if (url_struct) {
		NET_HoldURLStruct(url_struct);
		if (decoder->url_struct)
			NET_DropURLStruct(decoder->url_struct);
		decoder->url_struct = url_struct;
	}
	return TRUE;
}

void
ET_SetDecoderStream(MWContext *context, NET_StreamClass *stream,
					URL_Struct *url_struct, JSBool free_stream_on_close)
{
	LM_SetDecoderStream(context, stream, url_struct, free_stream_on_close);
}

void
ET_ClearDecoderStream(MWContext *context, NET_StreamClass *old_stream)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, FALSE);

	if (decoder && decoder->stream == old_stream) {
		decoder->stream = NULL;
		decoder->free_stream_on_close = FALSE;
	}
}

void
ET_DocWriteAck(MWContext *context, int status)
{
}

/* ---- script evaluation ----------------------------------------------------- */

typedef struct {
	MWContext		*context;
	int32			doc_id;
	char			*src;		/* UTF-8 */
	size_t			len;
	ETEvalStuff		*stuff;
	ETEvalAckFunc	fn;
} qjs_EvalRequest;

static void
qjs_eval_later(void *arg)
{
	qjs_EvalRequest *r = (qjs_EvalRequest *) arg;
	MochaDecoder *decoder;
	char *result = NULL;
	XP_Bool ok = FALSE;

	if (r->doc_id == XP_DOCID(r->context) &&
		(decoder = qjs_GetDecoder(r->context, TRUE)) != NULL) {
		decoder->doc_id = r->doc_id;
		ok = qjs_Evaluate(decoder, r->src, r->len, LM_GetSourceURL(decoder),
						  r->stuff->line_no,
						  r->stuff->want_result ? &result : NULL);
	}
	if (r->doc_id == XP_DOCID(r->context))
		r->fn(r->stuff->data, result, result ? XP_STRLEN(result) : 0,
			  NULL, NULL, ok);
	else
		XP_FREEIF(result);
	XP_FREEIF(r->stuff->scope_to);
	XP_FREE(r->stuff);
	XP_FREE(r->src);
	XP_FREE(r);
}

void
ET_EvaluateScript(MWContext *context, char *buffer, ETEvalStuff *stuff,
				  ETEvalAckFunc fn)
{
	qjs_EvalRequest *r;

	if (!LM_CanDoJS(context) ||
		(r = XP_NEW_ZAP(qjs_EvalRequest)) == NULL) {
		fn(stuff->data, NULL, 0, NULL, NULL, FALSE);
		XP_FREEIF(stuff->scope_to);
		XP_FREE(stuff);
		return;
	}
	r->context = context;
	r->doc_id = XP_DOCID(context);
	r->src = qjs_FromDocumentCharset(context, buffer, stuff->len);
	r->len = r->src ? XP_STRLEN(r->src) : 0;
	r->stuff = stuff;
	r->fn = fn;
	if (!r->src) {
		fn(stuff->data, NULL, 0, NULL, NULL, FALSE);
		XP_FREE(stuff);
		XP_FREE(r);
		return;
	}
	qjs_Later(qjs_eval_later, r);
}

void
ET_EvaluateBuffer(MWContext *context, char *buffer, uint buflen,
				  uint line_no, char *scope_to, JSBool want_result,
				  ETEvalAckFunc fn, void *data, JSVersion ver,
				  struct JSPrincipals *principals)
{
	ETEvalStuff *stuff = XP_NEW_ZAP(ETEvalStuff);

	if (!stuff) {
		fn(data, NULL, 0, NULL, NULL, FALSE);
		return;
	}
	stuff->len = buflen;
	stuff->line_no = line_no;
	stuff->data = data;
	stuff->want_result = want_result;
	ET_EvaluateScript(context, buffer, stuff, fn);
}

JSBool
LM_EvaluateBuffer(MochaDecoder *decoder, void *base, size_t length,
				  uint lineno, char *scope_to, struct JSPrincipals *principals,
				  JSBool unicode, jsval *result)
{
	char *src;
	XP_Bool ok;

	if (result)
		*result = 0;
	src = qjs_FromDocumentCharset(decoder->window_context, (char *) base, length);
	if (!src)
		return FALSE;
	ok = qjs_Evaluate(decoder, src, XP_STRLEN(src), LM_GetSourceURL(decoder),
					  lineno, NULL);
	XP_FREE(src);
	return ok;
}

/* A JavaScript entity or attribute value (&{...};): run it now, and give
 * back its value as a string in the document's encoding, or NULL. */
char *
LM_EvaluateAttribute(MWContext *context, char *expr, uint lineno)
{
	MochaDecoder *decoder;
	char *src, *result = NULL, *doc = NULL;

	if (!LM_CanDoJS(context) || !expr)
		return NULL;
	decoder = qjs_GetDecoder(context, TRUE);
	src = qjs_FromDocumentCharset(context, expr, XP_STRLEN(expr));
	if (!decoder || !src) {
		XP_FREEIF(src);
		return NULL;
	}
	qjs_Evaluate(decoder, src, XP_STRLEN(src), LM_GetSourceURL(decoder),
				 lineno, &result);
	XP_FREE(src);
	if (result) {
		doc = qjs_ToDocumentCharset(context, result, XP_STRLEN(result), NULL);
		XP_FREE(result);
	}
	return doc;
}

/* ---- SCRIPT SRC=: nesting URLs and the converter's stream -------------------- */

void
ET_SetNestingUrl(MWContext *context, char *url)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, TRUE);
	JSNestingUrl *nu;

	if (!decoder)
		return;
	if (!url) {
		/* the fetch failed: forget the innermost one */
		nu = decoder->nesting_url;
		if (nu) {
			decoder->nesting_url = nu->next;
			XP_FREE(nu->str);
			XP_FREE(nu);
		}
		return;
	}
	nu = XP_NEW_ZAP(JSNestingUrl);
	if (!nu)
		return;
	nu->str = XP_STRDUP(url);
	nu->next = decoder->nesting_url;
	decoder->nesting_url = nu;
}

void
ET_SetVersion(MWContext *context, JSVersion version)
{
}

typedef struct {
	MWContext	*context;
	int32		doc_id;
	char		*src;
	size_t		len;
	char		*url;
} qjs_StreamScript;

static void
qjs_stream_later(void *arg)
{
	qjs_StreamScript *s = (qjs_StreamScript *) arg;
	MochaDecoder *decoder = qjs_GetDecoder(s->context, FALSE);
	JSNestingUrl *nu;

	if (decoder && s->doc_id == XP_DOCID(s->context)) {
		qjs_Evaluate(decoder, s->src, s->len, s->url, 1, NULL);
		nu = decoder->nesting_url;
		if (nu) {
			/* layout waits on this SCRIPT SRC= */
			decoder->nesting_url = nu->next;
			XP_FREE(nu->str);
			XP_FREE(nu);
			lo_ScriptEvalExitFn(s->context, NULL, 0, NULL, NULL, FALSE);
		}
	}
	XP_FREEIF(s->url);
	XP_FREE(s->src);
	XP_FREE(s);
}

/* A script fetched by URL arrived (network/protocol/js/mkmocha.c). */
void
ET_MochaStreamComplete(MWContext *context, void *buf, int len,
					   char *content_type, Bool isUnicode)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, TRUE);
	qjs_StreamScript *s;

	if (!decoder) {
		XP_FREE(buf);
		return;
	}
	s = XP_NEW_ZAP(qjs_StreamScript);
	if (!s) {
		XP_FREE(buf);
		return;
	}
	s->context = context;
	s->doc_id = XP_DOCID(context);
	/* scripts by URL: UTF-8 unless they say otherwise (mkmocha converts
	 * those it knows to the document's charset) */
	if (content_type && !strcasecomp(content_type, TEXT_CSS)) {
		XP_FREE(buf);
		XP_FREE(s);
		return;
	}
	s->src = (char *) XP_ALLOC(len + 1);
	if (s->src) {
		XP_MEMCPY(s->src, buf, len);
		s->src[len] = '\0';
		s->len = len;
	}
	XP_FREE(buf);
	if (decoder->nesting_url && decoder->nesting_url->str)
		s->url = XP_STRDUP(decoder->nesting_url->str);
	if (!s->src) {
		XP_FREE(s);
		return;
	}
	qjs_Later(qjs_stream_later, s);
}

void
ET_MochaStreamAbort(MWContext *context, int status)
{
}

NET_StreamClass *
LM_StreamBuilder(int format_out, void *data_obj, URL_Struct *URL_s,
				 MWContext *mwcontext)
{
	return NULL;
}

/* ---- documents and windows going away -------------------------------------- */

void
LM_ReleaseDocument(MWContext *context, JSBool resize_reload)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, FALSE);

	if (!decoder)
		return;
	if (getenv("QJS_DOM_TRACE"))
		qjs_Log("dom: release document (resize %d)", (int) resize_reload);
	/* A resize reload lays the same document out again without running its
	 * scripts: keep what they made. */
	if (!resize_reload)
		qjs_DropContext(decoder);
	else
		qjs_DomResizeReload(decoder);
	decoder->stream = NULL;
	if (!resize_reload)
		decoder->load_event_sent = FALSE;
	while (decoder->nesting_url) {
		JSNestingUrl *nu = decoder->nesting_url;
		decoder->nesting_url = nu->next;
		XP_FREE(nu->str);
		XP_FREE(nu);
	}
}

void
ET_ReleaseDocument(MWContext *context, JSBool resize_reload)
{
	LM_ReleaseDocument(context, resize_reload);
}

void
LM_RemoveWindowContext(MWContext *context, History_entry *he)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, FALSE);

	if (!decoder)
		return;
	qjs_DropContext(decoder);
	if (decoder->url_struct)
		NET_DropURLStruct(decoder->url_struct);
	LM_ReleaseDocument(context, FALSE);
	context->mocha_decoder = NULL;
	XP_FREE(decoder);
}

void
ET_RemoveWindowContext(MWContext *context, ETVoidPtrFunc fn, void *data)
{
	LM_RemoveWindowContext(context, NULL);
	if (fn)
		fn(data);
}

void
LM_DropSavedWindow(MWContext *context, void *window)
{
}

/* ---- events -------------------------------------------------------------- */

extern JSBool qjs_SendEvent(MWContext *context, LO_Element *element,
							JSEvent *event);

JSBool
ET_SendEvent(MWContext *context, LO_Element *element, JSEvent *event,
			 ETClosureFunc closure, void *whatever)
{
	ETEventStatus status = EVENT_OK;

	if (LM_CanDoJS(context) && !EDT_IS_EDITOR(context)) {
		if (!qjs_SendEvent(context, element, event))
			status = EVENT_CANCEL;
	} else if (event->type == EVENT_MOUSEOVER) {
		status = EVENT_CANCEL;
	}
	if (closure)
		closure(context, element, event->type, whatever, status);
	XP_FREE(event);
	return TRUE;
}

extern void qjs_SendLoadEvent(MWContext *context, int32 type,
							  JSBool resize_reload);

typedef struct {
	MWContext		*context;
	int32			doc_id;
	int32			type;
	ETVoidPtrFunc	closure;
	void			*data;
	JSBool			resize_reload;
} qjs_LoadEvent;

static void
qjs_load_later(void *arg)
{
	qjs_LoadEvent *l = (qjs_LoadEvent *) arg;

	if (l->doc_id == XP_DOCID(l->context) || l->type == EVENT_UNLOAD)
		qjs_SendLoadEvent(l->context, l->type, l->resize_reload);
	if (l->closure)
		l->closure(l->data);
	XP_FREE(l);
}

void
ET_SendLoadEvent(MWContext *context, int32 type, ETVoidPtrFunc closure,
				 NET_StreamClass *stream, int32 layer_id, Bool resize_reload)
{
	qjs_LoadEvent *l;
	MochaDecoder *decoder = qjs_GetDecoder(context, FALSE);


	if (decoder && type == EVENT_LOAD && layer_id == LO_DOCUMENT_LAYER_ID) {
		/* the document is all laid out: document.write would start a
		 * new one now */
		decoder->stream = NULL;
	}
	if (!LM_CanDoJS(context) || layer_id != LO_DOCUMENT_LAYER_ID ||
		(l = XP_NEW_ZAP(qjs_LoadEvent)) == NULL) {
		if (closure)
			closure(stream);
		return;
	}
	l->context = context;
	l->doc_id = XP_DOCID(context);
	l->type = type;
	l->closure = closure;
	l->data = stream;
	l->resize_reload = (JSBool) resize_reload;
	qjs_Later(qjs_load_later, l);
}

void
ET_SendImageEvent(MWContext *context, LO_ImageStruct *image_data,
				  LM_ImageEvent event)
{
}

void
LM_ProcessImageEvent(MWContext *context, LO_ImageStruct *image_data,
					 LM_ImageEvent event)
{
}

XP_Bool LM_EventCaptureCheck(MWContext *context, uint32 current_event) { return FALSE; }
void LM_SendOnScroll(MWContext *context, int32 x, int32 y) { }
void LM_SendOnHelp(MWContext *context) { }
JSBool LM_SendOnLocate(MWContext *context, struct lo_NameList_struct *name_rec) { return FALSE; }

void ET_InterruptContext(MWContext *context) { }
JSBool ET_ContinueProcessing(MWContext *context) { return TRUE; }
void ET_FinishMocha(void) { }

/* ---- reflection: the window's handlers and the page's objects --------------- */

extern void qjs_SetWindowHandlers(MWContext *context, char *onload,
								  char *onunload);

void
ET_ReflectWindow(MWContext *context, PA_Block onLoad, PA_Block onUnload,
				 PA_Block onFocus, PA_Block onBlur, PA_Block onHelp,
				 PA_Block onMouseOver, PA_Block onMouseOut,
				 PA_Block onDragDrop, PA_Block onMove, PA_Block onResize,
				 PA_Block id, char *all, Bool bDelete, int newline_count)
{
	if (LM_CanDoJS(context))
		qjs_SetWindowHandlers(context, (char *) onLoad, (char *) onUnload);
	XP_FREEIF(onLoad);
	XP_FREEIF(onUnload);
	XP_FREEIF(onFocus);
	XP_FREEIF(onBlur);
	XP_FREEIF(onHelp);
	XP_FREEIF(onMouseOver);
	XP_FREEIF(onMouseOut);
	XP_FREEIF(onDragDrop);
	XP_FREEIF(onMove);
	XP_FREEIF(onResize);
	XP_FREEIF(id);
	XP_FREEIF(all);
}

/* ET_ReflectObject, ET_ReflectFormElement: qjs_dom.c */

void ET_SetActiveForm(MWContext *context, struct lo_FormData_struct *form) { }
void ET_SetActiveLayer(MWContext *context, int32 layer_id) { }

JSObject *LM_ReflectApplet(MWContext *c, LO_JavaAppStruct *a, PA_Tag *t, int32 l, uint i) { return NULL; }
JSObject *LM_ReflectEmbed(MWContext *c, LO_EmbedStruct *e, PA_Tag *t, int32 l, uint i) { return NULL; }
JSObject *LM_ReflectForm(MWContext *c, struct lo_FormData_struct *f, PA_Tag *t, int32 l, uint i) { return NULL; }
JSObject *LM_ReflectFormElement(MWContext *c, int32 l, int32 f, int32 e, PA_Tag *t) { return NULL; }
JSObject *LM_ReflectLink(MWContext *c, LO_AnchorData *a, PA_Tag *t, int32 l, uint i) { return NULL; }
JSObject *LM_ReflectNamedAnchor(MWContext *c, struct lo_NameList_struct *n, PA_Tag *t, int32 l, uint i) { return NULL; }
JSObject *LM_ReflectImage(MWContext *c, LO_ImageStruct *im, PA_Tag *t, int32 l, uint i) { return NULL; }
JSObject *LM_ReflectLayer(MWContext *c, int32 l, int32 p, PA_Tag *t) { return NULL; }
JSObject *LM_ReflectSpan(MWContext *c, struct lo_NameList_struct *n, PA_Tag *t, int32 l, uint i) { return NULL; }
JSObject *LM_ReflectTransclusion(MWContext *c, void *e, int32 l, uint i) { return NULL; }

/* ---- what the 1998 engine had and this one does not ------------------------ */

void ET_DestroyLayer(MWContext *context, JSObject *layer_obj) { }
void ET_SetPluginWindow(MWContext *context, void *app) { }
void ET_StartSoftUpdate(MWContext *context, char *codebase) { }

void
ET_RestoreLayerState(MWContext *context, int32 layer_id,
					 LO_BlockInitializeStruct *param, ETRestoreAckFunc fn,
					 void *data)
{
	if (fn)
		fn(data, param);
}

JSBool
ET_PostMessageBox(MWContext *context, char *szMessage, JSBool bConfirm)
{
	if (bConfirm)
		return FE_Confirm(context, szMessage);
	FE_Alert(context, szMessage);
	return TRUE;
}

JSPrincipals *
LM_NewJSPrincipals(URL_Struct *archive, char *name, const char *codebase)
{
	return NULL;
}

char *
LM_ExtractFromPrincipalsArchive(JSPrincipals *principals, char *name,
								uint *length)
{
	return NULL;
}

JSBool
LM_SetUntransformedSource(JSPrincipals *principals, char *original,
						  char *transformed)
{
	return TRUE;
}

JSBool LM_GetJSDebugActive(void) { return FALSE; }
void LM_JamSourceIntoJSDebug(const char *f, const char *s, int32 l, MWContext *c) { }

/* "wysiwyg:" URLs held document.write output for the 1998 engine's
 * history; there is none here. */
NET_StreamClass *
LM_WysiwygCacheConverter(MWContext *context, URL_Struct *url_struct,
						 const char *wysiwyg_url, const char *base_href)
{
	return NULL;
}

#define WYSIWYG_TYPE_LEN	10		/* wysiwyg:// */

const char *
LM_SkipWysiwygURLPrefix(const char *url_string)
{
	if (XP_STRLEN(url_string) < WYSIWYG_TYPE_LEN)
		return NULL;
	url_string += WYSIWYG_TYPE_LEN;
	url_string = XP_STRCHR(url_string, '/');
	if (!url_string)
		return NULL;
	return url_string + 1;
}

const char *
LM_StripWysiwygURLPrefix(const char *url_string)
{
	if (NET_URL_Type(url_string) == WYSIWYG_TYPE_URL)
		return LM_SkipWysiwygURLPrefix(url_string);
	return url_string;
}
