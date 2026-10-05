/* -*- Mode: C; tab-width: 4 -*-
 *   qjs_dom.c --- libmocha on QuickJS: the W3C DOM, on libdom.
 *
 * The document tree is built from the tags layout lays out (LM_DomTag is
 * called just before each one), so a script sees the document as far as
 * it has been laid out, as in any browser, and what document.write adds
 * comes in the same way.  Each start tag gets its element (tag->dom_node),
 * and layout hands that on to what it makes of the tag: links, images,
 * forms and form elements keep it in their mocha_object, which is how
 * clicks and submits on them find their element (qjs_ElementOf).
 *
 * JavaScript sees nodes through one class (qjs_node_class) whose objects
 * hold a reference to their dom_node.  A node has one object for the
 * document's life (the wrapper table here), so expando properties and
 * event listeners stick.  The prototypes (Node, Element, HTMLElement and
 * the HTML element classes) are made in JavaScript (qjs_dom.js) and given
 * to C (dom.protos), which picks one by node type and tag name.
 *
 * C provides what JavaScript cannot do itself: the tree, attributes and
 * text (libdom), CSS selectors (querySelector, matches), HTML serialisation
 * (innerHTML, outerHTML) and HTML fragment parsing (hubbub through libdom's
 * binding).
 */

#include "qjs.h"
#include "pa_parse.h"
#include "pa_tags.h"
#include "lo_ele.h"
#include "layout.h"
#include "prprf.h"
#include "shist.h"
#include "fe_proto.h"
#include <sys/time.h>

#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>

#include <string.h>
#include <ctype.h>

#define QJS_DOM_DEPTH	256

typedef struct qjs_NodeRef {
	dom_node		*node;		/* NULL once the document is gone */
} qjs_NodeRef;

typedef struct {
	dom_node		*node;
	JSValue			obj;
	qjs_NodeRef		*ref;
} qjs_Wrap;

typedef struct {
	dom_node		*node;
	char			name[24];	/* lower case, maybe cut short */
} qjs_Open;

typedef struct qjs_Dom {
	dom_document	*doc;
	int32			doc_id;
	dom_node		*html, *head, *body;	/* in the tree: not held */
	qjs_Open		open[QJS_DOM_DEPTH];
	int				depth;
	dom_node		*current_script;
	/* a resize reload lays the document out again: find each tag's
	 * element again, in document order, instead of making it */
	XP_Bool			resize_pending, remap;
	dom_node		*cursor;
	/* JavaScript's objects for nodes */
	JSContext		*cx;
	JSValue			protos;
	qjs_Wrap		*wraps;
	uint32			nwraps, wcap;
	/* nodes layout keeps (in mocha_object) */
	dom_node		**held;
	uint32			nheld, hcap;
	XP_Bool			mutated;
	/* named forms, images and form controls not yet given to JavaScript
	 * (document.NAME, form.NAME: __ns_names in qjs_dom.js) */
	dom_node		**named;
	uint32			nnamed, ncap;
	/* showing what scripts changed: the document laid out again from the
	 * tree (qjs_dom_render) */
	void			*render_timer;
	int32			renders;
	int64			last_render;	/* ms */
	XP_Bool			user_event;		/* a click or key since the last one */
	XP_Bool			in_render;		/* our layout is under way */
	XP_Bool			module_scripts;	/* the page has <script type=module> */
	/* form fields of the current layout, by element (value, checked ...
	 * live in layout's form elements): emptied when layout starts over */
	struct { dom_node *node; LO_FormElementStruct *fe; } *fields;
	uint32			nfields, fcap;
	MWContext		*context;
} qjs_Dom;

static JSClassID qjs_node_class;

static void qjs_dom_free_wrappers(qjs_Dom *dom);
static JSValue qjs_wrap(JSContext *cx, dom_node *node);
static LO_FormElementStruct *qjs_field_of(qjs_Dom *dom, dom_node *node);

/* ---- strings ------------------------------------------------------------ */

static dom_string *
qjs_dstr(const char *s, size_t len)
{
	dom_string *d = NULL;

	if (dom_string_create((const uint8_t *) s, len, &d) != DOM_NO_ERR)
		return NULL;
	return d;
}

