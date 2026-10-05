/* -*- Mode: C; tab-width: 4 -*-
 *   qjs.h --- libmocha on QuickJS (NS_QUICKJS): private declarations.
 *
 * JavaScript runs on the main (mozilla) thread, on the QuickJS runtime the
 * preferences module made (PREF_GetJSRuntime).  Each browser window has a
 * MochaDecoder (libmocha.h) holding its QuickJS context: decoder->js_context
 * is the QuickJS JSContext, and the window's MWContext points to it too
 * (mocha_context).  The context is the window's global scope; it is thrown
 * away when the window's document goes (LM_ReleaseDocument) and made again
 * for the next one.
 */

#ifndef QJS_H
#define QJS_H

#include <quickjs/quickjs.h>	/* before libmocha.h: see nsjscompat.h */

#include "xp.h"
#include "net.h"
#include "structs.h"
#include "libmocha.h"
#include "libevent.h"

/* SCRIPT SRC= URLs being fetched (innermost first): layout waits on each. */
struct JSNestingUrl {
	struct JSNestingUrl	*next;
	char				*str;
};

/* The window's decoder, made on first use; or NULL. */
extern MochaDecoder *qjs_GetDecoder(MWContext *context, XP_Bool create);

/* The QuickJS context of a decoder, made on first use. */
extern JSContext *qjs_GetContext(MochaDecoder *decoder);

/* Throw away the window's scripts and globals (the document went). */
extern void qjs_DropContext(MochaDecoder *decoder);

/* Define window, document, navigator, location, timers, alert, console
 * in a new context (qjs_window.c). */
extern void qjs_InitWindow(JSContext *cx, MochaDecoder *decoder);

/* Run SRC (UTF-8, NUL-terminated) in the window's context.  Errors are
 * logged, never shown.  *RESULT, when not NULL, gets the completion value
 * as a string (the caller frees it), or NULL. */
extern XP_Bool qjs_Evaluate(MochaDecoder *decoder, const char *src, size_t len,
							const char *filename, int lineno, char **result);

/* Call FN with a time budget (as a script gets), then run promise jobs.
 * An exception is logged; the result (maybe JS_EXCEPTION) is the caller's. */
extern JSValue qjs_Call(JSContext *cx, JSValueConst fn, JSValueConst this_val,
						int argc, JSValueConst *argv);

/* The DOM (qjs_dom.c) and fetches for scripts (qjs_net.c). */
extern void qjs_InitDom(JSContext *cx, JSValueConst ns);
extern void qjs_DomDropObjects(MochaDecoder *decoder);
extern void qjs_DomFree(MochaDecoder *decoder);
extern void qjs_DomResizeReload(MochaDecoder *decoder);
extern JSBool qjs_DomEvent(MWContext *context, LO_Element *element,
						   JSEvent *event);
extern void qjs_DomSyncNames(JSContext *cx);
extern void qjs_DropLoads(MochaDecoder *decoder);
extern JSValue qjs_ns_load(JSContext *cx, JSValueConst this_val, int argc,
						   JSValueConst *argv);

/* Log the pending exception of CX (NETSCAPE_JS_LOG=/file, or stderr). */
extern void qjs_ReportException(JSContext *cx);
extern void qjs_Log(const char *fmt, ...);

/* Call FN(ARG) later, from the main event queue. */
extern void qjs_Later(void (*fn)(void *), void *arg);

/* The document's text encoding, as a document.write must give it to the
 * parser: convert UTF-8 from JavaScript (the caller frees the result). */
extern char *qjs_ToDocumentCharset(MWContext *context, const char *utf8,
								   size_t len, size_t *out_len);
extern char *qjs_FromDocumentCharset(MWContext *context, const char *text,
									 size_t len);

#endif /* QJS_H */
