/* -*- Mode: C; tab-width: 4 -*-
 *   nsjscompat.h --- the old JavaScript engine's names, for NS_QUICKJS.
 *
 * With NS_QUICKJS the 1998 engine (js/) is not built: QuickJS runs
 * JavaScript (lib/libmocha).  The rest of the browser still mentions a few
 * of the old engine's types in passing: handles it keeps for libmocha
 * (struct JSObject *, struct JSContext *), flags (JSBool) and versions.
 * These keep their meaning here as opaque or plain types.
 *
 * libmocha includes <quickjs/quickjs.h> first: then JSContext and JSRuntime
 * are QuickJS's, and JS_TRUE/JS_FALSE are QuickJS values, not defined here.
 */

#ifndef nsjscompat_h___
#define nsjscompat_h___

#include "prtypes.h"

typedef int JSBool;
typedef int JSVersion;
typedef PRUint64 jsval;			/* QuickJS's JSValue on n32 (NaN boxing) */

typedef struct JSObject JSObject;	/* a QuickJS object, held by libmocha */
typedef struct JSPrincipals JSPrincipals;

#ifndef QUICKJS_H
typedef struct JSContext JSContext;
typedef struct JSRuntime JSRuntime;
#define JS_TRUE		1
#define JS_FALSE	0
#endif

#define JSVERSION_1_0		100
#define JSVERSION_1_1		110
#define JSVERSION_1_2		120
#define JSVERSION_1_3		130
#define JSVERSION_DEFAULT	0
#define JSVERSION_UNKNOWN	(-1)

#endif /* nsjscompat_h___ */