static dom_string *
qjs_dstr_lower(const char *s, size_t len)
{
	char buf[64], *p = len < sizeof buf ? buf : (char *) XP_ALLOC(len + 1);
	dom_string *d;
	size_t i;

	if (!p)
		return NULL;
	for (i = 0; i < len; i++)
		p[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
	d = qjs_dstr(p, len);
	if (p != buf)
		XP_FREE(p);
	return d;
}

static JSValue
qjs_jsstr(JSContext *cx, dom_string *s)
{
	if (!s)
		return JS_NULL;
	return JS_NewStringLen(cx, dom_string_data(s), dom_string_byte_length(s));
}

/* A JS argument as a dom_string (NULL on failure; the caller unrefs). */
static dom_string *
qjs_arg_dstr(JSContext *cx, JSValueConst v, XP_Bool lower)
{
	size_t len;
	const char *s = JS_ToCStringLen(cx, &len, v);
	dom_string *d;

	if (!s)
		return NULL;
	d = lower ? qjs_dstr_lower(s, len) : qjs_dstr(s, len);
	JS_FreeCString(cx, s);
	return d;
}

/* ---- the document ---------------------------------------------------------- */

static void
qjs_copy(char *dst, const char *src, size_t size)
{
	size_t i;

	for (i = 0; i + 1 < size && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static qjs_Dom *
qjs_dom_of(MochaDecoder *decoder)
{
	return decoder ? (qjs_Dom *) decoder->qjs_dom : NULL;
}

static qjs_Dom *
qjs_dom_new(MochaDecoder *decoder)
{
	qjs_Dom *dom = XP_NEW_ZAP(qjs_Dom);

	if (!dom)
		return NULL;
	if (dom_implementation_create_document(DOM_IMPLEMENTATION_HTML, NULL,
			NULL, NULL, NULL, NULL, &dom->doc) != DOM_NO_ERR) {
		XP_FREE(dom);
		return NULL;
	}
	dom->doc_id = decoder->doc_id;
	dom->protos = JS_UNDEFINED;
	decoder->qjs_dom = dom;
	return dom;
}

/* Called with the JavaScript context still there: let go of the node
 * objects (qjs_DropContext calls this, then frees the context, then
 * qjs_DomFree). */
void
qjs_DomDropObjects(MochaDecoder *decoder)
{
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (dom)
		qjs_dom_free_wrappers(dom);
}

static void qjs_dom_free(qjs_Dom *dom);

void
qjs_DomFree(MochaDecoder *decoder)
{
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (!dom)
		return;
	decoder->qjs_dom = NULL;
	qjs_dom_free(dom);
}

/* Take the tree from the window (its context is going, later). */
void *
qjs_DomDetach(MochaDecoder *decoder)
{
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (dom)
		decoder->qjs_dom = NULL;
	return dom;
}

void
qjs_DomFreeDetached(void *dom)
{
	if (dom)
		qjs_dom_free((qjs_Dom *) dom);
}

static void
qjs_dom_free(qjs_Dom *dom)
{
	uint32 i;

	if (dom->render_timer)
		FE_ClearTimeout(dom->render_timer);
	qjs_dom_free_wrappers(dom);
	for (i = 0; i < dom->nheld; i++)
		dom_node_unref(dom->held[i]);
	XP_FREEIF(dom->held);
	for (i = 0; i < dom->nnamed; i++)
		dom_node_unref(dom->named[i]);
	XP_FREEIF(dom->named);
	XP_FREEIF(dom->fields);
	if (dom->cursor)
		dom_node_unref(dom->cursor);
	if (dom->current_script)
		dom_node_unref(dom->current_script);
	dom_node_unref(dom->doc);
	XP_FREE(dom);
}

/* The document is being laid out again for a new size: keep the tree and
 * its objects, and find each tag's element again. */
void
qjs_DomResizeReload(MochaDecoder *decoder)
{
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (dom)
		dom->resize_pending = TRUE;
}

static void
qjs_dom_hold(qjs_Dom *dom, dom_node *node)
{
	if (dom->nheld == dom->hcap) {
		uint32 cap = dom->hcap ? dom->hcap * 2 : 64;
		dom_node **h = (dom_node **) XP_REALLOC(dom->held, cap * sizeof *h);
		if (!h)
			return;
		dom->held = h;
		dom->hcap = cap;
	}
	dom->held[dom->nheld++] = (dom_node *) dom_node_ref(node);
}

/* ---- building the tree from layout's tags ---------------------------------- */

static const char *const qjs_void_tags[] = {
	"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame",
	"hr", "image", "img", "input", "isindex", "keygen", "link", "meta",
	"param", "source", "spacer", "track", "wbr", NULL
};

static XP_Bool
qjs_in_list(const char *name, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(name, *list))
			return TRUE;
	return FALSE;
}

/* Elements an open <p> closes before. */
static const char *const qjs_p_closers[] = {
	"address", "article", "aside", "blockquote", "center", "details",
	"dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure",
	"footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup",
	"hr", "li", "listing", "main", "menu", "nav", "ol", "p", "plaintext",
	"pre", "section", "summary", "table", "ul", "xmp", "dd", "dt", NULL
};

static const char *const qjs_head_tags[] = {
	"title", "meta", "link", "style", "base", "script", "noscript", NULL
};

/* Scope boundaries: closing tags never pop past these. */
static const char *const qjs_scope_tags[] = {
	"html", "table", "td", "th", "caption", "applet", "object",
	"marquee", "template", NULL
};

static int
qjs_dom_find_open(qjs_Dom *dom, const char *name, const char *const *stop)
{
	int i;

	for (i = dom->depth - 1; i >= 0; i--) {
		if (!strcmp(dom->open[i].name, name))
			return i;
		if (stop && qjs_in_list(dom->open[i].name, stop))
			return -1;
	}
	return -1;
}

static void
qjs_dom_pop_to(qjs_Dom *dom, int level)
{
	while (dom->depth > level) {
		dom->depth--;
		dom_node_unref(dom->open[dom->depth].node);
	}
}

static dom_node *
qjs_dom_parent(qjs_Dom *dom)
{
	if (dom->depth > 0)
		return dom->open[dom->depth - 1].node;
	if (dom->body)
		return dom->body;
	if (dom->html)
		return dom->html;
	return (dom_node *) dom->doc;
}

static void
qjs_dom_append(dom_node *parent, dom_node *child)
{
	dom_node *ret = NULL;

	if (dom_node_append_child(parent, child, &ret) == DOM_NO_ERR && ret)
		dom_node_unref(ret);
}

static dom_node *
qjs_dom_create(qjs_Dom *dom, const char *name)
{
	dom_string *s = qjs_dstr(name, strlen(name));
	dom_element *el = NULL;

	if (!s)
		return NULL;
	if (dom_document_create_element(dom->doc, s, &el) != DOM_NO_ERR)
		el = NULL;
	dom_string_unref(s);
	return (dom_node *) el;
}

static void
qjs_dom_push(qjs_Dom *dom, dom_node *node, const char *name)
{
	if (dom->depth >= QJS_DOM_DEPTH)
		return;
	dom->open[dom->depth].node = (dom_node *) dom_node_ref(node);
	qjs_copy(dom->open[dom->depth].name, name,
					sizeof dom->open[dom->depth].name);
	dom->depth++;
}

static void
qjs_dom_ensure_html(qjs_Dom *dom)
{
	dom_node *n;

	if (dom->html)
		return;
	n = qjs_dom_create(dom, "html");
	if (!n)
		return;
	qjs_dom_append((dom_node *) dom->doc, n);
	dom->html = n;
	dom_node_unref(n);
	dom->depth = 0;
}

static void
qjs_dom_ensure_head(qjs_Dom *dom)
{
	dom_node *n;

	qjs_dom_ensure_html(dom);
	if (dom->head || !dom->html)
		return;
	n = qjs_dom_create(dom, "head");
	if (!n)
		return;
	qjs_dom_append(dom->html, n);
	dom->head = n;
	dom_node_unref(n);
}

static void
qjs_dom_ensure_body(qjs_Dom *dom)
{
	dom_node *n;

	qjs_dom_ensure_head(dom);
	if (dom->body || !dom->html)
		return;
	/* whatever was open in the head is over */
	qjs_dom_pop_to(dom, 0);
	n = qjs_dom_create(dom, "body");
	if (!n)
		return;
	qjs_dom_append(dom->html, n);
	dom->body = n;
	dom_node_unref(n);
}

/* The name of a tag: Netscape's own, or the word an unknown tag starts with.
 * Lower case; "" for tags layout makes for itself. */
static void
qjs_tag_name(PA_Tag *tag, char *buf, size_t size)
{
	buf[0] = '\0';
	switch (tag->type) {
	case P_TEXT:
	case P_NSCP_CLOSE:
	case P_NSCP_OPEN:
	case P_NSCP_REBLOCK:
	case P_NSDT:
	case P_SPELL:
	case P_INLINEINPUT:
	case P_INLINEINPUTTHICK:
	case P_INLINEINPUTDOTTED:
	case P_CERTIFICATE:
	case P_SERVER:
	case P_BUILTIN:
		return;
	case P_UNKNOWN: {
		char *p;
		int32 i = 0, k = 0;

		if (!tag->data)
			return;
		PA_LOCK(p, char *, tag->data);
		while (i < tag->data_len && isspace((unsigned char) p[i]))
			i++;
		if (i < tag->data_len && p[i] == '/')
			i++;
		if (i < tag->data_len && isalpha((unsigned char) p[i])) {
			while (i < tag->data_len && k < (int32) size - 1 &&
				   (isalnum((unsigned char) p[i]) || p[i] == '-' ||
					p[i] == '_' || p[i] == ':'))
				buf[k++] = tolower((unsigned char) p[i++]);
			/* "name=" would be an attribute (layout's relayout dummy) */
			if (i < tag->data_len && !isspace((unsigned char) p[i]) &&
				p[i] != '/' && p[i] != '>')
				k = 0;
		}
		buf[k] = '\0';
		PA_UNLOCK(tag->data);
		return;
	}
	default: {
		const char *s = PA_TagString(tag->type);
		size_t i;
		if (!s)
			return;
		for (i = 0; s[i] && i + 1 < size; i++)
			buf[i] = tolower((unsigned char) s[i]);
		buf[i] = '\0';
		return;
	}
	}
}

static void
qjs_dom_set_attrs(MWContext *context, dom_node *el, PA_Tag *tag,
				  const char *name)
{
	char **names = NULL, **values = NULL;
	int32 n, i;

	n = PA_FetchAllNameValues(tag, &names, &values, CS_FE_ASCII);
	for (i = 0; i < n; i++) {
		char *v = NULL;
		dom_string *dn, *dv;

		/* an unknown tag's name comes back as its first attribute */
		if (i == 0 && tag->type == P_UNKNOWN && names[i] &&
			!XP_STRCASECMP(names[i], name))
			goto next;
		if (!names[i] || !names[i][0])
			goto next;
		if (values[i])
			v = qjs_FromDocumentCharset(context, values[i], XP_STRLEN(values[i]));
		dn = qjs_dstr_lower(names[i], XP_STRLEN(names[i]));
		dv = qjs_dstr(v ? v : "", v ? XP_STRLEN(v) : 0);
		if (dn && dv)
			dom_element_set_attribute((dom_element *) el, dn, dv);
		if (dn)
			dom_string_unref(dn);
		if (dv)
			dom_string_unref(dv);
		XP_FREEIF(v);
	next:
		XP_FREEIF(names[i]);
		XP_FREEIF(values[i]);
	}
	XP_FREEIF(names);
	XP_FREEIF(values);
}

static void
qjs_dom_text(MWContext *context, qjs_Dom *dom, PA_Tag *tag)
{
	char *p, *u;
	dom_node *parent, *last = NULL;
	dom_node_type type;
	dom_string *s;
	int32 i;
	XP_Bool blank = TRUE;

	if (!tag->data || tag->data_len <= 0)
		return;
	PA_LOCK(p, char *, tag->data);
	for (i = 0; i < tag->data_len; i++)
		if (!isspace((unsigned char) p[i])) {
			blank = FALSE;
			break;
		}
	if (!dom->body && blank && dom->depth == 0) {
		PA_UNLOCK(tag->data);
		return;		/* between the head's elements */
	}
	u = qjs_FromDocumentCharset(context, p, tag->data_len);
	PA_UNLOCK(tag->data);
	if (!u)
		return;
	if (!dom->body && dom->depth == 0)
		qjs_dom_ensure_body(dom);
	parent = qjs_dom_parent(dom);
	s = qjs_dstr(u, XP_STRLEN(u));
	XP_FREE(u);
	if (!s)
		return;
	/* run on the last text node */
	if (dom_node_get_last_child(parent, &last) == DOM_NO_ERR && last) {
		if (dom_node_get_node_type(last, &type) == DOM_NO_ERR &&
			type == DOM_TEXT_NODE &&
			dom_characterdata_append_data((dom_characterdata *) last, s) ==
				DOM_NO_ERR) {
			tag->dom_node = last;
			dom_node_unref(last);
			dom_string_unref(s);
			return;
		}
		dom_node_unref(last);
	}
	{
		dom_text *t = NULL;
		if (dom_document_create_text_node(dom->doc, s, &t) == DOM_NO_ERR && t) {
			qjs_dom_append(parent, (dom_node *) t);
			tag->dom_node = t;
			dom_node_unref(t);
		}
	}
	dom_string_unref(s);
}

static void
qjs_dom_end_tag(qjs_Dom *dom, const char *name)
{
	int i;

	if (!strcmp(name, "html") || !strcmp(name, "body") || !strcmp(name, "head")) {
		if (!strcmp(name, "head"))
			qjs_dom_pop_to(dom, 0);
		return;
	}
	if (!strcmp(name, "p") || !strcmp(name, "li") || !strcmp(name, "dd") ||
		!strcmp(name, "dt"))
		i = qjs_dom_find_open(dom, name, qjs_scope_tags);
	else if (!strcmp(name, "table"))
		i = qjs_dom_find_open(dom, name, NULL);
	else
		i = qjs_dom_find_open(dom, name, qjs_scope_tags);
	if (i < 0 && qjs_in_list(name, qjs_scope_tags))
		i = qjs_dom_find_open(dom, name, NULL);
	if (i >= 0)
		qjs_dom_pop_to(dom, i);
}

/* Close what the start of NAME closes. */
static void
qjs_dom_implied_ends(qjs_Dom *dom, const char *name)
{
	static const char *const list_stop[] = {
		"ul", "ol", "menu", "dir", "html", "table", "td", "th", NULL };
	static const char *const dl_stop[] = { "dl", "html", "table", "td", "th", NULL };
	static const char *const row_stop[] = { "table", "html", NULL };
	static const char *const cell_stop[] = { "tr", "table", "html", NULL };
	static const char *const sel_stop[] = { "select", "html", "table", NULL };
	int i;

	if (qjs_in_list(name, qjs_p_closers) &&
		(i = qjs_dom_find_open(dom, "p", qjs_scope_tags)) >= 0)
		qjs_dom_pop_to(dom, i);
	if (!strcmp(name, "li")) {
		if ((i = qjs_dom_find_open(dom, "li", list_stop)) >= 0)
			qjs_dom_pop_to(dom, i);
	} else if (!strcmp(name, "dt") || !strcmp(name, "dd")) {
		if ((i = qjs_dom_find_open(dom, "dt", dl_stop)) >= 0 ||
			(i = qjs_dom_find_open(dom, "dd", dl_stop)) >= 0)
			qjs_dom_pop_to(dom, i);
	} else if (!strcmp(name, "option") || !strcmp(name, "optgroup")) {
		if ((i = qjs_dom_find_open(dom, "option", sel_stop)) >= 0)
			qjs_dom_pop_to(dom, i);
		if (!strcmp(name, "optgroup") &&
			(i = qjs_dom_find_open(dom, "optgroup", sel_stop)) >= 0)
			qjs_dom_pop_to(dom, i);
	} else if (!strcmp(name, "tr")) {
		if ((i = qjs_dom_find_open(dom, "tr", row_stop)) >= 0)
			qjs_dom_pop_to(dom, i);
	} else if (!strcmp(name, "td") || !strcmp(name, "th")) {
		if ((i = qjs_dom_find_open(dom, "td", cell_stop)) >= 0 ||
			(i = qjs_dom_find_open(dom, "th", cell_stop)) >= 0)
			qjs_dom_pop_to(dom, i);
	} else if (!strcmp(name, "thead") || !strcmp(name, "tbody") ||
			   !strcmp(name, "tfoot")) {
		if ((i = qjs_dom_find_open(dom, "thead", row_stop)) >= 0 ||
			(i = qjs_dom_find_open(dom, "tbody", row_stop)) >= 0 ||
			(i = qjs_dom_find_open(dom, "tfoot", row_stop)) >= 0)
			qjs_dom_pop_to(dom, i);
	} else if (!strcmp(name, "a")) {
		if ((i = qjs_dom_find_open(dom, "a", qjs_scope_tags)) >= 0)
			qjs_dom_pop_to(dom, i);
	}
}

/* Rows and cells straight in a table get the tbody and tr the HTML
 * parser would give them. */
static void
qjs_dom_table_parents(qjs_Dom *dom, const char *name)
{
	const char *top = dom->depth > 0 ? dom->open[dom->depth - 1].name : "";
	dom_node *n;

	if (!strcmp(name, "tr") && !strcmp(top, "table")) {
		if ((n = qjs_dom_create(dom, "tbody")) != NULL) {
			qjs_dom_append(qjs_dom_parent(dom), n);
			qjs_dom_push(dom, n, "tbody");
			dom_node_unref(n);
		}
	} else if ((!strcmp(name, "td") || !strcmp(name, "th")) &&
			   (!strcmp(top, "table") || !strcmp(top, "tbody") ||
				!strcmp(top, "thead") || !strcmp(top, "tfoot"))) {
		if (!strcmp(top, "table"))
			qjs_dom_table_parents(dom, "tr");
		if ((n = qjs_dom_create(dom, "tr")) != NULL) {
			qjs_dom_append(qjs_dom_parent(dom), n);
			qjs_dom_push(dom, n, "tr");
			dom_node_unref(n);
		}
	}
}

/* Elements whose NAME (or a form control's ID) is a property of the
 * document or their form. */
static const char *const qjs_named_tags[] = {
	"form", "img", "embed", "object", "applet", "iframe", "input", "select",
	"textarea", "button", "fieldset", "output", NULL
};

static void
qjs_dom_named(qjs_Dom *dom, dom_node *el)
{
	if (dom->nnamed == dom->ncap) {
		uint32 cap = dom->ncap ? dom->ncap * 2 : 32;
		dom_node **n = (dom_node **) XP_REALLOC(dom->named, cap * sizeof *n);
		if (!n)
			return;
		dom->named = n;
		dom->ncap = cap;
	}
	dom->named[dom->nnamed++] = (dom_node *) dom_node_ref(el);
}

/* Give JavaScript the named elements made since last time. */
void
qjs_DomSyncNames(JSContext *cx)
{
	qjs_Dom *dom = qjs_dom_of((MochaDecoder *) JS_GetContextOpaque(cx));
	JSValue global, fn, a, rv;
	uint32 i;

	if (!dom || !dom->nnamed)
		return;
	a = JS_NewArray(cx);
	for (i = 0; i < dom->nnamed; i++) {
		JS_SetPropertyUint32(cx, a, i, qjs_wrap(cx, dom->named[i]));
		dom_node_unref(dom->named[i]);
	}
	dom->nnamed = 0;
	global = JS_GetGlobalObject(cx);
	fn = JS_GetPropertyStr(cx, global, "__ns_names");
	if (JS_IsFunction(cx, fn)) {
		rv = JS_Call(cx, fn, global, 1, &a);
		if (JS_IsException(rv))
			qjs_ReportException(cx);
		JS_FreeValue(cx, rv);
	}
	JS_FreeValue(cx, fn);
	JS_FreeValue(cx, a);
	JS_FreeValue(cx, global);
}

static void
qjs_dom_start_tag(MWContext *context, qjs_Dom *dom, PA_Tag *tag,
				  const char *name)
{
	dom_node *el;

	if (!strcmp(name, "html")) {
		qjs_dom_ensure_html(dom);
		if (dom->html) {
			qjs_dom_set_attrs(context, dom->html, tag, name);
			tag->dom_node = dom->html;
		}
		return;
	}
	if (!strcmp(name, "head")) {
		qjs_dom_ensure_head(dom);
		if (dom->head) {
			qjs_dom_set_attrs(context, dom->head, tag, name);
			tag->dom_node = dom->head;
		}
		return;
	}
	if (!strcmp(name, "body") || !strcmp(name, "frameset")) {
		qjs_dom_ensure_body(dom);
		if (dom->body) {
			qjs_dom_set_attrs(context, dom->body, tag, name);
			tag->dom_node = dom->body;
		}
		return;
	}
	if (!dom->body && (dom->depth > 0 || qjs_in_list(name, qjs_head_tags))) {
		qjs_dom_ensure_head(dom);
		if (dom->depth == 0)
			qjs_dom_push(dom, dom->head, "head");
	} else {
		if (!dom->body)
			qjs_dom_ensure_body(dom);
		qjs_dom_implied_ends(dom, name);
		qjs_dom_table_parents(dom, name);
	}
	el = qjs_dom_create(dom, name);
	if (!el)
		return;
	qjs_dom_set_attrs(context, el, tag, name);
	qjs_dom_append(qjs_dom_parent(dom), el);
	tag->dom_node = el;
	if (qjs_in_list(name, qjs_named_tags))
		qjs_dom_named(dom, el);
	if (!strcmp(name, "script")) {
		dom_string *k = qjs_dstr("type", 4), *v = NULL;
		if (k) {
			dom_element_get_attribute((dom_element *) el, k, &v);
			dom_string_unref(k);
		}
		if (v) {
			if (dom_string_byte_length(v) == 6 &&
				!strncasecmp(dom_string_data(v), "module", 6))
				dom->module_scripts = TRUE;
			dom_string_unref(v);
		}
		if (dom->current_script)
			dom_node_unref(dom->current_script);
		dom->current_script = (dom_node *) dom_node_ref(el);
	}
	if (!qjs_in_list(name, qjs_void_tags) && tag->type != P_IMAGE) {
		char *p;
		XP_Bool self_closing = FALSE;

		/* <foo/>: XHTML's empty elements (unknown tags only: HTML's own
		 * know whether they are empty) */
		if (tag->type == P_UNKNOWN && tag->data && tag->data_len > 1) {
			PA_LOCK(p, char *, tag->data);
			self_closing = p[tag->data_len - 2] == '/';
			PA_UNLOCK(tag->data);
		}
		if (!self_closing)
			qjs_dom_push(dom, el, name);
	}
	dom_node_unref(el);
}

/* After a resize: the next element in document order named NAME. */
static void
qjs_dom_remap(qjs_Dom *dom, PA_Tag *tag, const char *name)
{
	dom_node *n, *next;
	dom_string *want;
	int steps;

	if (tag->is_end || !name[0] || !strcmp(name, "base"))
		return;
	if (!dom->cursor)
		dom->cursor = (dom_node *) dom_node_ref(dom->doc);
	want = qjs_dstr(name, strlen(name));
	if (!want)
		return;
	n = (dom_node *) dom_node_ref(dom->cursor);
	for (steps = 0; steps < 2000 && n; steps++) {
		/* the next node in document order */
		next = NULL;
		if (dom_node_get_first_child(n, &next) != DOM_NO_ERR || !next) {
			dom_node *up = (dom_node *) dom_node_ref(n);
			while (up) {
				dom_node *parent = NULL;
				if (dom_node_get_next_sibling(up, &next) == DOM_NO_ERR && next) {
					dom_node_unref(up);
					break;
				}
				dom_node_get_parent_node(up, &parent);
				dom_node_unref(up);
				up = parent;
			}
		}
		dom_node_unref(n);
		n = next;
		if (n) {
			dom_node_type type;
			dom_string *nm = NULL;
			if (dom_node_get_node_type(n, &type) == DOM_NO_ERR &&
				type == DOM_ELEMENT_NODE &&
				dom_node_get_node_name(n, &nm) == DOM_NO_ERR && nm) {
				bool same = dom_string_caseless_isequal(nm, want);
				dom_string_unref(nm);
				if (same) {
					dom_node_unref(dom->cursor);
					dom->cursor = n;		/* our reference */
					tag->dom_node = n;
					dom_string_unref(want);
					return;
				}
			}
		}
	}
	if (n)
		dom_node_unref(n);
	dom_string_unref(want);
}

/* Layout is about to lay TAG out. */
void
LM_DomTag(MWContext *context, PA_Tag *tag)
{
	MochaDecoder *decoder;
	qjs_Dom *dom;
	char name[24];

	tag->dom_node = NULL;
	if (!LM_CanDoJS(context) || EDT_IS_EDITOR(context))
		return;
	decoder = qjs_GetDecoder(context, TRUE);
	if (!decoder)
		return;
	dom = qjs_dom_of(decoder);
	if (dom && (dom->resize_pending || dom->doc_id != XP_DOCID(context)))
		dom->nfields = 0;		/* layout starts over: its form elements go */
	if (dom && dom->resize_pending) {
		if (getenv("QJS_DOM_TRACE"))
			qjs_Log("dom: remapping the laid out document");
		dom->resize_pending = FALSE;
		dom->remap = TRUE;
		dom->doc_id = XP_DOCID(context);
		if (dom->cursor)
			dom_node_unref(dom->cursor);
		dom->cursor = NULL;
	} else if (dom && dom->doc_id != XP_DOCID(context)) {
		/* a new document: the old one's scripts and tree go */
		qjs_DropContext(decoder);
		dom = NULL;
	}
	if (!dom) {
		if (getenv("QJS_DOM_TRACE"))
			qjs_Log("dom: new document %ld", (long) XP_DOCID(context));
		decoder->doc_id = XP_DOCID(context);
		dom = qjs_dom_new(decoder);
		if (!dom)
			return;
	}
	qjs_tag_name(tag, name, sizeof name);
	if (dom->remap) {
		qjs_dom_remap(dom, tag, name);
		return;
	}
	if (tag->type == P_TEXT) {
		qjs_dom_text(context, dom, tag);
		return;
	}
	if (!name[0])
		return;
	if (tag->is_end)
		qjs_dom_end_tag(dom, name);
	else
		qjs_dom_start_tag(context, dom, tag, name);
}

/* ---- layout's objects and their elements --------------------------------- */

void
ET_ReflectObject(MWContext *context, void *lo_ele, void *tagp,
				 int32 layer_id, uint index, ReflectedObject type)
{
	PA_Tag *tag = (PA_Tag *) tagp;
	qjs_Dom *dom = qjs_dom_of(qjs_GetDecoder(context, FALSE));
	dom_node *node;

	if (!dom || !tag || !lo_ele || !(node = (dom_node *) tag->dom_node))
		return;
	switch (type) {
	case LM_LINKS:
		((LO_AnchorData *) lo_ele)->mocha_object = (JSObject *) node;
		break;
	case LM_IMAGES:
		((LO_ImageStruct *) lo_ele)->mocha_object = (JSObject *) node;
		break;
	case LM_FORMS:
		((lo_FormData *) lo_ele)->mocha_object = (JSObject *) node;
		break;
	case LM_NAMEDANCHORS:
		((lo_NameList *) lo_ele)->mocha_object = (JSObject *) node;
		break;
	default:
		return;
	}
	qjs_dom_hold(dom, node);
}

void
ET_ReflectFormElement(MWContext *context, void *form,
					  LO_FormElementStruct *form_element, PA_Tag *tag)
{
	qjs_Dom *dom = qjs_dom_of(qjs_GetDecoder(context, FALSE));

	if (!dom || !tag || !tag->dom_node || !form_element)
		return;
	form_element->mocha_object = (JSObject *) tag->dom_node;
	qjs_dom_hold(dom, (dom_node *) tag->dom_node);
	dom->context = context;
	{
		uint32 i;
		for (i = 0; i < dom->nfields; i++)
			if (dom->fields[i].node == (dom_node *) tag->dom_node)
				break;
		if (i == dom->nfields) {
			if (dom->nfields == dom->fcap) {
				uint32 cap = dom->fcap ? dom->fcap * 2 : 32;
				void *a = XP_REALLOC(dom->fields, cap * sizeof *dom->fields);
				if (!a)
					return;
				dom->fields = a;
				dom->fcap = cap;
			}
			dom->nfields++;
		}
		dom->fields[i].node = (dom_node *) tag->dom_node;
		dom->fields[i].fe = form_element;
	}
}

static LO_FormElementStruct *
qjs_field_of(qjs_Dom *dom, dom_node *node)
{
	uint32 i;

	for (i = 0; dom && i < dom->nfields; i++)
		if (dom->fields[i].node == node)
			return dom->fields[i].fe;
	return NULL;
}


/* The element an event on layout's ELEMENT goes to, or NULL. */
static dom_node *
qjs_ElementOf(LO_Element *element)
{
	if (!element)
		return NULL;
	switch (element->type) {
	case LO_TEXT:
		if (element->lo_text.anchor_href)
			return (dom_node *) element->lo_text.anchor_href->mocha_object;
		return NULL;
	case LO_IMAGE:
		if (element->lo_image.mocha_object)
			return (dom_node *) element->lo_image.mocha_object;
		if (element->lo_image.anchor_href)
			return (dom_node *) element->lo_image.anchor_href->mocha_object;
		return NULL;
	case LO_FORM_ELE:
		return (dom_node *) element->lo_form.mocha_object;
	default:
		return NULL;
	}
}

/* ---- JavaScript's node objects -------------------------------------------- */

static void
qjs_node_finalizer(JSRuntime *rt, JSValue val)
{
	qjs_NodeRef *ref = (qjs_NodeRef *) JS_GetOpaque(val, qjs_node_class);

	if (!ref)
		return;
	if (ref->node)
		dom_node_unref(ref->node);
	XP_FREE(ref);
}

static JSClassDef qjs_node_class_def = {
	"Node",
	.finalizer = qjs_node_finalizer,
};

static void
qjs_dom_free_wrappers(qjs_Dom *dom)
{
	uint32 i;

	if (!dom->cx)
		return;
	for (i = 0; i < dom->wcap; i++) {
		qjs_Wrap *w = &dom->wraps[i];
		if (!w->node)
			continue;
		if (w->ref && w->ref->node) {
			dom_node_unref(w->ref->node);
			w->ref->node = NULL;
		}
		JS_FreeValue(dom->cx, w->obj);
	}
	XP_FREEIF(dom->wraps);
	dom->wraps = NULL;
	dom->nwraps = dom->wcap = 0;
	JS_FreeValue(dom->cx, dom->protos);
	dom->protos = JS_UNDEFINED;
	dom->cx = NULL;
}

static uint32
qjs_hash_ptr(void *p, uint32 cap)
{
	uintptr_t x = (uintptr_t) p;

	x ^= x >> 7;
	x *= 2654435761u;
	return (uint32) (x ^ (x >> 15)) & (cap - 1);
}

static XP_Bool
qjs_wrap_grow(qjs_Dom *dom)
{
	uint32 cap = dom->wcap ? dom->wcap * 2 : 1024, i;
	qjs_Wrap *w = (qjs_Wrap *) XP_CALLOC(cap, sizeof *w);

	if (!w)
		return FALSE;
	for (i = 0; i < dom->wcap; i++) {
		qjs_Wrap *o = &dom->wraps[i];
		uint32 h;
		if (!o->node)
			continue;
		for (h = qjs_hash_ptr(o->node, cap); w[h].node; h = (h + 1) & (cap - 1))
			;
		w[h] = *o;
	}
	XP_FREEIF(dom->wraps);
	dom->wraps = w;
	dom->wcap = cap;
	return TRUE;
}

static qjs_Dom *
qjs_cx_dom(JSContext *cx)
{
	return qjs_dom_of((MochaDecoder *) JS_GetContextOpaque(cx));
}

static JSValue
qjs_proto_for(JSContext *cx, qjs_Dom *dom, dom_node *node)
{
	dom_node_type type;
	JSValue p = JS_UNDEFINED;
	char key[40];

	if (!JS_IsObject(dom->protos))
		return JS_NULL;
	if (dom_node_get_node_type(node, &type) != DOM_NO_ERR)
		return JS_NULL;
	if (type == DOM_ELEMENT_NODE) {
		dom_string *nm = NULL;
		if (dom_node_get_node_name(node, &nm) == DOM_NO_ERR && nm) {
			if (dom_string_byte_length(nm) < sizeof key - 1) {
				XP_SPRINTF(key, "%s", dom_string_data(nm));
				key[dom_string_byte_length(nm)] = '\0';
				p = JS_GetPropertyStr(cx, dom->protos, key);
			}
			dom_string_unref(nm);
		}
	}
	if (!JS_IsObject(p)) {
		JS_FreeValue(cx, p);
		XP_SPRINTF(key, "%d", (int) type);
		p = JS_GetPropertyStr(cx, dom->protos, key);
	}
	if (!JS_IsObject(p)) {
		JS_FreeValue(cx, p);
		p = JS_GetPropertyStr(cx, dom->protos, "0");
	}
	return p;
}

/* The object for NODE (a new reference), or null. */
static JSValue
qjs_wrap(JSContext *cx, dom_node *node)
{
	qjs_Dom *dom = qjs_cx_dom(cx);
	qjs_NodeRef *ref;
	JSValue obj, proto;
	uint32 h;

	if (!node || !dom)
		return JS_NULL;
	if (dom->cx != cx) {
		if (dom->cx)
			qjs_dom_free_wrappers(dom);
		dom->cx = cx;
	}
	if (dom->wcap) {
		for (h = qjs_hash_ptr(node, dom->wcap); dom->wraps[h].node;
			 h = (h + 1) & (dom->wcap - 1))
			if (dom->wraps[h].node == node)
				return JS_DupValue(cx, dom->wraps[h].obj);
	}
	if ((dom->nwraps + 1) * 2 > dom->wcap && !qjs_wrap_grow(dom))
		return JS_NULL;
	proto = qjs_proto_for(cx, dom, node);
	obj = JS_IsObject(proto) ? JS_NewObjectProtoClass(cx, proto, qjs_node_class)
							 : JS_NewObjectClass(cx, qjs_node_class);
	JS_FreeValue(cx, proto);
	if (JS_IsException(obj))
		return obj;
	ref = XP_NEW_ZAP(qjs_NodeRef);
	if (!ref) {
		JS_FreeValue(cx, obj);
		return JS_NULL;
	}
	ref->node = (dom_node *) dom_node_ref(node);
	JS_SetOpaque(obj, ref);
	for (h = qjs_hash_ptr(node, dom->wcap); dom->wraps[h].node;
		 h = (h + 1) & (dom->wcap - 1))
		;
	dom->wraps[h].node = node;
	dom->wraps[h].obj = JS_DupValue(cx, obj);
	dom->wraps[h].ref = ref;
	dom->nwraps++;
	return obj;
}

/* The node behind V, or NULL (no exception). */
static dom_node *
qjs_node_of(JSValueConst v)
{
	qjs_NodeRef *ref = (qjs_NodeRef *) JS_GetOpaque(v, qjs_node_class);

	return ref ? ref->node : NULL;
}

static dom_node *
qjs_node_arg(JSContext *cx, int argc, JSValueConst *argv, int i)
{
	return i < argc ? qjs_node_of(argv[i]) : NULL;
}

#define NODE_ARG(var, i)											\
	dom_node *var = qjs_node_arg(cx, argc, argv, i);				\
	if (!var)														\
		return JS_ThrowTypeError(cx, "not a node")

static JSValue
qjs_wrap_ret(JSContext *cx, dom_exception err, dom_node *n)
{
	JSValue v;

	if (err != DOM_NO_ERR || !n)
		return JS_NULL;
	v = qjs_wrap(cx, n);
	dom_node_unref(n);
	return v;
}

static JSValue
qjs_dom_error(JSContext *cx, dom_exception err)
{
	static const char *const names[] = {
		"", "IndexSizeError", "DOMStringSizeError", "HierarchyRequestError",
		"WrongDocumentError", "InvalidCharacterError", "NoDataAllowedError",
		"NoModificationAllowedError", "NotFoundError", "NotSupportedError",
		"InUseAttributeError", "InvalidStateError", "SyntaxError",
		"InvalidModificationError", "NamespaceError", "InvalidAccessError"
	};
	int e = (int) err;

	return JS_ThrowTypeError(cx, "DOM: %s",
							 (e > 0 && e < (int) (sizeof names / sizeof names[0]))
							 ? names[e] : "error");
}

static void qjs_dom_schedule(MochaDecoder *decoder, qjs_Dom *dom);

/* Would a change at NODE show?  Not if NODE is outside the document (a
 * tree a script builds before putting it in), in the head, or in a script
 * element: then laying the document out again is wasted. */
static XP_Bool
qjs_dom_shows(qjs_Dom *dom, dom_node *node)
{
	dom_node *n = (dom_node *) dom_node_ref(node), *p;

	while (n) {
		dom_node_type t;
		if (n == (dom_node *) dom->doc) {
			dom_node_unref(n);
			return TRUE;
		}
		if (n == dom->head) {
			dom_node_unref(n);
			return FALSE;
		}
		if (dom_node_get_node_type(n, &t) == DOM_NO_ERR && t == DOM_ELEMENT_NODE) {
			dom_string *nm = NULL;
			XP_Bool script = FALSE;
			if (dom_node_get_node_name(n, &nm) == DOM_NO_ERR && nm) {
				script = !strcmp(dom_string_data(nm), "SCRIPT") ||
						 !strcmp(dom_string_data(nm), "TEMPLATE");
				dom_string_unref(nm);
			}
			if (script) {
				dom_node_unref(n);
				return FALSE;
			}
		}
		p = NULL;
		dom_node_get_parent_node(n, &p);
		dom_node_unref(n);
		n = p;
	}
	return FALSE;
}

/* A script changed the tree at NODE. */
static void
qjs_mutated(JSContext *cx, dom_node *node)
{
	MochaDecoder *decoder = (MochaDecoder *) JS_GetContextOpaque(cx);
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (!dom || dom->mutated || !qjs_dom_shows(dom, node))
		return;
	dom->mutated = TRUE;
	qjs_dom_schedule(decoder, dom);
}

/* ---- natives: the tree ------------------------------------------------- */

static JSValue
dom_document_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	MochaDecoder *decoder = (MochaDecoder *) JS_GetContextOpaque(cx);
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (!dom && decoder)
		dom = qjs_dom_new(decoder);
	if (!dom)
		return JS_NULL;
	return qjs_wrap(cx, (dom_node *) dom->doc);
}

static JSValue
dom_type_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_node_type t;
	NODE_ARG(n, 0);

	if (dom_node_get_node_type(n, &t) != DOM_NO_ERR)
		return JS_NewInt32(cx, 0);
	return JS_NewInt32(cx, (int32_t) t);
}

static JSValue
dom_name_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *s = NULL;
	JSValue v;
	NODE_ARG(n, 0);

	if (dom_node_get_node_name(n, &s) != DOM_NO_ERR || !s)
		return JS_NewString(cx, "");
	v = qjs_jsstr(cx, s);
	dom_string_unref(s);
	return v;
}

typedef dom_exception (*qjs_NodeGetter)(dom_node *, dom_node **);

static dom_exception g_parent(dom_node *n, dom_node **r) { return dom_node_get_parent_node(n, r); }
static dom_exception g_first(dom_node *n, dom_node **r) { return dom_node_get_first_child(n, r); }
static dom_exception g_last(dom_node *n, dom_node **r) { return dom_node_get_last_child(n, r); }
static dom_exception g_next(dom_node *n, dom_node **r) { return dom_node_get_next_sibling(n, r); }
static dom_exception g_prev(dom_node *n, dom_node **r) { return dom_node_get_previous_sibling(n, r); }

static JSValue
dom_rel_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv,
		   int magic)
{
	static const qjs_NodeGetter getters[] = { g_parent, g_first, g_last, g_next, g_prev };
	dom_node *r = NULL;
	dom_exception err;
	NODE_ARG(n, 0);

	err = getters[magic](n, &r);
	return qjs_wrap_ret(cx, err, r);
}

/* The children of N (magic 0), or only its element children (1). */
static JSValue
dom_kids_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv,
			int magic)
{
	JSValue a = JS_NewArray(cx);
	dom_node *c = NULL, *next;
	uint32_t i = 0;
	NODE_ARG(n, 0);

	if (JS_IsException(a))
		return a;
	dom_node_get_first_child(n, &c);
	while (c) {
		dom_node_type t;
		if (!magic || (dom_node_get_node_type(c, &t) == DOM_NO_ERR &&
					   t == DOM_ELEMENT_NODE))
			JS_SetPropertyUint32(cx, a, i++, qjs_wrap(cx, c));
		next = NULL;
		dom_node_get_next_sibling(c, &next);
		dom_node_unref(c);
		c = next;
	}
	return a;
}

static JSValue
dom_data_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *s = NULL;
	JSValue v;
	NODE_ARG(n, 0);

	if (dom_node_get_node_value(n, &s) != DOM_NO_ERR || !s)
		return JS_NULL;
	v = qjs_jsstr(cx, s);
	dom_string_unref(s);
	return v;
}

static JSValue
dom_set_data_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *s;
	NODE_ARG(n, 0);

	if (argc < 2 || !(s = qjs_arg_dstr(cx, argv[1], FALSE)))
		return JS_EXCEPTION;
	dom_node_set_node_value(n, s);
	dom_string_unref(s);
	qjs_mutated(cx, n);
	return JS_UNDEFINED;
}

static JSValue
dom_text_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *s = NULL;
	JSValue v;
	NODE_ARG(n, 0);

	if (dom_node_get_text_content(n, &s) != DOM_NO_ERR || !s)
		return JS_NewString(cx, "");
	v = qjs_jsstr(cx, s);
	dom_string_unref(s);
	return v;
}

static JSValue
dom_set_text_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *s;
	dom_exception err;
	NODE_ARG(n, 0);

	if (argc < 2 || !(s = qjs_arg_dstr(cx, argv[1], FALSE)))
		return JS_EXCEPTION;
	err = dom_node_set_text_content(n, s);
	dom_string_unref(s);
	qjs_mutated(cx, n);
	return err == DOM_NO_ERR ? JS_UNDEFINED : qjs_dom_error(cx, err);
}

/* ---- natives: attributes --------------------------------------------- */

static JSValue
dom_attr_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *name, *v = NULL;
	JSValue r;
	NODE_ARG(n, 0);

	if (argc < 2 || !(name = qjs_arg_dstr(cx, argv[1], TRUE)))
		return JS_EXCEPTION;
	dom_element_get_attribute((dom_element *) n, name, &v);
	dom_string_unref(name);
	r = qjs_jsstr(cx, v);
	if (v)
		dom_string_unref(v);
	return r;
}

static JSValue
dom_set_attr_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *name, *v;
	dom_exception err;
	NODE_ARG(n, 0);

	if (argc < 3 || !(name = qjs_arg_dstr(cx, argv[1], TRUE)))
		return JS_EXCEPTION;
	if (!(v = qjs_arg_dstr(cx, argv[2], FALSE))) {
		dom_string_unref(name);
		return JS_EXCEPTION;
	}
	err = dom_element_set_attribute((dom_element *) n, name, v);
	dom_string_unref(name);
	dom_string_unref(v);
	qjs_mutated(cx, n);
	return err == DOM_NO_ERR ? JS_UNDEFINED : qjs_dom_error(cx, err);
}

static JSValue
dom_rm_attr_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *name;
	NODE_ARG(n, 0);

	if (argc < 2 || !(name = qjs_arg_dstr(cx, argv[1], TRUE)))
		return JS_EXCEPTION;
	dom_element_remove_attribute((dom_element *) n, name);
	dom_string_unref(name);
	qjs_mutated(cx, n);
	return JS_UNDEFINED;
}

static JSValue
dom_has_attr_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_string *name;
	bool has = false;
	NODE_ARG(n, 0);

	if (argc < 2 || !(name = qjs_arg_dstr(cx, argv[1], TRUE)))
		return JS_EXCEPTION;
	dom_element_has_attribute((dom_element *) n, name, &has);
	dom_string_unref(name);
	return JS_NewBool(cx, has);
}

/* [[name, value], ...] */
static JSValue
dom_attrs_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_namednodemap *map = NULL;
	JSValue a;
	dom_ulong len = 0, i;
	NODE_ARG(n, 0);

	a = JS_NewArray(cx);
	if (JS_IsException(a))
		return a;
	if (dom_node_get_attributes(n, &map) != DOM_NO_ERR || !map)
		return a;
	dom_namednodemap_get_length(map, &len);
	for (i = 0; i < len; i++) {
		dom_attr *at = NULL;
		dom_string *nm = NULL, *v = NULL;
		JSValue pair;

		if (dom_namednodemap_item(map, i, (dom_node **) &at) != DOM_NO_ERR || !at)
			continue;
		dom_attr_get_name(at, &nm);
		dom_attr_get_value(at, &v);
		pair = JS_NewArray(cx);
		JS_SetPropertyUint32(cx, pair, 0, nm ? qjs_jsstr(cx, nm) : JS_NewString(cx, ""));
		JS_SetPropertyUint32(cx, pair, 1, v ? qjs_jsstr(cx, v) : JS_NewString(cx, ""));
		JS_SetPropertyUint32(cx, a, (uint32_t) i, pair);
		if (nm)
			dom_string_unref(nm);
		if (v)
			dom_string_unref(v);
		dom_node_unref(at);
	}
	dom_namednodemap_unref(map);
	return a;
}

/* ---- natives: making and moving nodes ------------------------------------ */

static dom_document *
qjs_cx_doc(JSContext *cx)
{
	qjs_Dom *dom = qjs_cx_dom(cx);

	return dom ? dom->doc : NULL;
}

/* magic: 0 element, 1 text, 2 comment, 3 fragment */
static JSValue
dom_create_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv,
			  int magic)
{
	dom_document *doc = qjs_cx_doc(cx);
	dom_string *s = NULL;
	dom_node *n = NULL;
	dom_exception err;

	if (!doc)
		return JS_NULL;
	if (magic != 3 && (argc < 1 || !(s = qjs_arg_dstr(cx, argv[0], magic == 0))))
		return JS_EXCEPTION;
	switch (magic) {
	case 0:
		err = dom_document_create_element(doc, s, (dom_element **) &n);
		break;
	case 1:
		err = dom_document_create_text_node(doc, s, (dom_text **) &n);
		break;
	case 2:
		err = dom_document_create_comment(doc, s, (dom_comment **) &n);
		break;
	default:
		err = dom_document_create_document_fragment(doc,
					(dom_document_fragment **) &n);
		break;
	}
	if (s)
		dom_string_unref(s);
	if (err != DOM_NO_ERR)
		return qjs_dom_error(cx, err);
	return qjs_wrap_ret(cx, err, n);
}

/* insert(parent, node, ref or null) */
static JSValue
dom_insert_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_node *ref = qjs_node_arg(cx, argc, argv, 2), *r = NULL;
	dom_exception err;
	NODE_ARG(parent, 0);
	NODE_ARG(child, 1);

	err = dom_node_insert_before(parent, child, ref, &r);
	if (err != DOM_NO_ERR)
		return qjs_dom_error(cx, err);
	if (r)
		dom_node_unref(r);
	qjs_mutated(cx, parent);
	return JS_DupValue(cx, argv[1]);
}

static JSValue
dom_remove_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_node *r = NULL;
	dom_exception err;
	NODE_ARG(parent, 0);
	NODE_ARG(child, 1);

	err = dom_node_remove_child(parent, child, &r);
	if (err != DOM_NO_ERR)
		return qjs_dom_error(cx, err);
	if (r)
		dom_node_unref(r);
	qjs_mutated(cx, parent);
	return JS_DupValue(cx, argv[1]);
}

static JSValue
dom_replace_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_node *r = NULL;
	dom_exception err;
	NODE_ARG(parent, 0);
	NODE_ARG(newc, 1);
	NODE_ARG(oldc, 2);

	err = dom_node_replace_child(parent, newc, oldc, &r);
	if (err != DOM_NO_ERR)
		return qjs_dom_error(cx, err);
	if (r)
		dom_node_unref(r);
	qjs_mutated(cx, parent);
	return JS_DupValue(cx, argv[2]);
}

static JSValue
dom_clone_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_node *r = NULL;
	dom_exception err;
	NODE_ARG(n, 0);

	err = dom_node_clone_node(n, argc > 1 && JS_ToBool(cx, argv[1]), &r);
	return qjs_wrap_ret(cx, err, r);
}

static JSValue
dom_by_id_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_document *doc = qjs_cx_doc(cx);
	dom_string *id;
	dom_element *el = NULL;
	dom_exception err;

	if (!doc || argc < 1)
		return JS_NULL;
	if (!(id = qjs_arg_dstr(cx, argv[0], FALSE)))
		return JS_EXCEPTION;
	err = dom_document_get_element_by_id(doc, id, &el);
	dom_string_unref(id);
	return qjs_wrap_ret(cx, err, (dom_node *) el);
}

/* contains(a, b): b is a or inside a */
static JSValue
dom_contains_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_node *n, *p;
	NODE_ARG(a, 0);

	if (!(n = qjs_node_arg(cx, argc, argv, 1)))
		return JS_FALSE;
	n = (dom_node *) dom_node_ref(n);
	while (n) {
		if (n == a) {
			dom_node_unref(n);
			return JS_TRUE;
		}
		p = NULL;
		dom_node_get_parent_node(n, &p);
		dom_node_unref(n);
		n = p;
	}
	return JS_FALSE;
}

static JSValue
dom_current_script_fn(JSContext *cx, JSValueConst this_val, int argc,
					  JSValueConst *argv)
{
	qjs_Dom *dom = qjs_cx_dom(cx);

	if (!dom || !dom->current_script)
		return JS_NULL;
	return qjs_wrap(cx, dom->current_script);
}

/* ---- CSS selectors ----------------------------------------------------------
 *
 * Selectors Level 3 and a little of 4: type, universal, #id, .class,
 * [attr], [attr=v], ~= |= ^= $= *= (with " i"), the combinators (descendant,
 * >, +, ~), and :first-child, :last-child, :only-child, :nth-child(),
 * :nth-last-child(), :first-of-type, :last-of-type, :only-of-type,
 * :nth-of-type(), :not(), :is()/:where()/:matches(), :has() (descendants
 * only), :root, :empty, :checked, :disabled, :enabled, :link, :any-link.
 * Dynamic states (:hover, :focus, :active, :visited ...) never match.
 * A selector this cannot read matches nothing.
 */

typedef struct {
	const char *s;		/* the selector text, UTF-8 */
	XP_Bool bad;
} qjs_Sel;

static void
sel_ws(qjs_Sel *p)
{
	while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r' ||
		   *p->s == '\f')
		p->s++;
}

static XP_Bool
sel_ident_char(int c)
{
	return isalnum(c) || c == '-' || c == '_' || (c & 0x80) || c == '\\';
}

/* An identifier into BUF (escapes taken literally). */
static size_t
sel_ident(qjs_Sel *p, char *buf, size_t size)
{
	size_t k = 0;

	while (*p->s && sel_ident_char((unsigned char) *p->s)) {
		char c = *p->s++;
		if (c == '\\' && *p->s)
			c = *p->s++;
		if (k < size - 1)
			buf[k++] = c;
	}
	buf[k] = '\0';
	return k;
}

/* Element facts the matcher needs, fetched once per element. */
typedef struct {
	dom_node *n;
	dom_string *name;		/* upper case */
} qjs_El;

static dom_string *
el_attr(dom_node *n, const char *name)
{
	dom_string *k = qjs_dstr(name, strlen(name)), *v = NULL;

	if (!k)
		return NULL;
	dom_element_get_attribute((dom_element *) n, k, &v);
	dom_string_unref(k);
	return v;
}

static XP_Bool
word_in(const char *list, size_t len, const char *w, size_t wl, XP_Bool ci)
{
	size_t i = 0;

	while (i < len) {
		size_t j;
		while (i < len && isspace((unsigned char) list[i]))
			i++;
		j = i;
		while (j < len && !isspace((unsigned char) list[j]))
			j++;
		if (j - i == wl && (ci ? !strncasecmp(list + i, w, wl)
							   : !strncmp(list + i, w, wl)))
			return TRUE;
		i = j;
	}
	return FALSE;
}

static dom_node *
el_sibling(dom_node *n, XP_Bool forward)
{
	dom_node *s = (dom_node *) dom_node_ref(n), *t;

	for (;;) {
		dom_node_type type;
		t = NULL;
		if (forward)
			dom_node_get_next_sibling(s, &t);
		else
			dom_node_get_previous_sibling(s, &t);
		dom_node_unref(s);
		if (!t)
			return NULL;
		if (dom_node_get_node_type(t, &type) == DOM_NO_ERR &&
			type == DOM_ELEMENT_NODE)
			return t;
		s = t;
	}
}

static dom_node *
el_parent(dom_node *n)
{
	dom_node *p = NULL;
	dom_node_type type;

	dom_node_get_parent_node(n, &p);
	if (p && (dom_node_get_node_type(p, &type) != DOM_NO_ERR ||
			  type != DOM_ELEMENT_NODE)) {
		dom_node_unref(p);
		return NULL;
	}
	return p;
}

static XP_Bool
el_same_name(dom_node *a, dom_node *b)
{
	dom_string *x = NULL, *y = NULL;
	XP_Bool same;

	dom_node_get_node_name(a, &x);
	dom_node_get_node_name(b, &y);
	same = x && y && dom_string_isequal(x, y);
	if (x)
		dom_string_unref(x);
	if (y)
		dom_string_unref(y);
	return same;
}

/* 1-based position among element siblings (of the same type if TYPED),
 * counted from the start or (FROM_END) the end. */
static int
el_index(dom_node *n, XP_Bool from_end, XP_Bool typed)
{
	dom_node *s = (dom_node *) dom_node_ref(n), *t;
	int i = 1;

	while ((t = el_sibling(s, from_end)) != NULL) {
		dom_node_unref(s);
		s = t;
		if (!typed || el_same_name(n, s))
			i++;
	}
	dom_node_unref(s);
	return i;
}

/* an+b from "odd", "even", "3", "2n+1", "-n+3" ... */
static XP_Bool
sel_nth(const char *arg, int *a, int *b)
{
	const char *s = arg;
	int sign = 1, n;

	while (isspace((unsigned char) *s))
		s++;
	if (!strncasecmp(s, "odd", 3)) {
		*a = 2;
		*b = 1;
		return TRUE;
	}
	if (!strncasecmp(s, "even", 4)) {
		*a = 2;
		*b = 0;
		return TRUE;
	}
	*a = 0;
	*b = 0;
	if (*s == '-' || *s == '+') {
		sign = *s == '-' ? -1 : 1;
		s++;
	}
	if (isdigit((unsigned char) *s)) {
		n = (int) strtol(s, (char **) &s, 10);
	} else {
		n = 1;
		if (*s != 'n' && *s != 'N')
			return FALSE;
	}
	if (*s == 'n' || *s == 'N') {
		*a = sign * n;
		s++;
		while (isspace((unsigned char) *s))
			s++;
		if (*s == '+' || *s == '-') {
			int bs = *s == '-' ? -1 : 1;
			s++;
			while (isspace((unsigned char) *s))
				s++;
			*b = bs * (int) strtol(s, (char **) &s, 10);
		}
	} else {
		*b = sign * n;
	}
	return TRUE;
}

static XP_Bool
nth_matches(int a, int b, int i)
{
	if (a == 0)
		return i == b;
	return (i - b) / a >= 0 && (i - b) % a == 0;
}

static XP_Bool sel_match_list(const char *sel, dom_node *n, dom_node *scope,
							  XP_Bool *bad);
static XP_Bool sel_match_complex(const char *start, const char *end,
								 dom_node *n, dom_node *scope);

/* The text of a (...) argument; P is just past the '('.  Allocated. */
static char *
sel_paren(qjs_Sel *p)
{
	int depth = 1;
	const char *b = p->s;
	char *r;

	while (*p->s && depth) {
		if (*p->s == '(')
			depth++;
		else if (*p->s == ')')
			depth--;
		else if (*p->s == '"' || *p->s == '\'') {
			char q = *p->s++;
			while (*p->s && *p->s != q)
				p->s++;
			if (!*p->s)
				break;
		}
		if (depth)
			p->s++;
	}
	if (*p->s != ')') {
		p->bad = TRUE;
		return NULL;
	}
	r = (char *) XP_ALLOC(p->s - b + 1);
	if (r) {
		XP_MEMCPY(r, b, p->s - b);
		r[p->s - b] = '\0';
	}
	p->s++;
	return r;
}

/* Does the compound selector at P (up to a combinator) match N?  Leaves
 * P past it.  Parses all of it even after a mismatch. */
static XP_Bool
sel_compound(qjs_Sel *p, dom_node *n, dom_node *scope)
{
	XP_Bool ok = TRUE, any = FALSE;
	char buf[256];

	if (*p->s == '*') {
		p->s++;
		any = TRUE;
	} else if (sel_ident_char((unsigned char) *p->s)) {
		dom_string *nm = NULL;
		size_t len = sel_ident(p, buf, sizeof buf);
		any = TRUE;
		if (dom_node_get_node_name(n, &nm) == DOM_NO_ERR && nm) {
			ok = dom_string_byte_length(nm) == len &&
				 !strncasecmp(dom_string_data(nm), buf, len);
			dom_string_unref(nm);
		} else {
			ok = FALSE;
		}
	}
	for (;;) {
		char c = *p->s;
		if (c == '#') {
			dom_string *v;
			size_t len;
			p->s++;
			len = sel_ident(p, buf, sizeof buf);
			any = TRUE;
			if (ok) {
				v = el_attr(n, "id");
				ok = v && dom_string_byte_length(v) == len &&
					 !strncmp(dom_string_data(v), buf, len);
				if (v)
					dom_string_unref(v);
			}
		} else if (c == '.') {
			dom_string *v;
			size_t len;
			p->s++;
			len = sel_ident(p, buf, sizeof buf);
			if (!len)
				p->bad = TRUE;
			any = TRUE;
			if (ok) {
				v = el_attr(n, "class");
				ok = v && word_in(dom_string_data(v), dom_string_byte_length(v),
								  buf, len, FALSE);
				if (v)
					dom_string_unref(v);
			}
		} else if (c == '[') {
			char name[128], op = 0, val[512];
			size_t vl = 0;
			XP_Bool ci = FALSE;
			dom_string *v;

			p->s++;
			sel_ws(p);
			sel_ident(p, name, sizeof name);
			sel_ws(p);
			if (*p->s == '=') {
				op = '=';
				p->s++;
			} else if (*p->s && strchr("~|^$*", *p->s) && p->s[1] == '=') {
				op = *p->s;
				p->s += 2;
			}
			if (op) {
				sel_ws(p);
				if (*p->s == '"' || *p->s == '\'') {
					char q = *p->s++;
					while (*p->s && *p->s != q) {
						if (*p->s == '\\' && p->s[1])
							p->s++;
						if (vl < sizeof val - 1)
							val[vl++] = *p->s;
						p->s++;
					}
					if (*p->s == q)
						p->s++;
					else
						p->bad = TRUE;
					val[vl] = '\0';
				} else {
					vl = sel_ident(p, val, sizeof val);
				}
				sel_ws(p);
				if (*p->s == 'i' || *p->s == 'I') {
					ci = TRUE;
					p->s++;
					sel_ws(p);
				} else if (*p->s == 's' || *p->s == 'S') {
					p->s++;
					sel_ws(p);
				}
			}
			if (*p->s != ']') {
				p->bad = TRUE;
				return FALSE;
			}
			p->s++;
			any = TRUE;
			if (ok) {
				size_t i;
				for (i = 0; name[i]; i++)
					name[i] = tolower((unsigned char) name[i]);
				v = el_attr(n, name);
				if (!v) {
					ok = FALSE;
				} else if (op) {
					const char *d = dom_string_data(v);
					size_t dl = dom_string_byte_length(v);
					int (*cmp)(const char *, const char *, size_t) =
						ci ? strncasecmp : strncmp;
					switch (op) {
					case '=':
						ok = dl == vl && !cmp(d, val, vl);
						break;
					case '~':
						ok = vl && word_in(d, dl, val, vl, ci);
						break;
					case '|':
						ok = (dl == vl && !cmp(d, val, vl)) ||
							 (dl > vl && !cmp(d, val, vl) && d[vl] == '-');
						break;
					case '^':
						ok = vl && dl >= vl && !cmp(d, val, vl);
						break;
					case '$':
						ok = vl && dl >= vl && !cmp(d + dl - vl, val, vl);
						break;
					case '*': {
						size_t i;
						ok = FALSE;
						for (i = 0; vl && i + vl <= dl; i++)
							if (!cmp(d + i, val, vl)) {
								ok = TRUE;
								break;
							}
						break;
					}
					}
				}
				if (v)
					dom_string_unref(v);
			}
		} else if (c == ':') {
			char *arg = NULL;
			p->s++;
			if (*p->s == ':') {		/* pseudo-elements: never elements */
				p->s++;
				sel_ident(p, buf, sizeof buf);
				ok = FALSE;
				any = TRUE;
				continue;
			}
			sel_ident(p, buf, sizeof buf);
			if (*p->s == '(') {
				p->s++;
				arg = sel_paren(p);
				if (!arg)
					return FALSE;
			}
			any = TRUE;
			if (ok) {
				XP_Bool r = FALSE, bad = FALSE;
				int a, b;
				if (!strcasecmp(buf, "first-child"))
					r = el_index(n, FALSE, FALSE) == 1;
				else if (!strcasecmp(buf, "last-child"))
					r = el_index(n, TRUE, FALSE) == 1;
				else if (!strcasecmp(buf, "only-child"))
					r = el_index(n, FALSE, FALSE) == 1 && el_index(n, TRUE, FALSE) == 1;
				else if (!strcasecmp(buf, "first-of-type"))
					r = el_index(n, FALSE, TRUE) == 1;
				else if (!strcasecmp(buf, "last-of-type"))
					r = el_index(n, TRUE, TRUE) == 1;
				else if (!strcasecmp(buf, "only-of-type"))
					r = el_index(n, FALSE, TRUE) == 1 && el_index(n, TRUE, TRUE) == 1;
				else if (arg && !strcasecmp(buf, "nth-child"))
					r = sel_nth(arg, &a, &b) && nth_matches(a, b, el_index(n, FALSE, FALSE));
				else if (arg && !strcasecmp(buf, "nth-last-child"))
					r = sel_nth(arg, &a, &b) && nth_matches(a, b, el_index(n, TRUE, FALSE));
				else if (arg && !strcasecmp(buf, "nth-of-type"))
					r = sel_nth(arg, &a, &b) && nth_matches(a, b, el_index(n, FALSE, TRUE));
				else if (arg && !strcasecmp(buf, "nth-last-of-type"))
					r = sel_nth(arg, &a, &b) && nth_matches(a, b, el_index(n, TRUE, TRUE));
				else if (arg && !strcasecmp(buf, "not"))
					r = !sel_match_list(arg, n, scope, &bad);
				else if (arg && (!strcasecmp(buf, "is") || !strcasecmp(buf, "where") ||
								 !strcasecmp(buf, "matches") ||
								 !strcasecmp(buf, "-webkit-any") ||
								 !strcasecmp(buf, "-moz-any")))
					r = sel_match_list(arg, n, scope, &bad);
				else if (arg && !strcasecmp(buf, "has")) {
					/* any descendant matching */
					dom_node *d = NULL, *next;
					const char *a2 = arg;
					while (*a2 == ' ' || *a2 == '>')
						a2++;
					dom_node_get_first_child(n, &d);
					while (d && !r) {
						dom_node_type t;
						if (dom_node_get_node_type(d, &t) == DOM_NO_ERR &&
							t == DOM_ELEMENT_NODE &&
							sel_match_list(a2, d, n, &bad))
							r = TRUE;
						next = NULL;
						dom_node_get_first_child(d, &next);
						if (!next) {
							dom_node *up = (dom_node *) dom_node_ref(d);
							while (up && up != n) {
								dom_node *parent = NULL;
								if (dom_node_get_next_sibling(up, &next) == DOM_NO_ERR && next)
									break;
								dom_node_get_parent_node(up, &parent);
								dom_node_unref(up);
								up = parent;
							}
							if (up)
								dom_node_unref(up);
							if (up == n)
								next = NULL;
						}
						dom_node_unref(d);
						d = next;
					}
					if (d)
						dom_node_unref(d);
				} else if (!strcasecmp(buf, "root")) {
					dom_node *par = NULL;
					dom_node_type t = DOM_DOCUMENT_NODE;
					dom_node_get_parent_node(n, &par);
					if (par) {
						dom_node_get_node_type(par, &t);
						dom_node_unref(par);
					}
					r = t == DOM_DOCUMENT_NODE;
				} else if (!strcasecmp(buf, "scope"))
					r = scope ? n == scope : FALSE;
				else if (!strcasecmp(buf, "empty")) {
					dom_node *c2 = NULL;
					dom_node_get_first_child(n, &c2);
					r = !c2;
					if (c2)
						dom_node_unref(c2);
				} else if (!strcasecmp(buf, "checked")) {
					dom_string *v = el_attr(n, "checked");
					dom_string *v2 = v ? NULL : el_attr(n, "selected");
					r = v || v2;
					if (v)
						dom_string_unref(v);
					if (v2)
						dom_string_unref(v2);
				} else if (!strcasecmp(buf, "disabled") || !strcasecmp(buf, "enabled")) {
					dom_string *v = el_attr(n, "disabled");
					r = (v != NULL) == (buf[0] == 'd' || buf[0] == 'D');
					if (v)
						dom_string_unref(v);
				} else if (!strcasecmp(buf, "link") || !strcasecmp(buf, "any-link")) {
					dom_string *v = el_attr(n, "href");
					r = v != NULL;
					if (v)
						dom_string_unref(v);
				} else if (!strcasecmp(buf, "defined"))
					r = TRUE;
				else
					r = FALSE;	/* :hover, :focus, :visited, unknown */
				ok = r && !bad;
			}
			XP_FREEIF(arg);
		} else {
			break;
		}
	}
	if (!any)
		p->bad = TRUE;
	return ok;
}

/* Does the complex selector in [start, end) match N?  Right to left: find
 * the last compound, match it, then the combinator before it. */
static XP_Bool
sel_match_complex(const char *start, const char *end, dom_node *n,
				  dom_node *scope)
{
	const char *p, *last;
	char comb = ' ';
	qjs_Sel s;
	XP_Bool ok;
	int depth = 0;
	char q = 0;

	/* trim */
	while (start < end && isspace((unsigned char) *start))
		start++;
	while (end > start && isspace((unsigned char) end[-1]))
		end--;
	if (start == end)
		return FALSE;
	/* find where the last compound starts: after the last top-level
	 * combinator (space, >, +, ~) */
	last = start;
	for (p = start; p < end; p++) {
		if (q) {
			if (*p == '\\' && p + 1 < end)
				p++;
			else if (*p == q)
				q = 0;
			continue;
		}
		if (*p == '"' || *p == '\'')
			q = *p;
		else if (*p == '(' || *p == '[')
			depth++;
		else if (*p == ')' || *p == ']')
			depth--;
		else if (*p == '\\' && p + 1 < end)
			p++;
		else if (!depth && (isspace((unsigned char) *p) || *p == '>' ||
							*p == '+' || *p == '~'))
			last = p + 1;
	}
	/* the compound, as its own string */
	{
		char tmp[512];
		size_t len = end - last;
		if (len >= sizeof tmp)
			return FALSE;
		XP_MEMCPY(tmp, last, len);
		tmp[len] = '\0';
		s.s = tmp;
		s.bad = FALSE;
		ok = sel_compound(&s, n, scope);
		if (s.bad || *s.s)
			return FALSE;
	}
	if (!ok)
		return FALSE;
	/* the combinator before it */
	p = last;
	while (p > start && (isspace((unsigned char) p[-1]) || p[-1] == '>' ||
						 p[-1] == '+' || p[-1] == '~')) {
		if (p[-1] != ' ' && !isspace((unsigned char) p[-1]))
			comb = p[-1];
		p--;
	}
	if (p == start)
		return TRUE;
	switch (comb) {
	case '>': {
		dom_node *par = el_parent(n);
		if (!par)
			return FALSE;
		ok = sel_match_complex(start, p, par, scope);
		dom_node_unref(par);
		return ok;
	}
	case '+': {
		dom_node *sib = el_sibling(n, FALSE);
		if (!sib)
			return FALSE;
		ok = sel_match_complex(start, p, sib, scope);
		dom_node_unref(sib);
		return ok;
	}
	case '~': {
		dom_node *sib = el_sibling(n, FALSE), *t;
		while (sib) {
			if (sel_match_complex(start, p, sib, scope)) {
				dom_node_unref(sib);
				return TRUE;
			}
			t = el_sibling(sib, FALSE);
			dom_node_unref(sib);
			sib = t;
		}
		return FALSE;
	}
	default: {
		dom_node *anc = el_parent(n), *t;
		while (anc) {
			if (sel_match_complex(start, p, anc, scope)) {
				dom_node_unref(anc);
				return TRUE;
			}
			t = el_parent(anc);
			dom_node_unref(anc);
			anc = t;
		}
		return FALSE;
	}
	}
}

/* A comma-separated list. */
static XP_Bool
sel_match_list(const char *sel, dom_node *n, dom_node *scope, XP_Bool *bad)
{
	const char *p = sel, *b = sel;
	int depth = 0;
	char q = 0;

	for (;; p++) {
		if (q) {
			if (*p == '\\' && p[1])
				p++;
			else if (*p == q)
				q = 0;
			else if (!*p)
				break;
			continue;
		}
		if (*p == '"' || *p == '\'')
			q = *p;
		else if (*p == '(' || *p == '[')
			depth++;
		else if (*p == ')' || *p == ']')
			depth--;
		else if (*p == '\\' && p[1])
			p++;
		else if (!*p || (*p == ',' && !depth)) {
			if (sel_match_complex(b, p, n, scope))
				return TRUE;
			if (!*p)
				break;
			b = p + 1;
		}
	}
	return FALSE;
}

/* Is SEL readable?  (Matching a bad selector would throw in a browser.) */
static XP_Bool
sel_valid(const char *sel)
{
	const char *p;
	int depth = 0;

	for (p = sel; *p; p++) {
		if (*p == '(' || *p == '[')
			depth++;
		else if (*p == ')' || *p == ']')
			depth--;
		if (depth < 0)
			return FALSE;
	}
	for (p = sel; *p && isspace((unsigned char) *p); p++)
		;
	return depth == 0 && *p && *p != ',';
}

/* select(root, selector, first): the matching elements under ROOT, in
 * document order (the first only if FIRST: null if none). */
static JSValue
dom_select_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *sel;
	XP_Bool first = argc > 2 && JS_ToBool(cx, argv[2]), bad = FALSE;
	JSValue a = JS_UNDEFINED;
	uint32_t count = 0;
	dom_node *n = NULL, *next;
	NODE_ARG(root, 0);

	if (argc < 2 || !(sel = JS_ToCString(cx, argv[1])))
		return JS_EXCEPTION;
	if (!sel_valid(sel)) {
		JS_ThrowSyntaxError(cx, "'%s' is not a valid selector", sel);
		JS_FreeCString(cx, sel);
		return JS_EXCEPTION;
	}
	if (!first) {
		a = JS_NewArray(cx);
		if (JS_IsException(a)) {
			JS_FreeCString(cx, sel);
			return a;
		}
	}
	dom_node_get_first_child(root, &n);
	while (n) {
		dom_node_type t;
		if (dom_node_get_node_type(n, &t) == DOM_NO_ERR && t == DOM_ELEMENT_NODE &&
			sel_match_list(sel, n, root, &bad)) {
			if (first) {
				JSValue v = qjs_wrap(cx, n);
				dom_node_unref(n);
				JS_FreeCString(cx, sel);
				return v;
			}
			JS_SetPropertyUint32(cx, a, count++, qjs_wrap(cx, n));
		}
		/* next in document order, inside ROOT */
		next = NULL;
		dom_node_get_first_child(n, &next);
		if (!next) {
			dom_node *up = (dom_node *) dom_node_ref(n);
			while (up && up != root) {
				dom_node *parent = NULL;
				if (dom_node_get_next_sibling(up, &next) == DOM_NO_ERR && next)
					break;
				dom_node_get_parent_node(up, &parent);
				dom_node_unref(up);
				up = parent;
			}
			if (up)
				dom_node_unref(up);
			if (up == root)
				next = NULL;
		}
		dom_node_unref(n);
		n = next;
	}
	JS_FreeCString(cx, sel);
	return first ? JS_NULL : a;
}

static JSValue
dom_matches_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *sel;
	XP_Bool r, bad = FALSE;
	NODE_ARG(n, 0);

	if (argc < 2 || !(sel = JS_ToCString(cx, argv[1])))
		return JS_EXCEPTION;
	if (!sel_valid(sel)) {
		JS_ThrowSyntaxError(cx, "'%s' is not a valid selector", sel);
		JS_FreeCString(cx, sel);
		return JS_EXCEPTION;
	}
	r = sel_match_list(sel, n, NULL, &bad);
	JS_FreeCString(cx, sel);
	return JS_NewBool(cx, r);
}

/* Elements under ROOT by tag name ("*" for all; magic 0) or by class
 * names (magic 1). */
static JSValue
dom_collect_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv,
			   int magic)
{
	const char *want;
	size_t wl;
	JSValue a;
	uint32_t count = 0;
	dom_node *n = NULL, *next;
	XP_Bool all;
	NODE_ARG(root, 0);

	if (argc < 2 || !(want = JS_ToCStringLen(cx, &wl, argv[1])))
		return JS_EXCEPTION;
	all = magic == 0 && !strcmp(want, "*");
	a = JS_NewArray(cx);
	dom_node_get_first_child(root, &n);
	while (n) {
		dom_node_type t;
		if (dom_node_get_node_type(n, &t) == DOM_NO_ERR && t == DOM_ELEMENT_NODE) {
			XP_Bool hit = all;
			if (!hit && magic == 0) {
				dom_string *nm = NULL;
				if (dom_node_get_node_name(n, &nm) == DOM_NO_ERR && nm) {
					hit = dom_string_byte_length(nm) == wl &&
						  !strncasecmp(dom_string_data(nm), want, wl);
					dom_string_unref(nm);
				}
			} else if (!hit) {
				dom_string *v = el_attr(n, "class");
				if (v) {
					/* every class asked for */
					size_t i = 0;
					hit = TRUE;
					while (i < wl && hit) {
						size_t j;
						while (i < wl && isspace((unsigned char) want[i]))
							i++;
						j = i;
						while (j < wl && !isspace((unsigned char) want[j]))
							j++;
						if (j > i)
							hit = word_in(dom_string_data(v),
										  dom_string_byte_length(v),
										  want + i, j - i, FALSE);
						i = j;
					}
					dom_string_unref(v);
				}
			}
			if (hit)
				JS_SetPropertyUint32(cx, a, count++, qjs_wrap(cx, n));
		}
		next = NULL;
		dom_node_get_first_child(n, &next);
		if (!next) {
			dom_node *up = (dom_node *) dom_node_ref(n);
			while (up && up != root) {
				dom_node *parent = NULL;
				if (dom_node_get_next_sibling(up, &next) == DOM_NO_ERR && next)
					break;
				dom_node_get_parent_node(up, &parent);
				dom_node_unref(up);
				up = parent;
			}
			if (up)
				dom_node_unref(up);
			if (up == root)
				next = NULL;
		}
		dom_node_unref(n);
		n = next;
	}
	JS_FreeCString(cx, want);
	return a;
}

/* ---- HTML in and out ------------------------------------------------------ */

typedef struct {
	char *buf;
	size_t len, cap;
	XP_Bool failed;
	XP_Bool for_layout;		/* scripts left out (they have run), and */
	qjs_Dom *dom;			/* form fields as they are now */
	lo_FormElementOptionData *opts;	/* in a select: its options' state */
	int32 nopts, opt_index;
} qjs_Buf;

static void
buf_add(qjs_Buf *b, const char *s, size_t n)
{
	if (b->failed)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
		char *p;
		while (cap < b->len + n + 1)
			cap *= 2;
		p = (char *) XP_REALLOC(b->buf, cap);
		if (!p) {
			b->failed = TRUE;
			return;
		}
		b->buf = p;
		b->cap = cap;
	}
	XP_MEMCPY(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
}

static void
buf_str(qjs_Buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

static void
buf_escaped(qjs_Buf *b, const char *s, size_t n, XP_Bool attr)
{
	size_t i, from = 0;

	for (i = 0; i < n; i++) {
		const char *rep = NULL;
		switch (s[i]) {
		case '&': rep = "&amp;"; break;
		case '<': rep = attr ? NULL : "&lt;"; break;
		case '>': rep = attr ? NULL : "&gt;"; break;
		case '"': rep = attr ? "&quot;" : NULL; break;
		case '\xc2':	/* U+00A0 */
			if (i + 1 < n && s[i + 1] == '\xa0') {
				buf_add(b, s + from, i - from);
				buf_str(b, "&nbsp;");
				from = ++i + 1;
			}
			continue;
		}
		if (rep) {
			buf_add(b, s + from, i - from);
			buf_str(b, rep);
			from = i + 1;
		}
	}
	buf_add(b, s + from, n - from);
}

static void qjs_serialise(qjs_Buf *b, dom_node *n, XP_Bool raw_text);

static void
qjs_serialise_children(qjs_Buf *b, dom_node *n, XP_Bool raw_text)
{
	dom_node *c = NULL, *next;

	dom_node_get_first_child(n, &c);
	while (c) {
		qjs_serialise(b, c, raw_text);
		next = NULL;
		dom_node_get_next_sibling(c, &next);
		dom_node_unref(c);
		c = next;
	}
}

static void
qjs_serialise(qjs_Buf *b, dom_node *n, XP_Bool raw_text)
{
	dom_node_type t;
	dom_string *s = NULL;

	if (dom_node_get_node_type(n, &t) != DOM_NO_ERR)
		return;
	switch (t) {
	case DOM_ELEMENT_NODE: {
		char name[64];
		size_t i, len;
		dom_namednodemap *map = NULL;
		dom_ulong na = 0, k;
		XP_Bool raw;

		if (dom_node_get_node_name(n, &s) != DOM_NO_ERR || !s)
			return;
		len = dom_string_byte_length(s);
		if (len >= sizeof name)
			len = sizeof name - 1;
		for (i = 0; i < len; i++)
			name[i] = tolower((unsigned char) dom_string_data(s)[i]);
		name[len] = '\0';
		dom_string_unref(s);
		LO_FormElementStruct *fe = NULL;
		LO_FormElementData *fd = NULL;
		const char *live = NULL;	/* the attribute the field's state replaces */
		char *live_value = NULL;
		int live_on = -1;			/* checked / selected: 1, 0; -1 none */

		if (b->for_layout && b->dom &&
			(!strcmp(name, "input") || !strcmp(name, "textarea") ||
			 !strcmp(name, "select")) &&
			(fe = qjs_field_of(b->dom, n)) != NULL && (fd = fe->element_data) &&
			b->dom->context) {
			FE_GetFormElementValue(b->dom->context, fe, FALSE);
			switch (fd->type) {
			case FORM_TYPE_TEXT:
			case FORM_TYPE_PASSWORD:
				live = "value";
				if (fd->ele_text.current_text)
					live_value = qjs_FromDocumentCharset(b->dom->context,
						(char *) fd->ele_text.current_text,
						XP_STRLEN((char *) fd->ele_text.current_text));
				break;
			case FORM_TYPE_RADIO:
			case FORM_TYPE_CHECKBOX:
				live = "checked";
				live_on = fd->ele_toggle.toggled ? 1 : 0;
				break;
			default:
				break;
			}
		}
		if (b->for_layout && b->opts && !strcmp(name, "option")) {
			live = "selected";
			live_on = b->opt_index < b->nopts && b->opts[b->opt_index].selected;
			b->opt_index++;
		}
		buf_str(b, "<");
		buf_str(b, name);
		if ((!b->for_layout || strcmp(name, "script")) &&
			dom_node_get_attributes(n, &map) == DOM_NO_ERR && map) {
			dom_namednodemap_get_length(map, &na);
			for (k = 0; k < na; k++) {
				dom_attr *at = NULL;
				dom_string *an = NULL, *av = NULL;
				if (dom_namednodemap_item(map, k, (dom_node **) &at) != DOM_NO_ERR || !at)
					continue;
				dom_attr_get_name(at, &an);
				dom_attr_get_value(at, &av);
				if (an && live && !strcasecmp(dom_string_data(an), live)) {
					dom_string_unref(an);
					an = NULL;		/* written below, as it is now */
				}
				if (an) {
					buf_str(b, " ");
					buf_add(b, dom_string_data(an), dom_string_byte_length(an));
					buf_str(b, "=\"");
					if (av)
						buf_escaped(b, dom_string_data(av), dom_string_byte_length(av), TRUE);
					buf_str(b, "\"");
				}
				if (an)
					dom_string_unref(an);
				if (av)
					dom_string_unref(av);
				dom_node_unref(at);
			}
			dom_namednodemap_unref(map);
		}
		if (live && live_value) {
			buf_str(b, " ");
			buf_str(b, live);
			buf_str(b, "=\"");
			buf_escaped(b, live_value, strlen(live_value), TRUE);
			buf_str(b, "\"");
		} else if (live && live_on == 1) {
			buf_str(b, " ");
			buf_str(b, live);
		}
		XP_FREEIF(live_value);
		buf_str(b, ">");
		if (qjs_in_list(name, qjs_void_tags))
			return;
		if (b->for_layout && !strcmp(name, "script")) {
			buf_str(b, "</script>");
			return;
		}
		if (fd && fd->type == FORM_TYPE_TEXTAREA) {
			char *t = fd->ele_textarea.current_text ?
				qjs_FromDocumentCharset(b->dom->context,
					(char *) fd->ele_textarea.current_text,
					XP_STRLEN((char *) fd->ele_textarea.current_text)) : NULL;
			if (t) {
				buf_escaped(b, t, strlen(t), FALSE);
				XP_FREE(t);
			}
			buf_str(b, "</textarea>");
			return;
		}
		if (fd && (fd->type == FORM_TYPE_SELECT_ONE ||
				   fd->type == FORM_TYPE_SELECT_MULT)) {
			lo_FormElementOptionData *so = b->opts;
			int32 sn = b->nopts, si = b->opt_index;
			b->opts = (lo_FormElementOptionData *) fd->ele_select.options;
			b->nopts = fd->ele_select.option_cnt;
			b->opt_index = 0;
			qjs_serialise_children(b, n, FALSE);
			b->opts = so;
			b->nopts = sn;
			b->opt_index = si;
			buf_str(b, "</select>");
			return;
		}
		raw = !strcmp(name, "script") || !strcmp(name, "style") ||
			  !strcmp(name, "xmp") || !strcmp(name, "plaintext") ||
			  !strcmp(name, "noscript");
		qjs_serialise_children(b, n, raw);
		buf_str(b, "</");
		buf_str(b, name);
		buf_str(b, ">");
		return;
	}
	case DOM_TEXT_NODE:
	case DOM_CDATA_SECTION_NODE:
		if (dom_characterdata_get_data((dom_characterdata *) n, &s) == DOM_NO_ERR && s) {
			if (raw_text)
				buf_add(b, dom_string_data(s), dom_string_byte_length(s));
			else
				buf_escaped(b, dom_string_data(s), dom_string_byte_length(s), FALSE);
			dom_string_unref(s);
		}
		return;
	case DOM_COMMENT_NODE:
		if (dom_characterdata_get_data((dom_characterdata *) n, &s) == DOM_NO_ERR && s) {
			buf_str(b, "<!--");
			buf_add(b, dom_string_data(s), dom_string_byte_length(s));
			buf_str(b, "-->");
			dom_string_unref(s);
		}
		return;
	case DOM_DOCUMENT_NODE:
	case DOM_DOCUMENT_FRAGMENT_NODE:
		qjs_serialise_children(b, n, FALSE);
		return;
	default:
		return;
	}
}

/* Serialise N: html(n, outer) */
static JSValue
dom_html_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	qjs_Buf b = { 0 };
	JSValue v;
	NODE_ARG(n, 0);

	if (argc > 1 && JS_ToBool(cx, argv[1])) {
		qjs_serialise(&b, n, FALSE);
	} else {
		dom_string *nm = NULL;
		XP_Bool raw = FALSE;
		if (dom_node_get_node_name(n, &nm) == DOM_NO_ERR && nm) {
			raw = !strcasecmp(dom_string_data(nm), "SCRIPT") ||
				  !strcasecmp(dom_string_data(nm), "STYLE");
			dom_string_unref(nm);
		}
		qjs_serialise_children(&b, n, raw);
	}
	if (b.failed) {
		XP_FREEIF(b.buf);
		return JS_ThrowOutOfMemory(cx);
	}
	v = JS_NewStringLen(cx, b.buf ? b.buf : "", b.len);
	XP_FREEIF(b.buf);
	return v;
}

static void
qjs_hubbub_msg(uint32_t severity, void *ctx, const char *msg, ...)
{
}

/* The fragment parser gives html > head, body around what it parsed:
 * take the children of those two out, in order, and drop the rest. */
static void
qjs_unwrap_fragment(dom_document_fragment *frag)
{
	dom_node *root = NULL, *part = NULL, *next, *c, *cnext, *r;
	dom_string *nm = NULL;

	if (dom_node_get_first_child(frag, &root) != DOM_NO_ERR || !root)
		return;
	if (dom_node_get_node_name(root, &nm) != DOM_NO_ERR || !nm ||
		strcasecmp(dom_string_data(nm), "HTML")) {
		if (nm)
			dom_string_unref(nm);
		dom_node_unref(root);
		return;
	}
	dom_string_unref(nm);
	dom_node_get_first_child(root, &part);
	while (part) {
		next = NULL;
		dom_node_get_next_sibling(part, &next);
		c = NULL;
		dom_node_get_first_child(part, &c);
		while (c) {
			cnext = NULL;
			dom_node_get_next_sibling(c, &cnext);
			r = NULL;
			if (dom_node_insert_before(frag, c, root, &r) == DOM_NO_ERR && r)
				dom_node_unref(r);
			dom_node_unref(c);
			c = cnext;
		}
		dom_node_unref(part);
		part = next;
	}
	r = NULL;
	if (dom_node_remove_child(frag, root, &r) == DOM_NO_ERR && r)
		dom_node_unref(r);
	dom_node_unref(root);
}

/* parse(html): a fragment of this document holding HTML's nodes */
static JSValue
dom_parse_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	dom_document *doc = qjs_cx_doc(cx);
	dom_hubbub_parser_params params;
	dom_hubbub_parser *parser = NULL;
	dom_document_fragment *frag = NULL;
	const char *html;
	size_t len;

	if (!doc || argc < 1 || !(html = JS_ToCStringLen(cx, &len, argv[0])))
		return JS_EXCEPTION;
	memset(&params, 0, sizeof params);
	params.enc = "UTF-8";
	params.fix_enc = true;
	params.enable_script = false;
	params.msg = qjs_hubbub_msg;
	if (dom_hubbub_fragment_parser_create(&params, doc, &parser, &frag) !=
		DOM_HUBBUB_OK || !parser) {
		JS_FreeCString(cx, html);
		return JS_ThrowInternalError(cx, "cannot parse HTML");
	}
	dom_hubbub_parser_parse_chunk(parser, (const uint8_t *) html, len);
	dom_hubbub_parser_completed(parser);
	dom_hubbub_parser_destroy(parser);
	JS_FreeCString(cx, html);
	qjs_unwrap_fragment(frag);
	return qjs_wrap_ret(cx, DOM_NO_ERR, (dom_node *) frag);
}

/* mutated(): did a script change the tree since the last call? */
static JSValue
dom_mutated_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	qjs_Dom *dom = qjs_cx_dom(cx);
	XP_Bool m = dom && dom->mutated;

	if (dom)
		dom->mutated = FALSE;
	return JS_NewBool(cx, m);
}

/* protos(table): the prototypes, by node type ("1", "3" ...; "0" for any
 * other) and by element tag name in upper case ("DIV" ...) */
static JSValue
dom_protos_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	qjs_Dom *dom = qjs_cx_dom(cx);

	if (!dom || argc < 1)
		return JS_UNDEFINED;
	if (dom->cx && dom->cx != cx)
		qjs_dom_free_wrappers(dom);
	dom->cx = cx;
	JS_FreeValue(cx, dom->protos);
	dom->protos = JS_DupValue(cx, argv[0]);
	return JS_UNDEFINED;
}

/* field(n): the live state of form field N -- {value, checked, selected:
 * [booleans]} -- or null when layout has none for it */
static JSValue
dom_field_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	qjs_Dom *dom = qjs_cx_dom(cx);
	LO_FormElementStruct *fe;
	LO_FormElementData *d;
	JSValue o;
	NODE_ARG(n, 0);

	if (!(fe = qjs_field_of(dom, n)) || !(d = fe->element_data) || !dom->context)
		return JS_NULL;
	FE_GetFormElementValue(dom->context, fe, FALSE);
	o = JS_NewObject(cx);
	switch (d->type) {
	case FORM_TYPE_TEXT:
	case FORM_TYPE_PASSWORD:
	case FORM_TYPE_FILE:
	case FORM_TYPE_TEXTAREA: {
		char *t = (char *) (d->type == FORM_TYPE_TEXTAREA ?
							d->ele_textarea.current_text : d->ele_text.current_text);
		char *u = t ? qjs_FromDocumentCharset(dom->context, t, XP_STRLEN(t)) : NULL;
		JS_SetPropertyStr(cx, o, "value", JS_NewString(cx, u ? u : ""));
		XP_FREEIF(u);
		break;
	}
	case FORM_TYPE_RADIO:
	case FORM_TYPE_CHECKBOX:
		JS_SetPropertyStr(cx, o, "checked", JS_NewBool(cx, d->ele_toggle.toggled));
		break;
	case FORM_TYPE_SELECT_ONE:
	case FORM_TYPE_SELECT_MULT: {
		lo_FormElementOptionData *opt = (lo_FormElementOptionData *) d->ele_select.options;
		JSValue a = JS_NewArray(cx);
		int32 i;
		for (i = 0; opt && i < d->ele_select.option_cnt; i++)
			JS_SetPropertyUint32(cx, a, i, JS_NewBool(cx, opt[i].selected));
		JS_SetPropertyStr(cx, o, "selected", a);
		break;
	}
	default:
		JS_FreeValue(cx, o);
		return JS_NULL;
	}
	return o;
}

/* setField(n, what, v): set form field N's "value", "checked" or
 * "selected" (an option index); FALSE when layout has no field for N */
static JSValue
dom_set_field_fn(JSContext *cx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	qjs_Dom *dom = qjs_cx_dom(cx);
	LO_FormElementStruct *fe;
	LO_FormElementData *d;
	const char *what;
	NODE_ARG(n, 0);

	if (argc < 3 || !(fe = qjs_field_of(dom, n)) || !(d = fe->element_data) ||
		!dom->context)
		return JS_FALSE;
	if (!(what = JS_ToCString(cx, argv[1])))
		return JS_EXCEPTION;
	if (!strcmp(what, "value") &&
		(d->type == FORM_TYPE_TEXT || d->type == FORM_TYPE_PASSWORD ||
		 d->type == FORM_TYPE_TEXTAREA)) {
		size_t len;
		const char *v = JS_ToCStringLen(cx, &len, argv[2]);
		char *t = v ? qjs_ToDocumentCharset(dom->context, v, len, NULL) : NULL;
		PA_Block *slot = d->type == FORM_TYPE_TEXTAREA ?
			&d->ele_textarea.current_text : &d->ele_text.current_text;
		if (v)
			JS_FreeCString(cx, v);
		if (t) {
			XP_FREEIF(*slot);
			*slot = (PA_Block) t;
			FE_ChangeInputElement(dom->context, (LO_Element *) fe);
		}
	} else if (!strcmp(what, "checked") &&
			   (d->type == FORM_TYPE_RADIO || d->type == FORM_TYPE_CHECKBOX)) {
		d->ele_toggle.toggled = JS_ToBool(cx, argv[2]);
		FE_SetFormElementToggle(dom->context, fe, d->ele_toggle.toggled);
	} else if (!strcmp(what, "selected") &&
			   (d->type == FORM_TYPE_SELECT_ONE || d->type == FORM_TYPE_SELECT_MULT)) {
		lo_FormElementOptionData *opt = (lo_FormElementOptionData *) d->ele_select.options;
		int32 k = -1, i;
		JS_ToInt32(cx, &k, argv[2]);
		for (i = 0; opt && i < d->ele_select.option_cnt; i++)
			if (d->type == FORM_TYPE_SELECT_ONE || i == k)
				opt[i].selected = i == k;
		FE_ChangeInputElement(dom->context, (LO_Element *) fe);
	} else {
		JS_FreeCString(cx, what);
		return JS_FALSE;
	}
	JS_FreeCString(cx, what);
	return JS_TRUE;
}

static const JSCFunctionListEntry qjs_dom_functions[] = {
	JS_CFUNC_DEF("document", 0, dom_document_fn),
	JS_CFUNC_DEF("protos", 1, dom_protos_fn),
	JS_CFUNC_DEF("type", 1, dom_type_fn),
	JS_CFUNC_DEF("name", 1, dom_name_fn),
	JS_CFUNC_MAGIC_DEF("parent", 1, dom_rel_fn, 0),
	JS_CFUNC_MAGIC_DEF("first", 1, dom_rel_fn, 1),
	JS_CFUNC_MAGIC_DEF("last", 1, dom_rel_fn, 2),
	JS_CFUNC_MAGIC_DEF("next", 1, dom_rel_fn, 3),
	JS_CFUNC_MAGIC_DEF("prev", 1, dom_rel_fn, 4),
	JS_CFUNC_MAGIC_DEF("kids", 1, dom_kids_fn, 0),
	JS_CFUNC_MAGIC_DEF("elementKids", 1, dom_kids_fn, 1),
	JS_CFUNC_DEF("data", 1, dom_data_fn),
	JS_CFUNC_DEF("setData", 2, dom_set_data_fn),
	JS_CFUNC_DEF("text", 1, dom_text_fn),
	JS_CFUNC_DEF("setText", 2, dom_set_text_fn),
	JS_CFUNC_DEF("attr", 2, dom_attr_fn),
	JS_CFUNC_DEF("setAttr", 3, dom_set_attr_fn),
	JS_CFUNC_DEF("removeAttr", 2, dom_rm_attr_fn),
	JS_CFUNC_DEF("hasAttr", 2, dom_has_attr_fn),
	JS_CFUNC_DEF("attrs", 1, dom_attrs_fn),
	JS_CFUNC_MAGIC_DEF("createElement", 1, dom_create_fn, 0),
	JS_CFUNC_MAGIC_DEF("createText", 1, dom_create_fn, 1),
	JS_CFUNC_MAGIC_DEF("createComment", 1, dom_create_fn, 2),
	JS_CFUNC_MAGIC_DEF("createFragment", 0, dom_create_fn, 3),
	JS_CFUNC_DEF("insert", 3, dom_insert_fn),
	JS_CFUNC_DEF("remove", 2, dom_remove_fn),
	JS_CFUNC_DEF("replace", 3, dom_replace_fn),
	JS_CFUNC_DEF("clone", 2, dom_clone_fn),
	JS_CFUNC_DEF("byId", 1, dom_by_id_fn),
	JS_CFUNC_DEF("contains", 2, dom_contains_fn),
	JS_CFUNC_DEF("currentScript", 0, dom_current_script_fn),
	JS_CFUNC_DEF("select", 3, dom_select_fn),
	JS_CFUNC_DEF("matches", 2, dom_matches_fn),
	JS_CFUNC_MAGIC_DEF("byTag", 2, dom_collect_fn, 0),
	JS_CFUNC_MAGIC_DEF("byClass", 2, dom_collect_fn, 1),
	JS_CFUNC_DEF("html", 2, dom_html_fn),
	JS_CFUNC_DEF("parse", 1, dom_parse_fn),
	JS_CFUNC_DEF("mutated", 0, dom_mutated_fn),
	JS_CFUNC_DEF("field", 1, dom_field_fn),
	JS_CFUNC_DEF("setField", 3, dom_set_field_fn),
};

/* ---- showing what scripts changed -----------------------------------------
 *
 * After the page has loaded, a change a script makes to the tree is shown
 * by laying the document out again from the tree: the tree is written out
 * as HTML (scripts left out: they have run) into the cache under a
 * wysiwyg: URL, the history entry points there, and the window is laid
 * out again as for a resize (fe_ReLayout), which keeps the scroll position
 * and the form fields, and runs no scripts.  Layout's tags find their
 * elements again (qjs_dom_remap).  Changes made while the page loads are
 * shown once it has loaded.  Soon after a click or key; otherwise at most
 * every few seconds, and only so often, for pages that keep changing.
 */

extern void FE_RelayoutFromText(MWContext *context, const char *text, int32 len);

#define QJS_RENDER_DELAY		50		/* ms after a change */
#define QJS_RENDER_INTERVAL		3000	/* ms between unprompted ones */
#define QJS_RENDER_UNPROMPTED	12		/* that many, then only after events */


static int64
qjs_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void qjs_dom_render_timeout(void *closure);

static void
qjs_dom_schedule(MochaDecoder *decoder, qjs_Dom *dom)
{
	int64 wait = QJS_RENDER_DELAY, since;

	if (!decoder->load_event_sent || dom->render_timer || dom->in_render)
		return;
	if (!dom->user_event) {
		if (dom->renders >= QJS_RENDER_UNPROMPTED)
			return;
		since = qjs_now_ms() - dom->last_render;
		if (since < QJS_RENDER_INTERVAL)
			wait = QJS_RENDER_INTERVAL - since;
	}
	dom->render_timer = FE_SetTimeout(qjs_dom_render_timeout, decoder, (uint32) wait);
}

/* The page has loaded (its load handlers have run): show what its
 * scripts changed. */
void
qjs_DomLoaded(MochaDecoder *decoder)
{
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (getenv("QJS_DOM_TRACE"))
		qjs_Log("dom: loaded");

	if (dom && dom->mutated)
		qjs_dom_schedule(decoder, dom);
}

/* Does the page have module scripts (which run at its load event, so
 * need a context even if no other script made one)? */
XP_Bool
qjs_DomHasModules(MochaDecoder *decoder)
{
	qjs_Dom *dom = qjs_dom_of(decoder);

	return dom && dom->module_scripts;
}

/* A resize reload is over. */
void
qjs_DomRelaidOut(MochaDecoder *decoder)
{
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (!dom || !dom->in_render)
		return;
	if (getenv("QJS_DOM_TRACE"))
		qjs_Log("dom: laid out again");
	dom->in_render = FALSE;
	if (dom->mutated)
		qjs_dom_schedule(decoder, dom);
}

static void qjs_dom_render(MochaDecoder *decoder, qjs_Dom *dom);

/* The window is laid out again for a new size (fe_ReLayout): if scripts
 * changed the document, lay out the tree, not the page's source.  TRUE:
 * done. */
JSBool
LM_RelayoutFromDom(MWContext *context)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, FALSE);
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (!dom || dom->renders == 0 || !decoder->load_event_sent)
		return FALSE;
	if (dom->render_timer) {
		FE_ClearTimeout(dom->render_timer);
		dom->render_timer = NULL;
	}
	dom->renders--;		/* not one of the script's */
	qjs_dom_render(decoder, dom);
	return TRUE;
}

static void
qjs_dom_render(MochaDecoder *decoder, qjs_Dom *dom)
{
	MWContext *context = decoder->window_context;
	qjs_Buf b = { 0 };
	char *text;
	size_t len;

	b.for_layout = TRUE;
	b.dom = dom;
	qjs_serialise(&b, (dom_node *) dom->doc, FALSE);
	if (b.failed || !b.buf) {
		XP_FREEIF(b.buf);
		return;
	}
	text = qjs_ToDocumentCharset(context, b.buf, b.len, &len);
	XP_FREE(b.buf);
	if (!text)
		return;
	dom->mutated = FALSE;
	dom->user_event = FALSE;
	dom->renders++;
	dom->last_render = qjs_now_ms();
	dom->in_render = TRUE;
	if (getenv("QJS_DOM_TRACE"))
		qjs_Log("dom: laying the document out again (%ld bytes)", (long) len);
	FE_RelayoutFromText(context, text, (int32) len);
	XP_FREE(text);
}

static void
qjs_dom_render_timeout(void *closure)
{
	MochaDecoder *decoder = (MochaDecoder *) closure;
	qjs_Dom *dom = qjs_dom_of(decoder);

	if (!dom)
		return;
	dom->render_timer = NULL;
	if (getenv("QJS_DOM_TRACE"))
		qjs_Log("dom: render timeout (mutated %d, busy %d)", dom->mutated,
				decoder->window_context ? XP_IsContextBusy(decoder->window_context) : -1);
	if (!dom->mutated || !decoder->window_context)
		return;
	if (XP_IsContextBusy(decoder->window_context)) {
		/* still loading something (an image ...): later */
		dom->render_timer = FE_SetTimeout(qjs_dom_render_timeout, decoder, 500);
		return;
	}
	qjs_dom_render(decoder, dom);
}

/* ---- set up, and events from layout ------------------------------------- */

#include "qjs_dom_js.h"		/* qjs_dom_js: the DOM's JavaScript */

void
qjs_InitDom(JSContext *cx, JSValueConst ns)
{
	JSRuntime *rt = JS_GetRuntime(cx);
	JSValue dom, v;

	if (!qjs_node_class)
		JS_NewClassID(&qjs_node_class);
	if (!JS_IsRegisteredClass(rt, qjs_node_class))
		JS_NewClass(rt, qjs_node_class, &qjs_node_class_def);
	dom = JS_NewObject(cx);
	JS_SetPropertyFunctionList(cx, dom, qjs_dom_functions,
		(int) (sizeof qjs_dom_functions / sizeof qjs_dom_functions[0]));
	JS_SetPropertyStr(cx, ns, "dom", dom);
	v = JS_Eval(cx, qjs_dom_js, sizeof qjs_dom_js - 1, "<netscape dom>",
				JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(v))
		qjs_ReportException(cx);
	JS_FreeValue(cx, v);
}

/* An event from layout or the front end on ELEMENT: dispatch it to its
 * element.  FALSE: a handler cancelled it (preventDefault, return false). */
JSBool
qjs_DomEvent(MWContext *context, LO_Element *element, JSEvent *event)
{
	MochaDecoder *decoder = qjs_GetDecoder(context, FALSE);
	JSContext *cx = decoder ? decoder->js_context : NULL;
	dom_node *node = qjs_ElementOf(element);
	const char *type;
	JSValue global, fn, args[3], rv;
	JSBool ok = TRUE;

	if (!cx || !node)
		return TRUE;
	if (event->type & (EVENT_CLICK | EVENT_DBLCLICK | EVENT_MOUSEUP |
					   EVENT_KEYUP | EVENT_KEYPRESS | EVENT_CHANGE |
					   EVENT_SUBMIT | EVENT_RESET)) {
		qjs_Dom *dom = qjs_dom_of(decoder);
		if (dom)
			dom->user_event = TRUE;
	}
	switch (event->type) {
	case EVENT_CLICK:		type = "click"; break;
	case EVENT_DBLCLICK:	type = "dblclick"; break;
	case EVENT_MOUSEDOWN:	type = "mousedown"; break;
	case EVENT_MOUSEUP:		type = "mouseup"; break;
	case EVENT_MOUSEOVER:	type = "mouseover"; break;
	case EVENT_MOUSEOUT:	type = "mouseout"; break;
	case EVENT_KEYDOWN:		type = "keydown"; break;
	case EVENT_KEYUP:		type = "keyup"; break;
	case EVENT_KEYPRESS:	type = "keypress"; break;
	case EVENT_FOCUS:		type = "focus"; break;
	case EVENT_BLUR:		type = "blur"; break;
	case EVENT_SELECT:		type = "select"; break;
	case EVENT_CHANGE:		type = "change"; break;
	case EVENT_SUBMIT:		type = "submit"; break;
	case EVENT_RESET:		type = "reset"; break;
	default:
		return TRUE;
	}
	global = JS_GetGlobalObject(cx);
	fn = JS_GetPropertyStr(cx, global, "__ns_event");
	if (JS_IsFunction(cx, fn)) {
		JSValue init = JS_NewObject(cx);
		JS_SetPropertyStr(cx, init, "clientX", JS_NewInt32(cx, event->x));
		JS_SetPropertyStr(cx, init, "clientY", JS_NewInt32(cx, event->y));
		JS_SetPropertyStr(cx, init, "pageX", JS_NewInt32(cx, event->docx));
		JS_SetPropertyStr(cx, init, "pageY", JS_NewInt32(cx, event->docy));
		JS_SetPropertyStr(cx, init, "screenX", JS_NewInt32(cx, event->screenx));
		JS_SetPropertyStr(cx, init, "screenY", JS_NewInt32(cx, event->screeny));
		JS_SetPropertyStr(cx, init, "which", JS_NewInt32(cx, (int32_t) event->which));
		JS_SetPropertyStr(cx, init, "modifiers", JS_NewInt32(cx, (int32_t) event->modifiers));
		args[0] = qjs_wrap(cx, node);
		args[1] = JS_NewString(cx, type);
		args[2] = init;
		rv = JS_Call(cx, fn, global, 3, args);
		if (JS_IsException(rv))
			qjs_ReportException(cx);
		else if (JS_IsBool(rv) && !JS_ToBool(cx, rv))
			ok = FALSE;
		JS_FreeValue(cx, rv);
		JS_FreeValue(cx, args[0]);
		JS_FreeValue(cx, args[1]);
		JS_FreeValue(cx, args[2]);
		{
			JSContext *jcx;
			while (JS_ExecutePendingJob(JS_GetRuntime(cx), &jcx) > 0)
				;
		}
	}
	JS_FreeValue(cx, fn);
	JS_FreeValue(cx, global);
	return ok;
}
