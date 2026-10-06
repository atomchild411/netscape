/* -*- Mode: C; tab-width: 4 -*-
 *   nscss.c --- CSS for layout, from libcss.  See nscss.h.
 *
 * The libcss selection handler works on NSCSS_Node: one per tag pushed on
 * the style stack, holding the tag's name, id, classes and attributes, its
 * parent and the sibling closed just before it.  A node keeps its parent
 * alive and a parent keeps its children until it closes itself: when a tag
 * closes, the siblings before it are no longer needed by any selector and
 * are let go.
 *
 * libcss computes styles, but layout wants what the style sheets declared
 * for a tag, as JavaScript Style Sheets gave it: layout inherits, applies
 * HTML's own presentation and converts units itself.  So nscss_export
 * reads libcss's style for the node before it is composed with its
 * parent's:
 *   - an inherited property that no rule set is still marked inherit there;
 *   - a property that is not inherited gets its initial value, so the user
 *     agent sheet below sets each one layout reads to a marker value that
 *     no page uses: anything else came from the page;
 *   - at the bottom of the stack (no parent) inherited properties are not
 *     marked either: they are compared with the style the user agent sheet
 *     alone gives.
 */

/* libcss's headers are C99; the tree is built as gnu89. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
#define restrict __restrict
#endif

#include "xp.h"
#include "net.h"
#include "nscss.h"
#include "prprf.h"

#include <libcss/libcss.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The names layout reads (lib/layout/laystyle.h). */
#define COLOR_PROP				"color"
#define FONTSIZE_PROP			"fontSize"
#define FONTFACE_PROP			"fontFamily"
#define FONTWEIGHT_PROP			"fontWeight"
#define FONTSTYLE_PROP			"fontStyle"
#define TEXTTRANSFORM_PROP		"textTransform"
#define TEXTALIGN_PROP			"textAlign"
#define TEXTINDENT_PROP			"textIndent"
#define LINEHEIGHT_PROP			"lineHeight"
#define LISTSTYLETYPE_PROP		"listStyleType"
#define WHITESPACE_PROP			"whiteSpace"
#define LINKCOLOR_PROP			"linkColor"
#define VISITEDCOLOR_PROP		"visitedColor"
#define TEXTDECORATION_PROP		"textDecoration"
#define DISPLAY_PROP			"display"
#define TOPMARGIN_PROP			"marginTop"
#define RIGHTMARGIN_PROP		"marginRight"
#define BOTTOMMARGIN_PROP		"marginBottom"
#define LEFTMARGIN_PROP			"marginLeft"
#define TOPPADDING_PROP			"paddingTop"
#define RIGHTPADDING_PROP		"paddingRight"
#define BOTTOMPADDING_PROP		"paddingBottom"
#define LEFTPADDING_PROP		"paddingLeft"
#define WIDTH_PROP				"width"
#define MAXWIDTH_PROP			"nsImageMaxWidth"
#define IMGWIDTH_PROP			"nsImageWidth"
#define IMGHEIGHT_PROP			"nsImageHeight"
#define HEIGHT_PROP				"height"
#define MINWIDTH_PROP			"nsMinWidth"
#define MAXWIDTH_BOX_PROP		"nsMaxWidth"
#define MINHEIGHT_PROP			"nsMinHeight"
#define MAXHEIGHT_PROP			"nsMaxHeight"
#define BLOCKBOX_PROP			"nsBlockBox"	/* a block-level box */
/* Flexbox, for layout's flex tables: the container's direction, its
 * justify-content, align-items and column gap; an item's "grow shrink
 * basis" and its align-self. */
#define FLEX_PROP				"nsFlex"
/* CSS positioning, for layout's layers (layblock.c,
 * lo_SetStyleSheetLayerProperties): absolute (fixed too) or relative,
 * with its offsets and z-index */
#define POSITION_PROP			"position"
#define TOP_PROP				"top"
#define LEFT_PROP				"left"
#define ZINDEX_PROP				"zIndex"
#define FLEXJUSTIFY_PROP		"nsFlexJustify"
#define FLEXALIGN_PROP			"nsFlexAlign"
#define FLEXGAP_PROP			"nsFlexGap"
#define FLEXWRAP_PROP			"nsFlexWrap"
#define FLEXITEM_PROP			"nsFlexItem"
#define FLEXALIGNSELF_PROP		"nsFlexAlignSelf"
/* Grid layout, in a flex table too (nsFlex "grid"): the container's track
 * lists, areas, implicit tracks, auto-placement, row gap (the column gap is
 * nsFlexGap), justify-items and align-content; an item's lines ("row-start
 * / column-start / row-end / column-end") and justify-self. */
#define GRIDCOLS_PROP			"nsGridCols"
#define GRIDROWS_PROP			"nsGridRows"
#define GRIDAREAS_PROP			"nsGridAreas"
#define GRIDAUTOCOLS_PROP		"nsGridAutoCols"
#define GRIDAUTOROWS_PROP		"nsGridAutoRows"
#define GRIDFLOW_PROP			"nsGridFlow"
#define GRIDROWGAP_PROP			"nsGridRowGap"
#define GRIDJUSTIFY_PROP		"nsGridJustify"
#define GRIDALIGNCONTENT_PROP	"nsGridAlignContent"
#define GRIDITEM_PROP			"nsGridItem"
/* CSS tables on elements that are not HTML's table tags: "table", "row" or
 * "cell"; a table's border-spacing (horizontal) */
#define TABLEPART_PROP			"nsTable"
/* Generated content: an element's ::before and ::after text, and their
 * colour, background, font ("bold", "italic") and whether they are
 * blocks ("1").  Layout lays the text out at the element's start and end. */
#define GEN_BEFORE_PROP			"nsBefore"
#define GEN_AFTER_PROP			"nsAfter"
/* ::first-letter: "color|background|font|size" (empty parts unset) */
#define FIRST_LETTER_PROP		"nsFirstLetter"
#define TABLESPACING_PROP		"nsTableSpacing"
#define TABLELAYOUT_PROP		"nsTableLayout"
/* rounded corners: eight radii (top left, top right, bottom right, bottom
 * left; horizontal, vertical), px or "pN" (N% of the box); a box-shadow:
 * "dx dy blur #rrggbbaa" in px */
#define RADIUS_PROP				"nsRadius"
#define BOXSHADOW_PROP			"nsBoxShadow"
#define TEXTSHADOW_PROP			"nsTextShadow"	/* "dx dy #rrggbb" */
#define GRIDJUSTIFYSELF_PROP	"nsGridJustifySelf"
#define BORDERTOPWIDTH_PROP		"borderTopWidth"
#define BORDERRIGHTWIDTH_PROP	"borderRightWidth"
#define BORDERBOTTOMWIDTH_PROP	"borderBottomWidth"
#define BORDERLEFTWIDTH_PROP	"borderLeftWidth"
#define BORDERSTYLE_PROP		"borderStyle"
#define BORDERCOLOR_PROP		"borderColor"
#define FLOAT_PROP				"align"
#define CLEAR_PROP				"clear"
#define NS_CLEAR_AFTER_PROP		"nsClearAfter"
#define VALIGN_PROP				"verticalAlign"
#define BGCOLOR_PROP			"backgroundColor"
#define BGIMAGE_PROP			"backgroundImage"
#define BGREPEAT_PROP			"backgroundRepeat"

/* Marker values for properties that are not inherited (see above). */
#define NSCSS_MARK_LEN	"-12345.25px"	/* margins, offsets, vertical-align */
#define NSCSS_MARK_POS	"12345.25px"	/* padding, border widths, sizes */
#define NSCSS_MARK_COLOR "rgba(1,2,3,0.004)"
#define NSCSS_MARK_URL	"url(about:nscss-unset)"

static const char nscss_ua_sheet[] =
	"* {"
	" margin-top: " NSCSS_MARK_LEN "; margin-right: " NSCSS_MARK_LEN ";"
	" margin-bottom: " NSCSS_MARK_LEN "; margin-left: " NSCSS_MARK_LEN ";"
	" padding-top: " NSCSS_MARK_POS "; padding-right: " NSCSS_MARK_POS ";"
	" padding-bottom: " NSCSS_MARK_POS "; padding-left: " NSCSS_MARK_POS ";"
	" border-top-width: " NSCSS_MARK_POS "; border-right-width: " NSCSS_MARK_POS ";"
	" border-bottom-width: " NSCSS_MARK_POS "; border-left-width: " NSCSS_MARK_POS ";"
	" width: " NSCSS_MARK_POS "; height: " NSCSS_MARK_POS ";"
	" top: " NSCSS_MARK_LEN "; left: " NSCSS_MARK_LEN ";"
	" vertical-align: " NSCSS_MARK_LEN ";"
	" background-color: " NSCSS_MARK_COLOR ";"
	" background-image: " NSCSS_MARK_URL ";"
	" border-top-color: " NSCSS_MARK_COLOR ";"
	" text-decoration: blink;"
	" display: table-column-group;"
	" }\n"
	/* what HTML hides (scripts set hidden; template content is inert) */
	"[hidden], template, dialog:not([open]) { display: none; }\n"
	/* a closed details shows its summary only */
	"details:not([open]) > :not(summary) { display: none; }\n";

struct NSCSS_Doc {
	css_select_ctx	*ctx;		/* user agent sheet and the page's */
	css_select_ctx	*ua_ctx;	/* user agent sheet only */
	css_stylesheet	*ua_sheet;
	css_stylesheet **sheets;	/* the page's, to destroy */
	uint32			*sheet_hashes;	/* of their URL and text: a resize */
	int32			 n_sheets;		/* reload adds them again */
	css_unit_ctx	 unit;
	css_media		 media;
	struct nscss_var *vars;		/* custom properties (nscss_vars_*) */
	int32			 n_vars, vars_cap;
};

typedef struct {
	char *name;					/* lower case */
	char *value;
} nscss_attr;

struct NSCSS_Node {
	int32			 refcount;
	NSCSS_Doc		*doc;
	NSCSS_Node		*parent;	/* holds a reference */
	NSCSS_Node		*prev;		/* sibling closed before: holds a reference */
	NSCSS_Node		*last_child;/* holds a reference, until this closes */
	lwc_string		*name;
	lwc_string		*id;
	lwc_string	   **classes;
	uint32_t		 n_classes;
	nscss_attr		*attrs;
	int32			 n_attrs;
	css_stylesheet	*inline_style;
	void			*node_data;	/* libcss's */
	XP_Bool			 flex_row;	/* lays its children out in a row */
	uint8_t			 flex;		/* a flex container: NSCSS_FLEX_* */
};

#define NSCSS_FLEX_NONE		0
#define NSCSS_FLEX_ROW		1	/* layout's flex table (laytable.c) */
#define NSCSS_FLEX_COLUMN	2	/* likewise, in a column */
#define NSCSS_FLEX_GRID		3	/* a grid, in a flex table too */

static css_select_handler nscss_handler;

/* ---- nodes ---------------------------------------------------------- */

static NSCSS_Node *
nscss_ref(NSCSS_Node *n)
{
	if (n)
		n->refcount++;
	return n;
}

static void
nscss_unref(NSCSS_Node *n)
{
	while (n && --n->refcount == 0) {
		NSCSS_Node *parent = n->parent;
		int32 i;

		if (n->node_data)
			css_libcss_node_data_handler(&nscss_handler, CSS_NODE_DELETED,
										 n->doc, n, NULL, n->node_data);
		if (n->last_child)
			nscss_unref(n->last_child);
		if (n->prev)
			nscss_unref(n->prev);
		if (n->name)
			lwc_string_unref(n->name);
		if (n->id)
			lwc_string_unref(n->id);
		for (i = 0; i < (int32) n->n_classes; i++)
			lwc_string_unref(n->classes[i]);
		XP_FREEIF(n->classes);
		for (i = 0; i < n->n_attrs; i++) {
			XP_FREEIF(n->attrs[i].name);
			XP_FREEIF(n->attrs[i].value);
		}
		XP_FREEIF(n->attrs);
		if (n->inline_style)
			css_stylesheet_destroy(n->inline_style);
		XP_FREE(n);
		n = parent;
	}
}

static lwc_string *
nscss_intern(const char *s, size_t len)
{
	lwc_string *l = NULL;

	if (lwc_intern_string(s, len, &l) != lwc_error_ok)
		return NULL;
	return l;
}

static XP_Bool
nscss_is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Split the attribute text the parser keeps for a tag into names and
 * values.  Values are taken as written (entities are not decoded). */
static void
nscss_parse_attrs(NSCSS_Node *n, const char *p, int32 len)
{
	const char *end = p + len;
	int32 room = 0;

	while (p < end) {
		const char *name, *value = NULL;
		int32 name_len, value_len = 0, i;

		while (p < end && (nscss_is_space(*p) || *p == '/'))
			p++;
		name = p;
		while (p < end && !nscss_is_space(*p) && *p != '=' && *p != '>')
			p++;
		name_len = p - name;
		while (p < end && nscss_is_space(*p))
			p++;
		if (p < end && *p == '=') {
			p++;
			while (p < end && nscss_is_space(*p))
				p++;
			if (p < end && (*p == '"' || *p == '\'')) {
				char q = *p++;
				value = p;
				while (p < end && *p != q)
					p++;
				value_len = p - value;
				if (p < end)
					p++;
			} else {
				value = p;
				while (p < end && !nscss_is_space(*p) && *p != '>')
					p++;
				value_len = p - value;
			}
		}
		if (name_len == 0) {
			if (p < end)
				p++;
			continue;
		}
		if (n->n_attrs == room) {
			nscss_attr *a;
			room = room ? room * 2 : 8;
			a = (nscss_attr *) XP_REALLOC(n->attrs, room * sizeof(nscss_attr));
			if (!a)
				return;
			n->attrs = a;
		}
		n->attrs[n->n_attrs].name = (char *) XP_ALLOC(name_len + 1);
		n->attrs[n->n_attrs].value = (char *) XP_ALLOC(value_len + 1);
		if (!n->attrs[n->n_attrs].name || !n->attrs[n->n_attrs].value) {
			XP_FREEIF(n->attrs[n->n_attrs].name);
			XP_FREEIF(n->attrs[n->n_attrs].value);
			return;
		}
		for (i = 0; i < name_len; i++)
			n->attrs[n->n_attrs].name[i] = tolower((unsigned char) name[i]);
		n->attrs[n->n_attrs].name[name_len] = '\0';
		if (value_len)
			XP_MEMCPY(n->attrs[n->n_attrs].value, value, value_len);
		n->attrs[n->n_attrs].value[value_len] = '\0';
		n->n_attrs++;
	}
}

static const char *
nscss_attr_value(NSCSS_Node *n, lwc_string *name)
{
	int32 i;

	for (i = 0; i < n->n_attrs; i++)
		if (!strcasecomp(n->attrs[i].name, lwc_string_data(name)))
			return n->attrs[i].value;
	return NULL;
}

static void
nscss_set_classes(NSCSS_Node *n, const char *s)
{
	int32 room = 0;

	while (s && *s) {
		const char *w;
		lwc_string *c;

		while (*s && nscss_is_space(*s))
			s++;
		w = s;
		while (*s && !nscss_is_space(*s))
			s++;
		if (s == w)
			break;
		if ((int32) n->n_classes == room) {
			lwc_string **a;
			room = room ? room * 2 : 4;
			a = (lwc_string **) XP_REALLOC(n->classes, room * sizeof(lwc_string *));
			if (!a)
				return;
			n->classes = a;
		}
		if ((c = nscss_intern(w, s - w)) != NULL)
			n->classes[n->n_classes++] = c;
	}
}

/* ---- the selection handler ------------------------------------------ */

static css_error
h_node_name(void *pw, void *node, css_qname *qname)
{
	NSCSS_Node *n = (NSCSS_Node *) node;

	qname->ns = NULL;
	qname->name = lwc_string_ref(n->name);
	return CSS_OK;
}

static css_error
h_node_classes(void *pw, void *node, lwc_string ***classes, uint32_t *n_classes)
{
	NSCSS_Node *n = (NSCSS_Node *) node;
	uint32_t i;

	/* The array stays the node's; libcss drops the references. */
	*classes = n->n_classes ? n->classes : NULL;
	*n_classes = n->n_classes;
	for (i = 0; i < n->n_classes; i++)
		lwc_string_ref(n->classes[i]);
	return CSS_OK;
}

static css_error
h_node_id(void *pw, void *node, lwc_string **id)
{
	NSCSS_Node *n = (NSCSS_Node *) node;

	*id = n->id ? lwc_string_ref(n->id) : NULL;
	return CSS_OK;
}

static XP_Bool
nscss_name_is(NSCSS_Node *n, lwc_string *name)
{
	bool match = false;

	lwc_string_caseless_isequal(n->name, name, &match);
	return match;
}

static css_error
h_named_ancestor_node(void *pw, void *node, const css_qname *qname, void **ancestor)
{
	NSCSS_Node *n;

	for (n = ((NSCSS_Node *) node)->parent; n; n = n->parent)
		if (nscss_name_is(n, qname->name))
			break;
	*ancestor = n;
	return CSS_OK;
}

static css_error
h_named_parent_node(void *pw, void *node, const css_qname *qname, void **parent)
{
	NSCSS_Node *p = ((NSCSS_Node *) node)->parent;

	*parent = (p && nscss_name_is(p, qname->name)) ? p : NULL;
	return CSS_OK;
}

static css_error
h_named_sibling_node(void *pw, void *node, const css_qname *qname, void **sibling)
{
	NSCSS_Node *s = ((NSCSS_Node *) node)->prev;

	*sibling = (s && nscss_name_is(s, qname->name)) ? s : NULL;
	return CSS_OK;
}

static css_error
h_named_generic_sibling_node(void *pw, void *node, const css_qname *qname,
							 void **sibling)
{
	NSCSS_Node *s;

	for (s = ((NSCSS_Node *) node)->prev; s; s = s->prev)
		if (nscss_name_is(s, qname->name))
			break;
	*sibling = s;
	return CSS_OK;
}

static css_error
h_parent_node(void *pw, void *node, void **parent)
{
	*parent = ((NSCSS_Node *) node)->parent;
	return CSS_OK;
}

static css_error
h_sibling_node(void *pw, void *node, void **sibling)
{
	*sibling = ((NSCSS_Node *) node)->prev;
	return CSS_OK;
}

static css_error
h_node_has_name(void *pw, void *node, const css_qname *qname, bool *match)
{
	*match = nscss_name_is((NSCSS_Node *) node, qname->name);
	return CSS_OK;
}

static css_error
h_node_has_class(void *pw, void *node, lwc_string *name, bool *match)
{
	NSCSS_Node *n = (NSCSS_Node *) node;
	uint32_t i;

	*match = false;
	for (i = 0; i < n->n_classes && !*match; i++)
		lwc_string_isequal(n->classes[i], name, match);
	return CSS_OK;
}

static css_error
h_node_has_id(void *pw, void *node, lwc_string *name, bool *match)
{
	NSCSS_Node *n = (NSCSS_Node *) node;

	*match = false;
	if (n->id)
		lwc_string_isequal(n->id, name, match);
	return CSS_OK;
}

typedef enum { A_ANY, A_EQ, A_DASH, A_INCL, A_PREFIX, A_SUFFIX, A_SUBSTR } nscss_amatch;

static XP_Bool
nscss_attr_match(NSCSS_Node *n, const css_qname *qname, lwc_string *value,
				 nscss_amatch how)
{
	const char *a = nscss_attr_value(n, qname->name);
	const char *v = value ? lwc_string_data(value) : "";
	size_t al, vl = value ? lwc_string_length(value) : 0, i;

	if (a == NULL)
		return FALSE;
	al = strlen(a);
	switch (how) {
	case A_ANY:
		return TRUE;
	case A_EQ:
		return al == vl && !strncmp(a, v, vl);
	case A_DASH:
		return al >= vl && !strncmp(a, v, vl) && (a[vl] == '\0' || a[vl] == '-');
	case A_PREFIX:
		return vl > 0 && al >= vl && !strncmp(a, v, vl);
	case A_SUFFIX:
		return vl > 0 && al >= vl && !strncmp(a + al - vl, v, vl);
	case A_SUBSTR:
		for (i = 0; vl > 0 && i + vl <= al; i++)
			if (!strncmp(a + i, v, vl))
				return TRUE;
		return FALSE;
	case A_INCL:
		while (vl > 0 && *a) {
			size_t w;
			while (*a && nscss_is_space(*a))
				a++;
			for (w = 0; a[w] && !nscss_is_space(a[w]); w++)
				;
			if (w == vl && !strncmp(a, v, vl))
				return TRUE;
			a += w;
		}
		return FALSE;
	}
	return FALSE;
}

#define NSCSS_ATTR_FN(fn, how) \
static css_error \
fn(void *pw, void *node, const css_qname *qname, lwc_string *value, bool *match) \
{ \
	*match = nscss_attr_match((NSCSS_Node *) node, qname, value, how); \
	return CSS_OK; \
}
NSCSS_ATTR_FN(h_node_has_attribute_equal, A_EQ)
NSCSS_ATTR_FN(h_node_has_attribute_dashmatch, A_DASH)
NSCSS_ATTR_FN(h_node_has_attribute_includes, A_INCL)
NSCSS_ATTR_FN(h_node_has_attribute_prefix, A_PREFIX)
NSCSS_ATTR_FN(h_node_has_attribute_suffix, A_SUFFIX)
NSCSS_ATTR_FN(h_node_has_attribute_substring, A_SUBSTR)

static css_error
h_node_has_attribute(void *pw, void *node, const css_qname *qname, bool *match)
{
	*match = nscss_attr_match((NSCSS_Node *) node, qname, NULL, A_ANY);
	return CSS_OK;
}

static css_error
h_node_is_root(void *pw, void *node, bool *match)
{
	*match = ((NSCSS_Node *) node)->parent == NULL;
	return CSS_OK;
}

static css_error
h_node_count_siblings(void *pw, void *node, bool same_name, bool after,
					  int32_t *count)
{
	NSCSS_Node *n = (NSCSS_Node *) node, *s;
	int32_t c = 0;

	if (after) {
		/* What follows has not arrived: say there is something, so that
		 * :last-child and :only-child do not match everything. */
		*count = 1;
		return CSS_OK;
	}
	for (s = n->prev; s; s = s->prev)
		if (!same_name || nscss_name_is(s, n->name))
			c++;
	*count = c;
	return CSS_OK;
}

static css_error
h_node_is_empty(void *pw, void *node, bool *match)
{
	/* Its content has not arrived yet. */
	*match = false;
	return CSS_OK;
}

static css_error
h_node_is_link(void *pw, void *node, bool *match)
{
	NSCSS_Node *n = (NSCSS_Node *) node;
	int32 i;

	*match = false;
	if (lwc_string_length(n->name) == 1 &&
		tolower((unsigned char) lwc_string_data(n->name)[0]) == 'a')
		for (i = 0; i < n->n_attrs; i++)
			if (!XP_STRCMP(n->attrs[i].name, "href"))
				*match = true;
	return CSS_OK;
}

static css_error
h_false(void *pw, void *node, bool *match)
{
	*match = false;
	return CSS_OK;
}

static css_error
h_node_is_enabled(void *pw, void *node, bool *match)
{
	NSCSS_Node *n = (NSCSS_Node *) node;
	int32 i;

	*match = true;
	for (i = 0; i < n->n_attrs; i++)
		if (!XP_STRCMP(n->attrs[i].name, "disabled"))
			*match = false;
	return CSS_OK;
}

static css_error
h_node_is_disabled(void *pw, void *node, bool *match)
{
	h_node_is_enabled(pw, node, match);
	*match = !*match;
	return CSS_OK;
}

static css_error
h_node_is_checked(void *pw, void *node, bool *match)
{
	NSCSS_Node *n = (NSCSS_Node *) node;
	int32 i;

	*match = false;
	for (i = 0; i < n->n_attrs; i++)
		if (!XP_STRCMP(n->attrs[i].name, "checked") ||
			!XP_STRCMP(n->attrs[i].name, "selected"))
			*match = true;
	return CSS_OK;
}

static css_error
h_node_is_lang(void *pw, void *node, lwc_string *lang, bool *match)
{
	NSCSS_Node *n;
	size_t ll = lwc_string_length(lang);

	*match = false;
	for (n = (NSCSS_Node *) node; n; n = n->parent) {
		int32 i;
		for (i = 0; i < n->n_attrs; i++)
			if (!XP_STRCMP(n->attrs[i].name, "lang")) {
				const char *v = n->attrs[i].value;
				*match = !strncasecomp(v, lwc_string_data(lang), ll) &&
					(v[ll] == '\0' || v[ll] == '-');
				return CSS_OK;
			}
	}
	return CSS_OK;
}

static css_error
h_node_presentational_hint(void *pw, void *node, uint32_t *nhints, css_hint **hints)
{
	/* Layout applies HTML's own presentation. */
	*nhints = 0;
	*hints = NULL;
	return CSS_OK;
}

static css_error
h_ua_default_for_property(void *pw, uint32_t property, css_hint *hint)
{
	if (property == CSS_PROP_COLOR) {
		hint->data.color = 0xff000000;
		hint->status = CSS_COLOR_COLOR;
	} else if (property == CSS_PROP_FONT_FAMILY) {
		hint->data.strings = NULL;
		hint->status = CSS_FONT_FAMILY_SERIF;
	} else if (property == CSS_PROP_QUOTES) {
		hint->data.strings = NULL;
		hint->status = CSS_QUOTES_NONE;
	} else if (property == CSS_PROP_VOICE_FAMILY) {
		hint->data.strings = NULL;
		hint->status = 0;
	} else {
		return CSS_INVALID;
	}
	return CSS_OK;
}

static css_error
h_set_libcss_node_data(void *pw, void *node, void *data)
{
	((NSCSS_Node *) node)->node_data = data;
	return CSS_OK;
}

static css_error
h_get_libcss_node_data(void *pw, void *node, void **data)
{
	*data = ((NSCSS_Node *) node)->node_data;
	return CSS_OK;
}

static css_select_handler nscss_handler = {
	CSS_SELECT_HANDLER_VERSION_1,
	h_node_name, h_node_classes, h_node_id,
	h_named_ancestor_node, h_named_parent_node,
	h_named_sibling_node, h_named_generic_sibling_node,
	h_parent_node, h_sibling_node,
	h_node_has_name, h_node_has_class, h_node_has_id,
	h_node_has_attribute, h_node_has_attribute_equal,
	h_node_has_attribute_dashmatch, h_node_has_attribute_includes,
	h_node_has_attribute_prefix, h_node_has_attribute_suffix,
	h_node_has_attribute_substring,
	h_node_is_root, h_node_count_siblings, h_node_is_empty,
	h_node_is_link,
	h_false, h_false, h_false, h_false,	/* visited hover active focus */
	h_node_is_enabled, h_node_is_disabled, h_node_is_checked,
	h_false, h_node_is_lang,			/* target, lang */
	h_node_presentational_hint, h_ua_default_for_property,
	h_set_libcss_node_data, h_get_libcss_node_data
};

/* ---- style sheets ----------------------------------------------------- */

static css_error
nscss_resolve(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{
	char *url = NULL, *r;

	r = XP_ALLOC(lwc_string_length(rel) + 1);
	if (!r)
		return CSS_NOMEM;
	XP_MEMCPY(r, lwc_string_data(rel), lwc_string_length(rel));
	r[lwc_string_length(rel)] = '\0';
	if (base && *base)
		url = NET_MakeAbsoluteURL((char *) base, r);
	if (!url) {
		url = r;
		r = NULL;
	}
	*abs = nscss_intern(url, XP_STRLEN(url));
	XP_FREE(url);
	XP_FREEIF(r);
	return *abs ? CSS_OK : CSS_NOMEM;
}

static css_stylesheet *
nscss_parse(const char *url, const char *charset, XP_Bool inline_style,
			const char *data, int32 len)
{
	css_stylesheet_params p;
	css_stylesheet *s = NULL;
	css_error e;

	XP_MEMSET(&p, 0, sizeof p);
	p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
	p.level = CSS_LEVEL_DEFAULT;
	p.charset = charset;
	p.url = url ? url : "";
	p.allow_quirks = true;
	p.inline_style = inline_style;
	p.resolve = nscss_resolve;
	if (css_stylesheet_create(&p, &s) != CSS_OK)
		return NULL;
	e = css_stylesheet_append_data(s, (const uint8_t *) data, len);
	if (e == CSS_OK || e == CSS_NEEDDATA)
		e = css_stylesheet_data_done(s);
	if (e != CSS_OK && e != CSS_IMPORTS_PENDING) {
		css_stylesheet_destroy(s);
		return NULL;
	}
	return s;
}

/* ---- custom properties ------------------------------------------------------
 *
 * libcss 0.9.2 has no custom properties: a declaration using var() is
 * dropped.  So before a sheet is parsed, its var() references are replaced
 * by the values the page's sheets give the properties.  One value per
 * property for the whole document: what :root, html, body or * set wins
 * over what other selectors set (themes, components); later over earlier.
 * Values set inside @media for a dark colour scheme or for print are left
 * out.  Approximate (a property set differently on some elements gets the
 * document-wide value), but the usual use -- design tokens on :root -- comes
 * out right.
 */

struct nscss_var {
	char	*name;
	char	*value;
	int		 prio;
};

static void
nscss_var_set(NSCSS_Doc *doc, const char *name, int32 nlen,
			  const char *value, int32 vlen, int prio)
{
	int32 i;
	char *v;

	while (vlen > 0 && isspace((unsigned char) value[vlen - 1]))
		vlen--;
	while (vlen > 0 && isspace((unsigned char) *value)) {
		value++;
		vlen--;
	}
	/* !important is not part of the value */
	if (vlen >= 10 && !strncasecomp(value + vlen - 10, "!important", 10)) {
		vlen -= 10;
		while (vlen > 0 && isspace((unsigned char) value[vlen - 1]))
			vlen--;
	}
	for (i = 0; i < doc->n_vars; i++)
		if ((int32) XP_STRLEN(doc->vars[i].name) == nlen &&
			!XP_STRNCMP(doc->vars[i].name, name, nlen))
			break;
	if (i < doc->n_vars && doc->vars[i].prio > prio)
		return;
	v = (char *) XP_ALLOC(vlen + 1);
	if (!v)
		return;
	XP_MEMCPY(v, value, vlen);
	v[vlen] = '\0';
	if (i < doc->n_vars) {
		XP_FREE(doc->vars[i].value);
		doc->vars[i].value = v;
		doc->vars[i].prio = prio;
		return;
	}
	if (doc->n_vars == doc->vars_cap) {
		int32 cap = doc->vars_cap ? doc->vars_cap * 2 : 64;
		struct nscss_var *a = (struct nscss_var *)
			XP_REALLOC(doc->vars, cap * sizeof *a);
		if (!a) {
			XP_FREE(v);
			return;
		}
		doc->vars = a;
		doc->vars_cap = cap;
	}
	doc->vars[i].name = (char *) XP_ALLOC(nlen + 1);
	if (!doc->vars[i].name) {
		XP_FREE(v);
		return;
	}
	XP_MEMCPY(doc->vars[i].name, name, nlen);
	doc->vars[i].name[nlen] = '\0';
	doc->vars[i].value = v;
	doc->vars[i].prio = prio;
	doc->n_vars++;
}

static const char *
nscss_var_get(NSCSS_Doc *doc, const char *name, int32 nlen)
{
	int32 i;

	for (i = 0; i < doc->n_vars; i++)
		if ((int32) XP_STRLEN(doc->vars[i].name) == nlen &&
			!XP_STRNCMP(doc->vars[i].name, name, nlen))
			return doc->vars[i].value;
	return NULL;
}

/* Skip a comment or string starting at d[i]; the index after it. */
static int32
nscss_skip(const char *d, int32 len, int32 i)
{
	if (d[i] == '/' && i + 1 < len && d[i + 1] == '*') {
		for (i += 2; i + 1 < len && !(d[i] == '*' && d[i + 1] == '/'); i++)
			;
		return i + 2 < len ? i + 2 : len;
	}
	if (d[i] == '"' || d[i] == '\'') {
		char q = d[i];
		for (i++; i < len && d[i] != q; i++)
			if (d[i] == '\\')
				i++;
		return i + 1 < len ? i + 1 : len;
	}
	return i + 1;
}

/* How much a selector's custom properties count: 2 for the document
 * itself (:root, html, body, * alone, in any of a list's selectors), 1
 * for anything else (components; the document under a condition: a theme
 * class, an attribute). */
static int
nscss_rootish(const char *sel, int32 n)
{
	int32 i = 0;

	while (i < n) {
		int32 j, k;
		while (i < n && (isspace((unsigned char) sel[i]) || sel[i] == ','))
			i++;
		j = i;
		while (j < n && sel[j] != ',')
			j++;
		k = j;
		while (k > i && isspace((unsigned char) sel[k - 1]))
			k--;
		if ((k - i == 5 && !strncasecomp(sel + i, ":root", 5)) ||
			(k - i == 4 && !strncasecomp(sel + i, "html", 4)) ||
			(k - i == 4 && !strncasecomp(sel + i, "body", 4)) ||
			(k - i == 5 && !strncasecomp(sel + i, ":host", 5)) ||
			(k - i == 1 && sel[i] == '*'))
			return 2;
		i = j;
	}
	return 1;
}

/* Record the custom properties DATA sets. */
static void
nscss_vars_collect(NSCSS_Doc *doc, const char *d, int32 len)
{
	/* the blocks we are in: for each, whether its declarations count, and
	 * how much (0: skipped) */
	int prio[32];
	int depth = 0;
	int32 i = 0, start = 0;

	prio[0] = 1;
	while (i < len) {
		char c = d[i];
		if ((c == '/' && i + 1 < len && d[i + 1] == '*') || c == '"' || c == '\'') {
			i = nscss_skip(d, len, i);
			continue;
		}
		if (c == '{') {
			const char *pre = d + start;
			int32 n = i - start, k;
			int p = depth > 0 ? prio[depth] : 1;

			while (n > 0 && isspace((unsigned char) *pre)) {
				pre++;
				n--;
			}
			if (n > 0 && pre[0] == '@') {
				/* a group: @media dark / print, @keyframes, @font-face
				 * declarations do not count */
				for (k = 0; k + 4 < n; k++)
					if (!strncasecomp(pre + k, "dark", 4) ||
						!strncasecomp(pre + k, "print", 5))
						p = 0;
				if (n >= 10 && !strncasecomp(pre, "@keyframes", 10))
					p = 0;
				if (n >= 10 && !strncasecomp(pre, "@font-face", 10))
					p = 0;
				if (n >= 6 && !strncasecomp(pre, "@media", 6) && p)
					p = prio[depth] ? prio[depth] : 1;
			} else if (p) {
				p = nscss_rootish(pre, n);
				/* a dark theme's values ([data-color-mode=dark], .dark,
				 * .skin-theme-clientpref-night ...) */
				for (k = 0; k + 4 <= n; k++)
					if (!strncasecomp(pre + k, "dark", 4) ||
						(k + 5 <= n && !strncasecomp(pre + k, "night", 5))) {
						p = 0;
						break;
					}
			}
			if (depth < 31)
				prio[++depth] = p;
			i++;
			start = i;
			continue;
		}
		if (c == '}') {
			if (depth > 0)
				depth--;
			i++;
			start = i;
			continue;
		}
		if (c == ';') {
			i++;
			start = i;
			continue;
		}
		/* a declaration "--name: value" in a block whose declarations count */
		if (depth > 0 && prio[depth] && c == '-' && i + 1 < len && d[i + 1] == '-') {
			int32 j = start;
			while (j < i && isspace((unsigned char) d[j]))
				j++;
			if (j == i) {
				int32 nstart = i, nend, vstart, paren = 0;
				while (i < len && d[i] != ':' && d[i] != ';' && d[i] != '}' &&
					   !isspace((unsigned char) d[i]))
					i++;
				nend = i;
				while (i < len && isspace((unsigned char) d[i]))
					i++;
				if (i < len && d[i] == ':') {
					i++;
					vstart = i;
					while (i < len) {
						if (d[i] == '"' || d[i] == '\'' ||
							(d[i] == '/' && i + 1 < len && d[i + 1] == '*')) {
							i = nscss_skip(d, len, i);
							continue;
						}
						if (d[i] == '(' || d[i] == '[' || (d[i] == '{' && paren > 0))
							paren++;
						else if ((d[i] == ')' || d[i] == ']' || d[i] == '}') && paren > 0)
							paren--;
						else if (!paren && (d[i] == ';' || d[i] == '}'))
							break;
						i++;
					}
					nscss_var_set(doc, d + nstart, nend - nstart, d + vstart,
								  i - vstart, prio[depth]);
				}
				continue;
			}
		}
		i++;
	}
}

typedef struct {
	char	*buf;
	int32	 len, cap;
} nscss_buf;

static void
nscss_buf_add(nscss_buf *b, const char *s, int32 n)
{
	if (b->len + n + 1 > b->cap) {
		int32 cap = (b->len + n + 1) * 2 + 256;
		char *p = (char *) XP_REALLOC(b->buf, cap);
		if (!p)
			return;
		b->buf = p;
		b->cap = cap;
	}
	XP_MEMCPY(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
}

/* Append D with its var() references replaced. */
static void
nscss_vars_subst(NSCSS_Doc *doc, nscss_buf *out, const char *d, int32 len,
				 int level)
{
	int32 i = 0, from = 0;

	while (i < len) {
		if ((d[i] == '/' && i + 1 < len && d[i + 1] == '*') ||
			d[i] == '"' || d[i] == '\'') {
			i = nscss_skip(d, len, i);
			continue;
		}
		if ((d[i] == 'v' || d[i] == 'V') && i + 4 <= len &&
			!strncasecomp(d + i, "var(", 4) &&
			(i == 0 || !(isalnum((unsigned char) d[i - 1]) || d[i - 1] == '-'))) {
			int32 j = i + 4, paren = 1, comma = -1, nstart, nend;
			const char *v;

			while (j < len && paren) {
				if (d[j] == '"' || d[j] == '\'') {
					j = nscss_skip(d, len, j);
					continue;
				}
				if (d[j] == '(')
					paren++;
				else if (d[j] == ')')
					paren--;
				else if (d[j] == ',' && paren == 1 && comma < 0)
					comma = j;
				if (paren)
					j++;
			}
			if (paren) {		/* unterminated: leave it */
				i = len;
				break;
			}
			nstart = i + 4;
			while (nstart < j && isspace((unsigned char) d[nstart]))
				nstart++;
			nend = comma >= 0 ? comma : j;
			while (nend > nstart && isspace((unsigned char) d[nend - 1]))
				nend--;
			v = level < 8 ? nscss_var_get(doc, d + nstart, nend - nstart) : NULL;
			if (v || comma >= 0) {
				nscss_buf_add(out, d + from, i - from);
				if (v)
					nscss_vars_subst(doc, out, v, XP_STRLEN(v), level + 1);
				else
					nscss_vars_subst(doc, out, d + comma + 1, j - comma - 1,
									 level + 1);
				from = j + 1;
			}
			i = j + 1;
			continue;
		}
		i++;
	}
	nscss_buf_add(out, d + from, len - from);
}

/* Cascade layers (@layer) are newer than libcss, which drops the block
 * and every rule in it: keep the rules (in source order, which is how
 * layers usually come anyway), drop the layer statements. */
static char *
nscss_unlayer(const char *d, int32 len, int32 *out_len)
{
	nscss_buf b = { NULL, 0, 0 };
	int32 i = 0, from = 0, depth = 0;
	int32 drop[32];			/* brace depths whose closing brace goes */
	int ndrop = 0;

	while (i < len) {
		char c = d[i];
		if ((c == '/' && i + 1 < len && d[i + 1] == '*') || c == '"' || c == '\'') {
			i = nscss_skip(d, len, i);
			continue;
		}
		if (c == '@' && i + 6 <= len && !strncasecomp(d + i, "@layer", 6) &&
			(i + 6 == len || !isalnum((unsigned char) d[i + 6]))) {
			int32 j = i + 6;
			while (j < len && d[j] != '{' && d[j] != ';' && d[j] != '}')
				j++;
			nscss_buf_add(&b, d + from, i - from);
			if (j < len && d[j] == '{') {
				depth++;
				if (ndrop < 32)
					drop[ndrop++] = depth;
			}
			i = j < len && d[j] != '}' ? j + 1 : j;
			from = i;
			continue;
		}
		if (c == '{')
			depth++;
		else if (c == '}') {
			if (ndrop > 0 && drop[ndrop - 1] == depth) {
				ndrop--;
				nscss_buf_add(&b, d + from, i - from);
				from = i + 1;
			}
			depth--;
		}
		i++;
	}
	if (!b.buf)
		return NULL;
	nscss_buf_add(&b, d + from, len - from);
	*out_len = b.len;
	return b.buf;
}

static char *nscss_media_list(const char *media);

/* @media lists libcss can read (see nscss_media_list).  Allocated, or NULL
 * if no @media needed it. */
static char *
nscss_fix_media(const char *d, int32 len, int32 *out_len)
{
	nscss_buf b = { NULL, 0, 0 };
	int32 i = 0, from = 0;

	while (i < len) {
		char c = d[i];

		if ((c == '/' && i + 1 < len && d[i + 1] == '*') || c == '"' || c == '\'') {
			i = nscss_skip(d, len, i);
			continue;
		}
		if (c == '@' && i + 6 < len && !strncasecomp(d + i, "@media", 6) &&
			isspace((unsigned char) d[i + 6])) {
			int32 j = i + 6;
			char *list, *m;

			while (j < len && d[j] != '{' && d[j] != ';' && d[j] != '}')
				j++;
			if (j >= len || d[j] != '{') {
				i = j;
				continue;
			}
			list = (char *) XP_ALLOC(j - i - 6 + 1);
			if (!list)
				break;
			XP_MEMCPY(list, d + i + 6, j - i - 6);
			list[j - i - 6] = '\0';
			m = nscss_media_list(list);
			if (m) {
				nscss_buf_add(&b, d + from, i + 6 - from);
				nscss_buf_add(&b, " ", 1);
				nscss_buf_add(&b, m, XP_STRLEN(m));
				nscss_buf_add(&b, " ", 1);
				from = j;
				XP_FREE(m);
			}
			XP_FREE(list);
			i = j + 1;
			continue;
		}
		i++;
	}
	if (!b.buf)
		return NULL;
	nscss_buf_add(&b, d + from, len - from);
	*out_len = b.len;
	return b.buf;
}

/* A stray '}' outside any block (a selector that lost its '{'): browsers
 * read it as part of the next rule's selector and drop that rule; libcss
 * loses its place and drops the rest of the sheet.  Drop the statement it
 * ends and the rule after it, as browsers do.  Allocated, or NULL if the
 * sheet has none. */
static char *
nscss_drop_stray(const char *d, int32 len, int32 *out_len)
{
	nscss_buf b = { NULL, 0, 0 };
	int32 i = 0, from = 0, depth = 0, stmt = 0;

	while (i < len) {
		char c = d[i];

		if ((c == '/' && i + 1 < len && d[i + 1] == '*') || c == '"' || c == '\'') {
			i = nscss_skip(d, len, i);
			continue;
		}
		if (c == '{')
			depth++;
		else if (c == '}' && depth > 0) {
			if (--depth == 0)
				stmt = i + 1;
		} else if (c == '}') {
			/* the rule after it: to the end of its block */
			int32 j = i + 1, inner = 0;

			while (j < len) {
				char e = d[j];

				if ((e == '/' && j + 1 < len && d[j + 1] == '*') ||
					e == '"' || e == '\'') {
					j = nscss_skip(d, len, j);
					continue;
				}
				j++;
				if (e == '{')
					inner++;
				else if (e == '}' && inner > 0 && --inner == 0)
					break;
			}
			nscss_buf_add(&b, d + from, stmt - from);
			from = stmt = i = j;
			continue;
		} else if (c == ';' && depth == 0)
			stmt = i + 1;
		i++;
	}
	if (!b.buf)
		return NULL;
	nscss_buf_add(&b, d + from, len - from);
	*out_len = b.len;
	return b.buf;
}

/* DATA with what libcss cannot read made readable (@layer unwrapped, var()
 * resolved): allocated, or NULL if there was nothing to do. */
static char *
nscss_vars_resolve(NSCSS_Doc *doc, const char *data, int32 len, int32 *out_len)
{
	nscss_buf b = { NULL, 0, 0 };
	int32 i, ulen = 0;
	char *u = nscss_unlayer(data, len, &ulen), *t;
	int32 tlen = 0;

	if (u) {
		data = u;
		len = ulen;
	}
	if ((t = nscss_drop_stray(data, len, &tlen)) != NULL) {
		XP_FREEIF(u);
		data = u = t;
		len = ulen = tlen;
	}
	if ((t = nscss_fix_media(data, len, &tlen)) != NULL) {
		XP_FREEIF(u);
		data = u = t;
		len = ulen = tlen;
	}
	for (i = 0; i + 4 <= len; i++)
		if (!strncasecomp(data + i, "var(", 4))
			break;
	if (i + 4 > len) {
		*out_len = ulen;
		return u;
	}
	nscss_vars_subst(doc, &b, data, len, 0);
	XP_FREEIF(u);
	*out_len = b.len;
	return b.buf;
}

NSCSS_Doc *
NSCSS_NewDoc(void)
{
	NSCSS_Doc *doc = XP_NEW_ZAP(NSCSS_Doc);

	if (!doc)
		return NULL;
	doc->ua_sheet = nscss_parse("about:nscss-ua", "UTF-8", FALSE,
								nscss_ua_sheet, sizeof nscss_ua_sheet - 1);
	if (!doc->ua_sheet ||
		css_select_ctx_create(&doc->ctx) != CSS_OK ||
		css_select_ctx_create(&doc->ua_ctx) != CSS_OK) {
		NSCSS_DestroyDoc(doc);
		return NULL;
	}
	css_select_ctx_append_sheet(doc->ctx, doc->ua_sheet, CSS_ORIGIN_UA, NULL);
	css_select_ctx_append_sheet(doc->ua_ctx, doc->ua_sheet, CSS_ORIGIN_UA, NULL);

	doc->unit.font_size_default = INTTOFIX(16);
	doc->unit.font_size_minimum = INTTOFIX(6);
	doc->unit.device_dpi = INTTOFIX(96);
	doc->media.type = CSS_MEDIA_SCREEN;
	NSCSS_SetViewport(doc, 800, 600);
	return doc;
}

void
NSCSS_DestroyDoc(NSCSS_Doc *doc)
{
	int32 i;

	if (!doc)
		return;
	if (doc->ctx)
		css_select_ctx_destroy(doc->ctx);
	if (doc->ua_ctx)
		css_select_ctx_destroy(doc->ua_ctx);
	for (i = 0; i < doc->n_sheets; i++)
		css_stylesheet_destroy(doc->sheets[i]);
	XP_FREEIF(doc->sheets);
	XP_FREEIF(doc->sheet_hashes);
	for (i = 0; i < doc->n_vars; i++) {
		XP_FREE(doc->vars[i].name);
		XP_FREE(doc->vars[i].value);
	}
	XP_FREEIF(doc->vars);
	if (doc->ua_sheet)
		css_stylesheet_destroy(doc->ua_sheet);
	XP_FREE(doc);
}

void
NSCSS_SetViewport(NSCSS_Doc *doc, int32 width, int32 height)
{
	if (!doc || width <= 0 || height <= 0)
		return;
	doc->unit.viewport_width = doc->media.width = INTTOFIX(width);
	doc->unit.viewport_height = doc->media.height = INTTOFIX(height);
	doc->media.aspect_ratio = css_divide_fixed(INTTOFIX(width), INTTOFIX(height));
	doc->media.orientation = width > height ?
		CSS_MEDIA_ORIENTATION_LANDSCAPE : CSS_MEDIA_ORIENTATION_PORTRAIT;
	doc->media.color = 8;
	doc->media.grid = false;
}

/* A media list libcss can read: it rejects the whole list over one old
 * media type (media="screen, projection").  Queries on types it does not
 * know are left out (they never match a screen); none left: "not all".
 * Allocated, or NULL to use MEDIA as it is. */
static char *
nscss_media_list(const char *media)
{
	static const char *const known[] = { "all", "screen", "print", "speech", NULL };
	const char *p = media, *q;
	char *out;
	XP_Bool changed = FALSE;
	int32 n = 0;

	if (!media || !*media)
		return NULL;
	out = (char *) XP_ALLOC(XP_STRLEN(media) + 8);
	if (!out)
		return NULL;
	while (*p) {
		const char *w;
		int32 wl, k;
		XP_Bool keep = FALSE;

		while (*p == ',' || isspace((unsigned char) *p))
			p++;
		if (!*p)
			break;
		for (q = p; *q && *q != ','; q++)
			;
		/* its media type: the first word, after "only" or "not" */
		w = p;
		for (;;) {
			for (wl = 0; w + wl < q && (isalnum((unsigned char) w[wl]) ||
										w[wl] == '-'); wl++)
				;
			if ((wl == 4 && !strncasecomp(w, "only", 4)) ||
				(wl == 3 && !strncasecomp(w, "not", 3))) {
				w += wl;
				while (w < q && isspace((unsigned char) *w))
					w++;
				continue;
			}
			break;
		}
		if (wl == 0)
			keep = TRUE;			/* "(min-width: ...)" */
		for (k = 0; !keep && known[k]; k++)
			if ((int32) XP_STRLEN(known[k]) == wl &&
				!strncasecomp(w, known[k], wl))
				keep = TRUE;
		if (keep) {
			if (n)
				out[n++] = ',';
			XP_MEMCPY(out + n, p, q - p);
			n += q - p;
		} else
			changed = TRUE;
		p = q;
	}
	if (!changed) {
		XP_FREE(out);
		return NULL;
	}
	if (n == 0) {
		XP_STRCPY(out, "not all");
		return out;
	}
	out[n] = '\0';
	return out;
}

/* NSCSS_DEBUG=/file (or any other value, for stderr): log each sheet, each
 * tag and what it is given. */
static FILE *nscss_log;

static FILE *
nscss_log_open(void)
{
	static int checked;

	if (!checked) {
		const char *e = getenv("NSCSS_DEBUG");
		checked = 1;
		if (e && *e == '/')
			nscss_log = fopen(e, "a");
		else if (e)
			nscss_log = stderr;
	}
	return nscss_log;
}

void
NSCSS_AddSheet(NSCSS_Doc *doc, const char *url, const char *charset,
			   const char *media, const char *data, int32 len)
{
	css_stylesheet *s, **a;
	uint32 h = 2166136261u, *ha;
	const char *p;
	int32 i;

	if (!doc || !data)
		return;
	{
		/* XHTML's <style><![CDATA[ ... ]]></style>: an XML parser would
		 * have taken the markers away; ours leaves them in the sheet */
		int32 i = 0;

		while (i < len && isspace((unsigned char) data[i]))
			i++;
		if (len - i >= 9 && !strncmp(data + i, "<![CDATA[", 9)) {
			char *c = (char *) XP_ALLOC(len + 1), *end;

			if (c) {
				memcpy(c, data, len);
				c[len] = '\0';
				memset(c + i, ' ', 9);
				if ((end = strstr(c + i, "]]>")) != NULL)
					memset(end, ' ', 3);
				NSCSS_AddSheet(doc, url, charset, media, c, len);
				XP_FREE(c);
				return;
			}
		}
	}
	if (nscss_log_open()) {
		fprintf(nscss_log, "sheet %s media \"%s\" %ld bytes\n",
				url ? url : "(inline)", media ? media : "", (long) len);
		fflush(nscss_log);
	}
	/* FNV-1a over URL, media and text: the same sheet once */
	for (p = url ? url : ""; *p; p++)
		h = (h ^ (unsigned char) *p) * 16777619u;
	for (p = media ? media : ""; *p; p++)
		h = (h ^ (unsigned char) *p) * 16777619u;
	for (i = 0; i < len; i++)
		h = (h ^ (unsigned char) data[i]) * 16777619u;
	for (i = 0; i < doc->n_sheets; i++)
		if (doc->sheet_hashes[i] == h)
			return;
	nscss_vars_collect(doc, data, len);
	{
		int32 rlen = 0;
		char *r = nscss_vars_resolve(doc, data, len, &rlen);
		s = nscss_parse(url, charset, FALSE, r ? r : data, r ? rlen : len);
		XP_FREEIF(r);
	}
	if (!s)
		return;
	a = (css_stylesheet **) XP_REALLOC(doc->sheets,
									   (doc->n_sheets + 1) * sizeof *a);
	ha = (uint32 *) XP_REALLOC(doc->sheet_hashes,
							   (doc->n_sheets + 1) * sizeof *ha);
	if (a)
		doc->sheets = a;
	if (ha)
		doc->sheet_hashes = ha;
	if (!a || !ha) {
		css_stylesheet_destroy(s);
		return;
	}
	doc->sheet_hashes[doc->n_sheets] = h;
	doc->sheets[doc->n_sheets++] = s;
	{
		char *m = nscss_media_list(media);

		css_select_ctx_append_sheet(doc->ctx, s, CSS_ORIGIN_AUTHOR,
									m ? m : media && *media ? media : NULL);
		XP_FREEIF(m);
	}
}

NSCSS_Node *
NSCSS_OpenNode(NSCSS_Doc *doc, NSCSS_Node *parent, const char *name,
			   const char *class_name, const char *id, const char *attrs,
			   int32 attrs_len, const char *style, const char *base_url)
{
	NSCSS_Node *n;

	if (!doc || !name)
		return NULL;
	n = XP_NEW_ZAP(NSCSS_Node);
	if (!n)
		return NULL;
	n->refcount = 1;			/* the stack's */
	n->doc = doc;
	n->name = nscss_intern(name, XP_STRLEN(name));
	if (!n->name) {
		XP_FREE(n);
		return NULL;
	}
	if (id && *id)
		n->id = nscss_intern(id, XP_STRLEN(id));
	nscss_set_classes(n, class_name);
	if (attrs && attrs_len > 0)
		nscss_parse_attrs(n, attrs, attrs_len);
	if (style && *style) {
		int32 rlen = 0;
		char *r = nscss_vars_resolve(doc, style, XP_STRLEN(style), &rlen);
		n->inline_style = nscss_parse(base_url, "UTF-8", TRUE,
									  r ? r : style, r ? rlen : XP_STRLEN(style));
		XP_FREEIF(r);
	}
	if (parent) {
		n->parent = nscss_ref(parent);
		n->prev = parent->last_child;	/* the parent's reference moves */
		parent->last_child = nscss_ref(n);
	}
	return n;
}

void
NSCSS_CloseNode(NSCSS_Doc *doc, NSCSS_Node *node)
{
	NSCSS_Node *c;

	if (!node)
		return;
	/* Nothing can select on this node's children any more. */
	c = node->last_child;
	node->last_child = NULL;
	if (c)
		nscss_unref(c);
	nscss_unref(node);
}

/* ---- export to the StyleStruct ---------------------------------------- */

/* The marker values as libcss stores them (22.10 fixed point). */
#define NSCSS_FIX_MARK_LEN	((css_fixed) (-12345.25 * 1024))
#define NSCSS_FIX_MARK_POS	((css_fixed) (12345.25 * 1024))
#define NSCSS_COLOR_MARK	0x01010203

#define NSCSS_PRIORITY	MAX_STYLESTRUCT_PRIORITY


static void
nscss_set(StyleStruct *style, char *name, const char *value)
{
	if (value && *value) {
		STYLESTRUCT_SetString(style, name, (char *) value, NSCSS_PRIORITY);
		if (nscss_log)
			fprintf(nscss_log, "    %s: %s\n", name, value);
	}
}

static void
nscss_log_node(NSCSS_Node *node)
{
	NSCSS_Node *n;
	int32 depth = 0;

	if (!nscss_log_open())
		return;
	for (n = node->parent; n; n = n->parent)
		depth++;
	fprintf(nscss_log, "%*s<%s", (int) depth, "", lwc_string_data(node->name));
	if (node->id)
		fprintf(nscss_log, " #%s", lwc_string_data(node->id));
	if (node->n_classes)
		fprintf(nscss_log, " .%s", lwc_string_data(node->classes[0]));
	fprintf(nscss_log, ">%s\n", node->prev ? " (after a sibling)" : "");
	fflush(nscss_log);
}

static XP_Bool
nscss_marked(css_fixed len, css_unit unit, css_fixed mark)
{
	return unit == CSS_UNIT_PX && len == mark;
}

/* A length as layout takes it ("2em", "50%", "12pt"; LO_AdjustSSUnits
 * converts).  Units layout does not know become pixels or ems.  Returns
 * BUF, or NULL for units that make no sense here. */
static char *
nscss_len(NSCSS_Doc *doc, char *buf, css_fixed len, css_unit unit)
{
	double v = FIXTOFLT(len), px = 0;
	const char *u = NULL;

	switch (unit) {
	case CSS_UNIT_PX: u = "px"; break;
	case CSS_UNIT_EM: u = "em"; break;
	case CSS_UNIT_EX: u = "ex"; break;
	case CSS_UNIT_PT: u = "pt"; break;
	case CSS_UNIT_PC: u = "pc"; break;
	case CSS_UNIT_IN: u = "in"; break;
	case CSS_UNIT_CM: u = "cm"; break;
	case CSS_UNIT_MM: u = "mm"; break;
	case CSS_UNIT_PCT: u = "%"; break;
	case CSS_UNIT_CH: v *= 0.5; u = "em"; break;
	case CSS_UNIT_LH: v *= 1.2; u = "em"; break;
	case CSS_UNIT_Q: v *= 0.25; u = "mm"; break;
	case CSS_UNIT_REM: px = v * FIXTOFLT(doc->unit.font_size_default); break;
	case CSS_UNIT_VW: case CSS_UNIT_VI:
		px = v * FIXTOFLT(doc->unit.viewport_width) / 100; break;
	case CSS_UNIT_VH: case CSS_UNIT_VB:
		px = v * FIXTOFLT(doc->unit.viewport_height) / 100; break;
	case CSS_UNIT_VMIN: case CSS_UNIT_VMAX: {
		double w = FIXTOFLT(doc->unit.viewport_width);
		double h = FIXTOFLT(doc->unit.viewport_height);
		px = v * ((unit == CSS_UNIT_VMIN) == (w < h) ? w : h) / 100;
		break;
	}
	default:
		return NULL;
	}
	if (u == NULL) {
		v = px;
		u = "px";
	}
	PR_snprintf(buf, 40, "%g%s", v, u);
	return buf;
}

static char *
nscss_color(char *buf, css_color c)
{
	if ((c >> 24) == 0)			/* transparent: layout would paint black */
		return NULL;
	PR_snprintf(buf, 40, "#%06x", (unsigned) (c & 0xffffff));
	return buf;
}

/* ---- inherited properties: NULL when no rule set them ---- */

static char *
fmt_color(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	css_color c;

	if (css_computed_color(st, &c) != CSS_COLOR_COLOR)
		return NULL;
	return nscss_color(buf, c);
}

static char *
fmt_font_size(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	static const char *const kw[] = {
		NULL, "xx-small", "x-small", "small", "medium", "large", "x-large",
		"xx-large", "larger", "smaller"
	};
	css_fixed len;
	css_unit unit;
	uint8_t t = css_computed_font_size(st, &len, &unit);

	if (t == CSS_FONT_SIZE_DIMENSION)
		return len > 0 ? nscss_len(doc, buf, len, unit) : NULL;
	return t < sizeof kw / sizeof kw[0] ? (char *) kw[t] : NULL;
}

static char *
fmt_font_family(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	static const char *const generic[] = {
		NULL, "serif", "sans-serif", "cursive", "fantasy", "monospace"
	};
	lwc_string **names = NULL;
	uint8_t t = css_computed_font_family(st, &names);
	int32 n = 0;

	if (t == CSS_FONT_FAMILY_INHERIT)
		return NULL;
	buf[0] = '\0';
	/* The X front end splits the list on commas. */
	while (names && *names) {
		size_t l = lwc_string_length(*names);
		if (n + l + 2 >= 200)
			break;
		if (n)
			buf[n++] = ',';
		XP_MEMCPY(buf + n, lwc_string_data(*names), l);
		n += l;
		buf[n] = '\0';
		names++;
	}
	if (t < sizeof generic / sizeof generic[0] && generic[t] &&
		n + XP_STRLEN(generic[t]) + 2 < 200)
		PR_snprintf(buf + n, 200 - n, "%s%s", n ? "," : "", generic[t]);
	return buf[0] ? buf : NULL;
}

static char *
fmt_font_weight(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	static const char *const kw[] = {
		NULL, "normal", "bold", "bolder", "lighter", "100", "200", "300",
		"400", "500", "600", "700", "800", "900"
	};
	uint8_t t = css_computed_font_weight(st);

	return t < sizeof kw / sizeof kw[0] ? (char *) kw[t] : NULL;
}

static char *
fmt_font_style(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	switch (css_computed_font_style(st)) {
	case CSS_FONT_STYLE_NORMAL: return "normal";
	case CSS_FONT_STYLE_ITALIC:
	case CSS_FONT_STYLE_OBLIQUE: return "italic";	/* no oblique in layout */
	default: return NULL;
	}
}

static char *
fmt_text_transform(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	switch (css_computed_text_transform(st)) {
	case CSS_TEXT_TRANSFORM_CAPITALIZE: return "capitalize";
	case CSS_TEXT_TRANSFORM_UPPERCASE: return "uppercase";
	case CSS_TEXT_TRANSFORM_LOWERCASE: return "lowercase";
	case CSS_TEXT_TRANSFORM_NONE: return "none";
	default: return NULL;
	}
}

static char *
fmt_text_align(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	switch (css_computed_text_align(st)) {
	case CSS_TEXT_ALIGN_LEFT:
	case CSS_TEXT_ALIGN_LIBCSS_LEFT:
	case CSS_TEXT_ALIGN_DEFAULT: return "left";
	case CSS_TEXT_ALIGN_RIGHT:
	case CSS_TEXT_ALIGN_LIBCSS_RIGHT: return "right";
	case CSS_TEXT_ALIGN_CENTER:
	case CSS_TEXT_ALIGN_LIBCSS_CENTER: return "center";
	case CSS_TEXT_ALIGN_JUSTIFY: return "justify";
	default: return NULL;
	}
}

static char *
fmt_text_indent(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	css_fixed len;
	css_unit unit;

	if (css_computed_text_indent(st, &len, &unit) != CSS_TEXT_INDENT_SET)
		return NULL;
	return nscss_len(doc, buf, len, unit);
}

static char *
fmt_line_height(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	css_fixed len;
	css_unit unit;

	switch (css_computed_line_height(st, &len, &unit)) {
	case CSS_LINE_HEIGHT_NORMAL:
		return "normal";
	case CSS_LINE_HEIGHT_NUMBER:	/* a multiple of the font height */
		PR_snprintf(buf, 40, "%g", FIXTOFLT(len));
		return buf;
	case CSS_LINE_HEIGHT_DIMENSION:
		return nscss_len(doc, buf, len, unit);
	default:
		return NULL;
	}
}

static char *
fmt_list_style_type(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	switch (css_computed_list_style_type(st)) {
	case CSS_LIST_STYLE_TYPE_INHERIT: return NULL;
	case CSS_LIST_STYLE_TYPE_DISC: return "disc";
	case CSS_LIST_STYLE_TYPE_CIRCLE: return "circle";
	case CSS_LIST_STYLE_TYPE_SQUARE: return "square";
	case CSS_LIST_STYLE_TYPE_LOWER_ROMAN: return "lower-roman";
	case CSS_LIST_STYLE_TYPE_UPPER_ROMAN: return "upper-roman";
	case CSS_LIST_STYLE_TYPE_LOWER_ALPHA:
	case CSS_LIST_STYLE_TYPE_LOWER_LATIN: return "lower-alpha";
	case CSS_LIST_STYLE_TYPE_UPPER_ALPHA:
	case CSS_LIST_STYLE_TYPE_UPPER_LATIN: return "upper-alpha";
	case CSS_LIST_STYLE_TYPE_NONE: return "none";
	default: return "decimal";
	}
}

static char *
fmt_white_space(NSCSS_Doc *doc, const css_computed_style *st, char *buf)
{
	switch (css_computed_white_space(st)) {
	case CSS_WHITE_SPACE_NORMAL: return "normal";
	case CSS_WHITE_SPACE_PRE:
	case CSS_WHITE_SPACE_PRE_WRAP: return "pre";
	default: return NULL;		/* nowrap, pre-line: layout has neither */
	}
}

typedef char *(*nscss_fmt)(NSCSS_Doc *, const css_computed_style *, char *);

static const struct {
	char		*name;
	nscss_fmt	 fmt;
} nscss_inherited[] = {
	{ COLOR_PROP,		fmt_color },
	{ FONTSIZE_PROP,	fmt_font_size },
	{ FONTFACE_PROP,	fmt_font_family },
	{ FONTWEIGHT_PROP,	fmt_font_weight },
	{ FONTSTYLE_PROP,	fmt_font_style },
	{ TEXTTRANSFORM_PROP, fmt_text_transform },
	{ TEXTALIGN_PROP,	fmt_text_align },
	{ TEXTINDENT_PROP,	fmt_text_indent },
	{ LINEHEIGHT_PROP,	fmt_line_height },
	{ LISTSTYLETYPE_PROP, fmt_list_style_type },
	{ WHITESPACE_PROP,	fmt_white_space },
};

/* ---- properties that are not inherited ---- */

static void
nscss_box_len(NSCSS_Doc *doc, StyleStruct *style, char *name,
			  uint8_t t, uint8_t set, css_fixed len, css_unit unit,
			  css_fixed mark)
{
	char buf[40];

	if (t == set && !nscss_marked(len, unit, mark))
		nscss_set(style, name, nscss_len(doc, buf, len, unit));
}

/* Is this element hidden the way pages hide things for screen readers
 * only (off screen, clipped away, or one pixel)?  Layout would show it. */
static XP_Bool
nscss_visually_hidden(const css_computed_style *st)
{
	css_fixed len, w, h;
	css_unit unit, wu, hu;
	css_computed_clip_rect r;
	uint8_t pos = css_computed_position(st);

	if (pos != CSS_POSITION_ABSOLUTE && pos != CSS_POSITION_FIXED)
		return FALSE;
	if (css_computed_left(st, &len, &unit) == CSS_LEFT_SET &&
		!nscss_marked(len, unit, NSCSS_FIX_MARK_LEN) && len < INTTOFIX(-999))
		return TRUE;
	if (css_computed_top(st, &len, &unit) == CSS_TOP_SET &&
		!nscss_marked(len, unit, NSCSS_FIX_MARK_LEN) && len < INTTOFIX(-999))
		return TRUE;
	if (css_computed_clip(st, &r) == CSS_CLIP_RECT &&
		!r.top_auto && !r.bottom_auto && !r.left_auto && !r.right_auto &&
		r.bottom - r.top <= INTTOFIX(1) && r.right - r.left <= INTTOFIX(1))
		return TRUE;
	if (css_computed_width(st, &w, &wu) == CSS_WIDTH_SET &&
		css_computed_height(st, &h, &hu) == CSS_HEIGHT_SET &&
		wu == CSS_UNIT_PX && hu == CSS_UNIT_PX &&
		w <= INTTOFIX(1) && h <= INTTOFIX(1))
		return TRUE;
	return FALSE;
}

static XP_Bool
nscss_name_in(NSCSS_Node *n, const char *const *names)
{
	for (; *names; names++)
		if (!strcasecomp(lwc_string_data(n->name), *names))
			return TRUE;
	return FALSE;
}

/* Is the element a block (by its display, or by HTML's default for the
 * tag)?  Layout knows margins, padding, sizes, borders and alignment only
 * as block indents and boxes: on inline elements it would open a new
 * indented block for each one. */
static XP_Bool
nscss_is_block(NSCSS_Node *node, uint8_t display)
{
	static const char *const blocks[] = {
		"html", "body", "div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
		"ul", "ol", "li", "dl", "dt", "dd", "dir", "menu", "pre",
		"blockquote", "center", "address", "form", "fieldset", "legend",
		"table", "caption", "tr", "td", "th", "thead", "tbody", "tfoot",
		"hr", "noscript", "header", "footer", "nav", "section", "article",
		"aside", "main", "figure", "figcaption", "details", "summary",
		"hgroup", "search", NULL
	};

	switch (display) {
	case CSS_DISPLAY_BLOCK:
	case CSS_DISPLAY_LIST_ITEM:
	case CSS_DISPLAY_TABLE:
	case CSS_DISPLAY_FLEX:
	case CSS_DISPLAY_GRID:
	case CSS_DISPLAY_TABLE_CELL:
	case CSS_DISPLAY_TABLE_ROW:
		return TRUE;
	case CSS_DISPLAY_INLINE:
	case CSS_DISPLAY_INLINE_BLOCK:
	case CSS_DISPLAY_INLINE_FLEX:
	case CSS_DISPLAY_INLINE_GRID:
	case CSS_DISPLAY_INLINE_TABLE:
		return FALSE;
	default:					/* not set: HTML's default */
		return nscss_name_in(node, blocks);
	}
}

static void nscss_export_box(NSCSS_Doc *doc, NSCSS_Node *node,
							 const css_computed_style *st, StyleStruct *style);

/* Made fully transparent: a form control (pages draw their own widget
 * over it: menus' checkboxes, custom selects), or a box out of the flow
 * (a dropdown menu faded out until hover).  Layout would show them. */
static XP_Bool
nscss_invisible_control(NSCSS_Node *node, const css_computed_style *st)
{
	const char *n = lwc_string_data(node->name);
	css_fixed op = INTTOFIX(1);
	uint8_t pos = css_computed_position(st);

	if (strcasecomp(n, "input") && strcasecomp(n, "select") &&
		strcasecomp(n, "textarea") && strcasecomp(n, "button") &&
		pos != CSS_POSITION_ABSOLUTE && pos != CSS_POSITION_FIXED)
		return FALSE;
	if (css_computed_opacity(st, &op) != CSS_OPACITY_SET)
		return FALSE;
	return op <= 0;
}

/* Tags layout lays out as blocks whatever their display (lists move the
 * margin, tables and headings break lines): never flex row items. */
static XP_Bool
nscss_layout_block(const char *n)
{
	static const char *const tags[] = {
		"ul", "ol", "dl", "menu", "dir", "table", "form", "p", "pre",
		"blockquote", "center", "hr", "h1", "h2", "h3", "h4", "h5", "h6",
		"address", "listing", "xmp", "multicol", NULL
	};
	int k;

	for (k = 0; tags[k]; k++)
		if (!strcasecomp(n, tags[k]))
			return TRUE;
	return FALSE;
}

static void
nscss_export_flex_container(NSCSS_Doc *doc, const css_computed_style *st,
							StyleStruct *style)
{
	char buf[64];
	css_fixed len;
	css_unit unit;

	switch (css_computed_flex_direction(st)) {
	case CSS_FLEX_DIRECTION_ROW_REVERSE: nscss_set(style, FLEX_PROP, "row-reverse"); break;
	case CSS_FLEX_DIRECTION_COLUMN: nscss_set(style, FLEX_PROP, "column"); break;
	case CSS_FLEX_DIRECTION_COLUMN_REVERSE: nscss_set(style, FLEX_PROP, "column-reverse"); break;
	default: nscss_set(style, FLEX_PROP, "row"); break;
	}
	switch (css_computed_justify_content(st)) {
	case CSS_JUSTIFY_CONTENT_FLEX_END: nscss_set(style, FLEXJUSTIFY_PROP, "end"); break;
	case CSS_JUSTIFY_CONTENT_CENTER: nscss_set(style, FLEXJUSTIFY_PROP, "center"); break;
	case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: nscss_set(style, FLEXJUSTIFY_PROP, "space-between"); break;
	case CSS_JUSTIFY_CONTENT_SPACE_AROUND: nscss_set(style, FLEXJUSTIFY_PROP, "space-around"); break;
	case CSS_JUSTIFY_CONTENT_SPACE_EVENLY: nscss_set(style, FLEXJUSTIFY_PROP, "space-evenly"); break;
	default: break;				/* start */
	}
	switch (css_computed_align_items(st)) {
	case CSS_ALIGN_ITEMS_FLEX_START: nscss_set(style, FLEXALIGN_PROP, "start"); break;
	case CSS_ALIGN_ITEMS_FLEX_END: nscss_set(style, FLEXALIGN_PROP, "end"); break;
	case CSS_ALIGN_ITEMS_CENTER: nscss_set(style, FLEXALIGN_PROP, "center"); break;
	case CSS_ALIGN_ITEMS_BASELINE: nscss_set(style, FLEXALIGN_PROP, "baseline"); break;
	default: break;				/* stretch */
	}
	switch (css_computed_flex_wrap(st)) {
	case CSS_FLEX_WRAP_WRAP: nscss_set(style, FLEXWRAP_PROP, "wrap"); break;
	case CSS_FLEX_WRAP_WRAP_REVERSE: nscss_set(style, FLEXWRAP_PROP, "wrap-reverse"); break;
	default: break;
	}
	if (css_computed_column_gap(st, &len, &unit) == CSS_COLUMN_GAP_SET &&
		len > 0 && unit != CSS_UNIT_PCT)
		nscss_set(style, FLEXGAP_PROP, nscss_len(doc, buf, len, unit));
}

static void
nscss_export_flex_item(NSCSS_Doc *doc, const css_computed_style *st,
					   StyleStruct *style)
{
	char buf[96], basis[48];
	css_fixed grow = 0, shrink = INTTOFIX(1), len;
	css_unit unit;
	uint8_t b;
	int32_t order = 0;

	css_computed_flex_grow(st, &grow);
	css_computed_flex_shrink(st, &shrink);
	b = css_computed_flex_basis(st, &len, &unit);
	if (b == CSS_FLEX_BASIS_SET && unit != CSS_UNIT_PCT)
		nscss_len(doc, basis, len, unit);
	else if (b == CSS_FLEX_BASIS_SET && unit == CSS_UNIT_PCT)
		PR_snprintf(basis, sizeof basis, "%g%%", FIXTOFLT(len));
	else
		XP_STRCPY(basis, b == CSS_FLEX_BASIS_CONTENT ? "content" : "auto");
	css_computed_order(st, &order);
	PR_snprintf(buf, sizeof buf, "%g %g %s %ld", FIXTOFLT(grow),
				FIXTOFLT(shrink), basis, (long) order);
	nscss_set(style, FLEXITEM_PROP, buf);
	switch (css_computed_align_self(st)) {
	case CSS_ALIGN_SELF_STRETCH: nscss_set(style, FLEXALIGNSELF_PROP, "stretch"); break;
	case CSS_ALIGN_SELF_FLEX_START: nscss_set(style, FLEXALIGNSELF_PROP, "start"); break;
	case CSS_ALIGN_SELF_FLEX_END: nscss_set(style, FLEXALIGNSELF_PROP, "end"); break;
	case CSS_ALIGN_SELF_CENTER: nscss_set(style, FLEXALIGNSELF_PROP, "center"); break;
	case CSS_ALIGN_SELF_BASELINE: nscss_set(style, FLEXALIGNSELF_PROP, "baseline"); break;
	default: break;				/* auto: the container's */
	}
}

/* A track list as layout reads it: lengths in units it converts (px, em,
 * pt, ...), the others (rem, vw, ch, ...) made so by nscss_len; fr, %,
 * keywords, names and functions as they are. */
static void
nscss_set_tracks(NSCSS_Doc *doc, StyleStruct *style, char *name,
				 lwc_string *tracks)
{
	static const struct { const char *u; css_unit unit; } units[] = {
		{ "rem", CSS_UNIT_REM }, { "vw", CSS_UNIT_VW }, { "vh", CSS_UNIT_VH },
		{ "vmin", CSS_UNIT_VMIN }, { "vmax", CSS_UNIT_VMAX },
		{ "ch", CSS_UNIT_CH }, { "lh", CSS_UNIT_LH }, { "q", CSS_UNIT_Q },
		{ "vi", CSS_UNIT_VI }, { "vb", CSS_UNIT_VB }
	};
	const char *p, *end;
	char *out, *o, num[48];
	size_t n;

	if (tracks == NULL)
		return;
	p = lwc_string_data(tracks);
	end = p + lwc_string_length(tracks);
	out = o = XP_ALLOC(lwc_string_length(tracks) * 4 + 16);
	if (out == NULL)
		return;
	while (p < end) {
		const char *q = p;
		char *e;
		double v;
		size_t k, ul = 0;

		/* a number starts a token (not inside a name: "col-1") */
		if ((isdigit((unsigned char) *p) || *p == '.' ||
			 ((*p == '-' || *p == '+') && p + 1 < end &&
			  (isdigit((unsigned char) p[1]) || p[1] == '.'))) &&
			(o == out || !(isalnum((unsigned char) o[-1]) ||
						   o[-1] == '-' || o[-1] == '_'))) {
			v = strtod(p, &e);
			q = e;
			while (q + ul < end && isalpha((unsigned char) q[ul]))
				ul++;
			for (k = 0; k < sizeof units / sizeof units[0]; k++)
				if (strlen(units[k].u) == ul &&
					!strncasecomp(q, units[k].u, ul))
					break;
			if (k < sizeof units / sizeof units[0] &&
				nscss_len(doc, num, FLTTOFIX(v), units[k].unit)) {
				n = strlen(num);
				XP_MEMCPY(o, num, n);
				o += n;
				p = q + ul;
				continue;
			}
			n = (q + ul) - p;
			XP_MEMCPY(o, p, n);
			o += n;
			p = q + ul;
			continue;
		}
		*o++ = *p++;
	}
	*o = '\0';
	nscss_set(style, name, out);
	XP_FREE(out);
}

static void
nscss_export_grid_container(NSCSS_Doc *doc, const css_computed_style *st,
							StyleStruct *style)
{
	char buf[64];
	css_fixed len;
	css_unit unit;
	lwc_string *s;

	nscss_set(style, FLEX_PROP, "grid");
	if (css_computed_grid_template_columns(st, &s) && s)
		nscss_set_tracks(doc, style, GRIDCOLS_PROP, s);
	if (css_computed_grid_template_rows(st, &s) && s)
		nscss_set_tracks(doc, style, GRIDROWS_PROP, s);
	if (css_computed_grid_template_areas(st, &s) && s)
		nscss_set(style, GRIDAREAS_PROP, lwc_string_data(s));
	if (css_computed_grid_auto_columns(st, &s) && s)
		nscss_set_tracks(doc, style, GRIDAUTOCOLS_PROP, s);
	if (css_computed_grid_auto_rows(st, &s) && s)
		nscss_set_tracks(doc, style, GRIDAUTOROWS_PROP, s);
	switch (css_computed_grid_auto_flow(st)) {
	case CSS_GRID_AUTO_FLOW_COLUMN: nscss_set(style, GRIDFLOW_PROP, "column"); break;
	case CSS_GRID_AUTO_FLOW_ROW_DENSE: nscss_set(style, GRIDFLOW_PROP, "row dense"); break;
	case CSS_GRID_AUTO_FLOW_COLUMN_DENSE: nscss_set(style, GRIDFLOW_PROP, "column dense"); break;
	default: break;				/* row */
	}
	if (css_computed_column_gap(st, &len, &unit) == CSS_COLUMN_GAP_SET &&
		len > 0) {
		if (unit == CSS_UNIT_PCT)
			PR_snprintf(buf, sizeof buf, "%g%%", FIXTOFLT(len));
		if (unit == CSS_UNIT_PCT || nscss_len(doc, buf, len, unit))
			nscss_set(style, FLEXGAP_PROP, buf);
	}
	if (css_computed_row_gap(st, &len, &unit) == CSS_ROW_GAP_SET &&
		len > 0) {
		if (unit == CSS_UNIT_PCT)
			PR_snprintf(buf, sizeof buf, "%g%%", FIXTOFLT(len));
		if (unit == CSS_UNIT_PCT || nscss_len(doc, buf, len, unit))
			nscss_set(style, GRIDROWGAP_PROP, buf);
	}
	switch (css_computed_align_items(st)) {
	case CSS_ALIGN_ITEMS_FLEX_START: nscss_set(style, FLEXALIGN_PROP, "start"); break;
	case CSS_ALIGN_ITEMS_FLEX_END: nscss_set(style, FLEXALIGN_PROP, "end"); break;
	case CSS_ALIGN_ITEMS_CENTER: nscss_set(style, FLEXALIGN_PROP, "center"); break;
	case CSS_ALIGN_ITEMS_BASELINE: nscss_set(style, FLEXALIGN_PROP, "start"); break;
	default: break;				/* stretch */
	}
	switch (css_computed_justify_items(st)) {
	case CSS_JUSTIFY_ITEMS_START:
	case CSS_JUSTIFY_ITEMS_LEFT:
	case CSS_JUSTIFY_ITEMS_BASELINE: nscss_set(style, GRIDJUSTIFY_PROP, "start"); break;
	case CSS_JUSTIFY_ITEMS_END: nscss_set(style, GRIDJUSTIFY_PROP, "end"); break;
	case CSS_JUSTIFY_ITEMS_CENTER: nscss_set(style, GRIDJUSTIFY_PROP, "center"); break;
	default: break;				/* normal, stretch */
	}
	switch (css_computed_justify_content(st)) {
	case CSS_JUSTIFY_CONTENT_FLEX_END: nscss_set(style, FLEXJUSTIFY_PROP, "end"); break;
	case CSS_JUSTIFY_CONTENT_CENTER: nscss_set(style, FLEXJUSTIFY_PROP, "center"); break;
	case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: nscss_set(style, FLEXJUSTIFY_PROP, "space-between"); break;
	case CSS_JUSTIFY_CONTENT_SPACE_AROUND: nscss_set(style, FLEXJUSTIFY_PROP, "space-around"); break;
	case CSS_JUSTIFY_CONTENT_SPACE_EVENLY: nscss_set(style, FLEXJUSTIFY_PROP, "space-evenly"); break;
	default: break;				/* normal, start */
	}
	switch (css_computed_align_content(st)) {
	case CSS_ALIGN_CONTENT_FLEX_START: nscss_set(style, GRIDALIGNCONTENT_PROP, "start"); break;
	case CSS_ALIGN_CONTENT_FLEX_END: nscss_set(style, GRIDALIGNCONTENT_PROP, "end"); break;
	case CSS_ALIGN_CONTENT_CENTER: nscss_set(style, GRIDALIGNCONTENT_PROP, "center"); break;
	case CSS_ALIGN_CONTENT_SPACE_BETWEEN: nscss_set(style, GRIDALIGNCONTENT_PROP, "space-between"); break;
	case CSS_ALIGN_CONTENT_SPACE_AROUND: nscss_set(style, GRIDALIGNCONTENT_PROP, "space-around"); break;
	case CSS_ALIGN_CONTENT_SPACE_EVENLY: nscss_set(style, GRIDALIGNCONTENT_PROP, "space-evenly"); break;
	default: break;				/* normal, stretch */
	}
}

static void
nscss_export_grid_item(const css_computed_style *st, StyleStruct *style)
{
	char buf[400];
	lwc_string *l[4];
	int k;

	css_computed_grid_row_start(st, &l[0]);
	css_computed_grid_column_start(st, &l[1]);
	css_computed_grid_row_end(st, &l[2]);
	css_computed_grid_column_end(st, &l[3]);
	if (l[0] || l[1] || l[2] || l[3]) {
		buf[0] = '\0';
		for (k = 0; k < 4; k++)
			PR_snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s%s",
						k ? " / " : "", l[k] ? lwc_string_data(l[k]) : "auto");
		nscss_set(style, GRIDITEM_PROP, buf);
	}
	switch (css_computed_justify_self(st)) {
	case CSS_JUSTIFY_SELF_START:
	case CSS_JUSTIFY_SELF_LEFT:
	case CSS_JUSTIFY_SELF_BASELINE: nscss_set(style, GRIDJUSTIFYSELF_PROP, "start"); break;
	case CSS_JUSTIFY_SELF_END: nscss_set(style, GRIDJUSTIFYSELF_PROP, "end"); break;
	case CSS_JUSTIFY_SELF_CENTER: nscss_set(style, GRIDJUSTIFYSELF_PROP, "center"); break;
	case CSS_JUSTIFY_SELF_NORMAL:
	case CSS_JUSTIFY_SELF_STRETCH: nscss_set(style, GRIDJUSTIFYSELF_PROP, "stretch"); break;
	default: break;				/* auto: the container's justify-items */
	}
}

/* An absolutely or relatively positioned element: its layer.  Offsets are
 * layout's left and top (from the containing layer); a relative one's right
 * or bottom (with left or top auto) are the same moves the other way.
 * Absolute right and bottom need the containing block's size, which layout
 * does not give: not yet. */
static void
nscss_export_position(NSCSS_Doc *doc, const css_computed_style *st,
					  StyleStruct *style)
{
	char buf[64];
	css_fixed len;
	css_unit unit;
	int32_t z;
	uint8_t pos = css_computed_position(st);
	XP_Bool relative = pos == CSS_POSITION_RELATIVE;

	if (pos != CSS_POSITION_ABSOLUTE && pos != CSS_POSITION_FIXED &&
		!relative)
		return;
	nscss_set(style, POSITION_PROP, relative ? "relative" : "absolute");
	if (css_computed_top(st, &len, &unit) == CSS_TOP_SET)
		nscss_set(style, TOP_PROP, nscss_len(doc, buf, len, unit));
	else if (relative &&
			 css_computed_bottom(st, &len, &unit) == CSS_BOTTOM_SET)
		nscss_set(style, TOP_PROP, nscss_len(doc, buf, -len, unit));
	if (css_computed_left(st, &len, &unit) == CSS_LEFT_SET)
		nscss_set(style, LEFT_PROP, nscss_len(doc, buf, len, unit));
	else if (relative &&
			 css_computed_right(st, &len, &unit) == CSS_RIGHT_SET)
		nscss_set(style, LEFT_PROP, nscss_len(doc, buf, -len, unit));
	if (css_computed_z_index(st, &z) == CSS_Z_INDEX_SET) {
		PR_snprintf(buf, sizeof buf, "%ld", (long) z);
		nscss_set(style, ZINDEX_PROP, buf);
	}
}

static const char *nscss_attr_named(NSCSS_Node *node, const char *name,
									size_t len);

/* A table's cell spacing: border-spacing (horizontal), none if the
 * borders collapse */
static void
nscss_table_spacing(NSCSS_Doc *doc, const css_computed_style *st,
					StyleStruct *style)
{
	css_fixed hs = 0, vs = 0;
	css_unit hu = CSS_UNIT_PX, vu = CSS_UNIT_PX;
	char buf[40];

	if (css_computed_table_layout(st) == CSS_TABLE_LAYOUT_FIXED)
		nscss_set(style, TABLELAYOUT_PROP, "fixed");
	if (css_computed_border_collapse(st) == CSS_BORDER_COLLAPSE_COLLAPSE) {
		nscss_set(style, TABLESPACING_PROP, "0px");
		return;
	}
	if (css_computed_border_spacing(st, &hs, &hu, &vs, &vu) ==
		CSS_BORDER_SPACING_SET && hs >= 0 && nscss_len(doc, buf, hs, hu))
		nscss_set(style, TABLESPACING_PROP, buf);
}

/* HTML's table tags (layout makes their tables itself) */
static const char *const nscss_table_tags_x[] = {
	"table", "caption", "thead", "tbody", "tfoot", "tr", "td", "th",
	"col", "colgroup", NULL
};

/* ---- gradients (background-image: linear-gradient(...) and the like):
 * libcss keeps them as "nscss-gradient:" + their text; layout draws images,
 * so they become one: a PNG in a data: URL, or a colour if they are one
 * colour ---------------------------------------------------------------- */

typedef struct {
	double r, g, b, a;
} nscss_rgba;

static const struct { const char *name; unsigned rgb; } nscss_named[] = {
	{ "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 },
	{ "green", 0x008000 }, { "lime", 0x00ff00 }, { "blue", 0x0000ff },
	{ "yellow", 0xffff00 }, { "orange", 0xffa500 }, { "purple", 0x800080 },
	{ "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xc0c0c0 },
	{ "navy", 0x000080 }, { "teal", 0x008080 }, { "aqua", 0x00ffff },
	{ "cyan", 0x00ffff }, { "fuchsia", 0xff00ff }, { "magenta", 0xff00ff },
	{ "maroon", 0x800000 }, { "olive", 0x808000 }, { "pink", 0xffc0cb },
	{ "brown", 0xa52a2a }, { "gold", 0xffd700 }, { "indigo", 0x4b0082 },
	{ "violet", 0xee82ee }, { "lightgray", 0xd3d3d3 },
	{ "lightgrey", 0xd3d3d3 }, { "darkgray", 0xa9a9a9 },
	{ "darkgrey", 0xa9a9a9 }, { "lightblue", 0xadd8e6 },
	{ "darkblue", 0x00008b }, { "darkgreen", 0x006400 },
	{ "darkred", 0x8b0000 }, { "skyblue", 0x87ceeb },
	{ "steelblue", 0x4682b4 }, { "tomato", 0xff6347 },
	{ "crimson", 0xdc143c }, { "coral", 0xff7f50 },
	{ "salmon", 0xfa8072 }, { "khaki", 0xf0e68c }, { "beige", 0xf5f5dc },
	{ "ivory", 0xfffff0 }, { "lavender", 0xe6e6fa },
	{ "whitesmoke", 0xf5f5f5 }, { "gainsboro", 0xdcdcdc },
	{ "rebeccapurple", 0x663399 }, { NULL, 0 }
};

/* the numbers in "fn(a, b c / d)" (percentages as fractions of PCT_OF) */
static int
nscss_fn_args(const char *s, double *v, int max, double pct_of)
{
	int n = 0;
	char *e;

	while (*s && *s != '(')
		s++;
	while (*s && n < max) {
		double d;

		while (*s && (*s == '(' || *s == ',' || *s == ' ' || *s == '/'))
			s++;
		if (!*s || *s == ')')
			break;
		d = strtod(s, &e);
		if (e == s)
			break;
		if (*e == '%') {
			d = d * pct_of / 100;
			e++;
		} else if (!strncasecomp(e, "deg", 3)) {
			e += 3;
		} else if (!strncasecomp(e, "turn", 4)) {
			d *= 360;
			e += 4;
		}
		v[n++] = d;
		s = e;
	}
	return n;
}

static double
nscss_hue(double p, double q, double t)
{
	if (t < 0) t += 1;
	if (t > 1) t -= 1;
	if (t < 1.0 / 6) return p + (q - p) * 6 * t;
	if (t < 0.5) return q;
	if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
	return p;
}

/* a colour (#hex, rgb(), rgba(), hsl(), hsla(), a name, transparent) */
static XP_Bool
nscss_parse_color(const char *s, nscss_rgba *c)
{
	double v[4];
	int n, k;

	c->a = 1;
	if (s[0] == '#') {
		unsigned long x = strtoul(s + 1, NULL, 16);
		size_t len = strspn(s + 1, "0123456789abcdefABCDEF");

		if (len == 3 || len == 4) {
			unsigned long r = (x >> (len == 4 ? 12 : 8)) & 15;
			unsigned long g = (x >> (len == 4 ? 8 : 4)) & 15;
			unsigned long b = (x >> (len == 4 ? 4 : 0)) & 15;

			c->r = r * 17; c->g = g * 17; c->b = b * 17;
			if (len == 4)
				c->a = (x & 15) / 15.0;
			return TRUE;
		}
		if (len == 6 || len == 8) {
			if (len == 8) {
				c->a = (x & 255) / 255.0;
				x >>= 8;
			}
			c->r = (x >> 16) & 255; c->g = (x >> 8) & 255; c->b = x & 255;
			return TRUE;
		}
		return FALSE;
	}
	if (!strncasecomp(s, "rgb", 3)) {
		n = nscss_fn_args(s, v, 4, 255);
		if (n < 3)
			return FALSE;
		c->r = v[0]; c->g = v[1]; c->b = v[2];
		if (n == 4)
			c->a = v[3] > 1 ? v[3] / 255 : v[3];
		return TRUE;
	}
	if (!strncasecomp(s, "hsl", 3)) {
		double h, sat, l, q, p;

		n = nscss_fn_args(s, v, 4, 1);
		if (n < 3)
			return FALSE;
		h = v[0] / 360; sat = v[1]; l = v[2];
		h -= (long)h;
		if (h < 0) h += 1;
		q = l < 0.5 ? l * (1 + sat) : l + sat - l * sat;
		p = 2 * l - q;
		c->r = 255 * nscss_hue(p, q, h + 1.0 / 3);
		c->g = 255 * nscss_hue(p, q, h);
		c->b = 255 * nscss_hue(p, q, h - 1.0 / 3);
		if (n == 4)
			c->a = v[3];
		return TRUE;
	}
	if (!strncasecomp(s, "transparent", 11)) {
		c->r = c->g = c->b = 0;
		c->a = 0;
		return TRUE;
	}
	for (k = 0; nscss_named[k].name; k++)
		if (!strncasecomp(s, nscss_named[k].name, strlen(nscss_named[k].name)) &&
			!isalpha((unsigned char) s[strlen(nscss_named[k].name)])) {
			c->r = (nscss_named[k].rgb >> 16) & 255;
			c->g = (nscss_named[k].rgb >> 8) & 255;
			c->b = nscss_named[k].rgb & 255;
			return TRUE;
		}
	return FALSE;
}

static unsigned long
nscss_crc(unsigned long crc, const unsigned char *p, size_t n)
{
	static unsigned long table[256];
	static int made;
	size_t i;
	int k;

	if (!made) {
		for (i = 0; i < 256; i++) {
			unsigned long c = i;

			for (k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320UL ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		made = 1;
	}
	crc ^= 0xffffffffUL;
	for (i = 0; i < n; i++)
		crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
	return crc ^ 0xffffffffUL;
}

static void
nscss_be32(unsigned char *p, unsigned long v)
{
	p[0] = (v >> 24) & 255; p[1] = (v >> 16) & 255;
	p[2] = (v >> 8) & 255; p[3] = v & 255;
}

/* A W x H RGB image (RGB, rows of 3 * W bytes) as "data:image/png;base64,"
 * (malloc'd): stored deflate blocks, no compression library. */
static char *
nscss_png_data_url(const unsigned char *rgb, int w, int h)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t raw_len = (size_t) h * (3 * w + 1), z_len, png_len, i, o;
	size_t blocks = raw_len / 65535 + 1;
	unsigned char *raw, *png, *z;
	unsigned long a1 = 1, a2 = 0;
	char *out;
	int y;

	raw = (unsigned char *) XP_ALLOC(raw_len);
	if (!raw)
		return NULL;
	for (y = 0; y < h; y++) {
		raw[y * (3 * w + 1)] = 0;	/* no filter */
		XP_MEMCPY(raw + y * (3 * w + 1) + 1, rgb + (size_t) y * 3 * w, 3 * w);
	}
	z_len = 2 + raw_len + blocks * 5 + 4;
	png_len = 8 + 25 + 12 + z_len + 12;
	png = (unsigned char *) XP_ALLOC(png_len);
	if (!png) {
		XP_FREE(raw);
		return NULL;
	}
	XP_MEMCPY(png, "\x89PNG\r\n\x1a\n", 8);
	/* IHDR */
	nscss_be32(png + 8, 13);
	XP_MEMCPY(png + 12, "IHDR", 4);
	nscss_be32(png + 16, w);
	nscss_be32(png + 20, h);
	png[24] = 8; png[25] = 2; png[26] = 0; png[27] = 0; png[28] = 0;
	nscss_be32(png + 29, nscss_crc(0, png + 12, 17));
	/* IDAT: zlib header, stored blocks, adler32 */
	nscss_be32(png + 33, z_len);
	XP_MEMCPY(png + 37, "IDAT", 4);
	z = png + 41;
	o = 0;
	z[o++] = 0x78;
	z[o++] = 0x01;
	for (i = 0; i < raw_len || i == 0; ) {
		size_t n = raw_len - i > 65535 ? 65535 : raw_len - i;

		z[o++] = i + n >= raw_len ? 1 : 0;
		z[o++] = n & 255; z[o++] = n >> 8;
		z[o++] = ~n & 255; z[o++] = (~n >> 8) & 255;
		XP_MEMCPY(z + o, raw + i, n);
		o += n;
		i += n;
		if (n == 0)
			break;
	}
	for (i = 0; i < raw_len; i++) {
		a1 = (a1 + raw[i]) % 65521;
		a2 = (a2 + a1) % 65521;
	}
	nscss_be32(z + o, (a2 << 16) | a1);
	o += 4;
	nscss_be32(png + 33, o);
	nscss_be32(png + 41 + o, nscss_crc(0, png + 37, 4 + o));
	o += 41 + 4;
	nscss_be32(png + o, 0);
	XP_MEMCPY(png + o + 4, "IEND", 4);
	nscss_be32(png + o + 8, nscss_crc(0, png + o + 4, 4));
	png_len = o + 12;
	XP_FREE(raw);

	out = (char *) XP_ALLOC(22 + (png_len + 2) / 3 * 4 + 1);
	if (out) {
		XP_STRCPY(out, "data:image/png;base64,");
		o = 22;
		for (i = 0; i < png_len; i += 3) {
			unsigned long v = (unsigned long) png[i] << 16;

			if (i + 1 < png_len) v |= (unsigned long) png[i + 1] << 8;
			if (i + 2 < png_len) v |= png[i + 2];
			out[o++] = b64[(v >> 18) & 63];
			out[o++] = b64[(v >> 12) & 63];
			out[o++] = i + 1 < png_len ? b64[(v >> 6) & 63] : '=';
			out[o++] = i + 2 < png_len ? b64[v & 63] : '=';
		}
		out[o] = '\0';
	}
	XP_FREE(png);
	return out;
}

#define NSCSS_MAX_STOPS 16

/* Draw the gradient TEXT ("linear-gradient(...)") on BOX_W x BOX_H (0:
 * unknown) over BG: as a colour (*SOLID) if it has one colour, else as an
 * image (malloc'd data: URL) with how it repeats (*REPEAT) and the colour
 * beyond it (*FILL: its last). */
static char *
nscss_gradient_image(const char *text, int box_w, int box_h, nscss_rgba bg,
					 nscss_rgba *solid, XP_Bool *is_solid, const char **repeat,
					 nscss_rgba *fill)
{
	char args[NSCSS_MAX_STOPS + 2][96];
	nscss_rgba col[NSCSS_MAX_STOPS];
	double pos[NSCSS_MAX_STOPS];
	int nargs = 0, nstops = 0, depth = 0, k, j, first = 0, w, h, x, y;
	double angle = 180, len, period = 0;
	XP_Bool radial = !strncasecomp(text, "radial", 6) ||
		!strncasecomp(text, "repeating-radial", 16);
	XP_Bool repeating = !strncasecomp(text, "repeating", 9);
	const char *p = strchr(text, '(');
	size_t n = 0;
	unsigned char *rgb;
	char *url;

	*is_solid = FALSE;
	if (!p)
		return NULL;
	/* the arguments, split at commas outside parentheses */
	for (p++; *p && nargs < NSCSS_MAX_STOPS + 2; p++) {
		if (*p == '(')
			depth++;
		if (*p == ')' && depth-- == 0)
			break;
		if (*p == ',' && depth == 0) {
			args[nargs][n] = '\0';
			nargs++;
			n = 0;
			continue;
		}
		if (n == 0 && *p == ' ')
			continue;
		if (n < sizeof args[0] - 1)
			args[nargs][n++] = *p;
	}
	args[nargs][n] = '\0';
	nargs++;
	/* the direction (linear) or shape (radial: ignored) */
	if (radial) {
		if (!nscss_parse_color(args[0], &col[0]))
			first = 1;
	} else if (!strncasecomp(args[0], "to ", 3)) {
		XP_Bool top = strstr(args[0], "top") != NULL;
		XP_Bool bottom = strstr(args[0], "bottom") != NULL;
		XP_Bool left = strstr(args[0], "left") != NULL;
		XP_Bool right = strstr(args[0], "right") != NULL;

		angle = top ? (left ? 315 : right ? 45 : 0)
			: bottom ? (left ? 225 : right ? 135 : 180)
			: left ? 270 : 90;
		first = 1;
	} else if (isdigit((unsigned char) args[0][0]) || args[0][0] == '-' ||
			   args[0][0] == '.') {
		char *e;

		angle = strtod(args[0], &e);
		if (!strncasecomp(e, "turn", 4))
			angle *= 360;
		else if (!strncasecomp(e, "rad", 3))
			angle = angle * 180 / 3.14159265358979;
		else if (!strncasecomp(e, "grad", 4))
			angle = angle * 0.9;
		first = 1;
	}
	/* the colour stops: a colour and (perhaps) a position */
	for (k = first; k < nargs && nstops < NSCSS_MAX_STOPS; k++) {
		char *sp;

		if (!nscss_parse_color(args[k], &col[nstops]))
			continue;
		pos[nstops] = -1;
		/* the position: after the colour (past its parentheses) */
		sp = args[k];
		if (strchr(sp, ')'))
			sp = strrchr(sp, ')') + 1;
		else
			sp = strchr(sp, ' ');
		while (sp && *sp == ' ')
			sp++;
		if (sp && *sp) {
			char *e;
			double d = strtod(sp, &e);

			if (e != sp)
				pos[nstops] = *e == '%' ? d / 100 : -2 - d;	/* px: later */
		}
		nstops++;
	}
	if (nstops == 0)
		return NULL;
	/* alpha: over the box's background */
	for (k = 0; k < nstops; k++) {
		col[k].r = col[k].r * col[k].a + bg.r * (1 - col[k].a);
		col[k].g = col[k].g * col[k].a + bg.g * (1 - col[k].a);
		col[k].b = col[k].b * col[k].a + bg.b * (1 - col[k].a);
		col[k].a = 1;
	}
	*fill = col[nstops - 1];
	for (k = 1; k < nstops; k++)
		if ((int) col[k].r != (int) col[0].r || (int) col[k].g != (int) col[0].g ||
			(int) col[k].b != (int) col[0].b)
			break;
	if (k == nstops) {
		*solid = col[0];
		*is_solid = TRUE;
		return NULL;
	}
	/* the image: a strip along the gradient if it is vertical or
	 * horizontal (the other way it repeats), else the box */
	while (angle < 0) angle += 360;
	while (angle >= 360) angle -= 360;
	if (radial) {
		w = box_w > 0 ? box_w : 100;
		h = box_h > 0 ? box_h : 100;
		*repeat = "no-repeat";
	} else if (angle == 180 || angle == 0) {
		w = 1;
		h = box_h > 0 ? box_h : 100;
		*repeat = "repeat-x";
	} else if (angle == 90 || angle == 270) {
		w = box_w > 0 ? box_w : 300;
		h = 1;
		*repeat = "repeat-y";
	} else if (box_w > 0 && box_h > 0 && (long) box_w * box_h <= 160000) {
		w = box_w;
		h = box_h;
		*repeat = "no-repeat";
	} else {
		/* (no size: the nearer axis) */
		w = 1;
		h = box_h > 0 ? box_h : 100;
		angle = (angle > 90 && angle < 270) ? 180 : 0;
		*repeat = "repeat-x";
	}
	{
		double a = angle * 3.14159265358979 / 180;
		double sx = sin(a), sy = -cos(a);

		len = radial ? sqrt((double) w * w + (double) h * h) / 2
			: fabs(w * sx) + fabs(h * sy);
		if (len <= 0)
			len = 1;
		/* positions: px into fractions, missing ones spread evenly */
		for (k = 0; k < nstops; k++)
			if (pos[k] <= -2)
				pos[k] = (-2 - pos[k]) / len;
		if (pos[0] < 0)
			pos[0] = 0;
		if (pos[nstops - 1] < 0)
			pos[nstops - 1] = 1;
		for (k = 1; k < nstops; k++) {
			if (pos[k] >= 0) {
				if (pos[k] < pos[k - 1])
					pos[k] = pos[k - 1];
				continue;
			}
			for (j = k; j < nstops && pos[j] < 0; j++)
				;
			{
				int m;

				for (m = k; m < j; m++)
					pos[m] = pos[k - 1] + (pos[j] - pos[k - 1]) *
						(m - k + 1) / (j - k + 1);
			}
		}
		period = pos[nstops - 1] - pos[0];
		rgb = (unsigned char *) XP_ALLOC((size_t) w * h * 3);
		if (!rgb)
			return NULL;
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				double t, dx = x + 0.5 - w / 2.0, dy = y + 0.5 - h / 2.0;
				nscss_rgba c;
				unsigned char *o = rgb + ((size_t) y * w + x) * 3;

				t = radial ? sqrt(dx * dx + dy * dy) / len
					: (dx * sx + dy * sy) / len + 0.5;
				if (repeating && period > 0) {
					t = (t - pos[0]) / period;
					t = pos[0] + (t - floor(t)) * period;
				}
				if (t <= pos[0])
					c = col[0];
				else if (t >= pos[nstops - 1])
					c = col[nstops - 1];
				else {
					for (k = 1; k < nstops && pos[k] < t; k++)
						;
					{
						double span = pos[k] - pos[k - 1];
						double f = span > 0 ? (t - pos[k - 1]) / span : 1;

						c.r = col[k - 1].r + (col[k].r - col[k - 1].r) * f;
						c.g = col[k - 1].g + (col[k].g - col[k - 1].g) * f;
						c.b = col[k - 1].b + (col[k].b - col[k - 1].b) * f;
					}
				}
				o[0] = (unsigned char) (c.r + 0.5);
				o[1] = (unsigned char) (c.g + 0.5);
				o[2] = (unsigned char) (c.b + 0.5);
			}
	}
	url = nscss_png_data_url(rgb, w, h);
	XP_FREE(rgb);
	return url;
}

/* A length in a value's text ("10px", "1.5em", "50%"): pixels, or (a
 * percentage) -1 with *PCT set; FONT is the element's font size (px) */
static double
nscss_text_len(NSCSS_Doc *doc, const char *s, double font, double *pct)
{
	char *e;
	double v = strtod(s, &e);
	css_unit unit = CSS_UNIT_PX;
	char buf[48];

	*pct = -1;
	if (e == s)
		return 0;
	if (*e == '%') {
		*pct = v;
		return -1;
	}
	if (!strncasecomp(e, "em", 2))
		return v * font;
	if (!strncasecomp(e, "rem", 3))
		unit = CSS_UNIT_REM;
	else if (!strncasecomp(e, "pt", 2))
		return v * 4 / 3;
	else if (!strncasecomp(e, "vw", 2))
		unit = CSS_UNIT_VW;
	else if (!strncasecomp(e, "vh", 2))
		unit = CSS_UNIT_VH;
	else
		return v;				/* px, or no unit (0) */
	if (nscss_len(doc, buf, FLTTOFIX(v), unit))
		return atof(buf);
	return v;
}

/* border-radius and box-shadow, for a block's box */
static void
nscss_export_decorations(NSCSS_Doc *doc, const css_computed_style *st,
						 StyleStruct *style)
{
	lwc_string *r[4], *sh = NULL;
	css_fixed fs;
	css_unit fu;
	double font = 16, pct;
	char out[200];
	int k;

	if (css_computed_font_size(st, &fs, &fu) == CSS_FONT_SIZE_DIMENSION &&
		fu == CSS_UNIT_PX)
		font = FIXTOFLT(fs);
	css_computed_border_top_left_radius(st, &r[0]);
	css_computed_border_top_right_radius(st, &r[1]);
	css_computed_border_bottom_right_radius(st, &r[2]);
	css_computed_border_bottom_left_radius(st, &r[3]);
	if (r[0] || r[1] || r[2] || r[3]) {
		out[0] = '\0';
		for (k = 0; k < 4; k++) {
			const char *t = r[k] ? lwc_string_data(r[k]) : "0";
			const char *v2 = strchr(t, ' ');
			double h = nscss_text_len(doc, t, font, &pct), hp = pct;
			double v = v2 ? nscss_text_len(doc, v2 + 1, font, &pct) : h;
			double vp = v2 ? pct : hp;
			size_t n = strlen(out);

			if (hp >= 0)
				PR_snprintf(out + n, sizeof out - n, "p%g ", hp);
			else
				PR_snprintf(out + n, sizeof out - n, "%d ", (int) (h + 0.5));
			n = strlen(out);
			if (vp >= 0)
				PR_snprintf(out + n, sizeof out - n, "p%g ", vp);
			else
				PR_snprintf(out + n, sizeof out - n, "%d ", (int) (v + 0.5));
		}
		nscss_set(style, RADIUS_PROP, out);
	}
	/* the first shadow that is not inset: offsets, blur, colour */
	css_computed_box_shadow(st, &sh);
	if (sh) {
		const char *t = lwc_string_data(sh);
		double num[4];
		int nn = 0;
		nscss_rgba c;
		XP_Bool have_color = FALSE, inset = FALSE;
		const char *p = t;

		c.r = c.g = c.b = 0;
		c.a = 1;
		while (*p && *p != ',') {
			while (*p == ' ')
				p++;
			if (!*p || *p == ',')
				break;
			if (!strncasecomp(p, "inset", 5))
				inset = TRUE;
			else if (isdigit((unsigned char) *p) || *p == '-' || *p == '.') {
				if (nn < 4)
					num[nn++] = nscss_text_len(doc, p, font, &pct);
			} else if (nscss_parse_color(p, &c))
				have_color = TRUE;
			/* past the token (and a function's parentheses) */
			{
				int depth = 0;

				while (*p && (depth > 0 || (*p != ' ' && *p != ','))) {
					if (*p == '(')
						depth++;
					else if (*p == ')')
						depth--;
					p++;
				}
			}
		}
		if (nn >= 2 && !inset) {
			if (!have_color) {
				c.r = c.g = c.b = 0;
				c.a = 0.5;
			}
			PR_snprintf(out, sizeof out, "%d %d %d #%02x%02x%02x%02x",
						(int) num[0], (int) num[1], nn > 2 ? (int) num[2] : 0,
						(int) (c.r + 0.5), (int) (c.g + 0.5), (int) (c.b + 0.5),
						(int) (c.a * 255 + 0.5));
			nscss_set(style, BOXSHADOW_PROP, out);
		}
	}
}

/* A block's gradient background: its image, or its colour */
static void
nscss_export_gradient(NSCSS_Doc *doc, const css_computed_style *st,
					  StyleStruct *style, const char *text)
{
	css_fixed len;
	css_unit unit;
	css_color c;
	nscss_rgba bg, solid, fill;
	XP_Bool is_solid;
	const char *repeat = "repeat";
	int w = 0, h = 0;
	char *url, buf[40];

	if (css_computed_width(st, &len, &unit) == CSS_WIDTH_SET &&
		unit == CSS_UNIT_PX && !nscss_marked(len, unit, NSCSS_FIX_MARK_POS))
		w = FIXTOINT(len);
	if (css_computed_height(st, &len, &unit) == CSS_HEIGHT_SET &&
		unit == CSS_UNIT_PX && !nscss_marked(len, unit, NSCSS_FIX_MARK_POS))
		h = FIXTOINT(len);
	bg.r = bg.g = bg.b = 255;
	bg.a = 1;
	if (css_computed_background_color(st, &c) == CSS_BACKGROUND_COLOR_COLOR &&
		c != NSCSS_COLOR_MARK && (c >> 24) != 0) {
		bg.r = (c >> 16) & 255; bg.g = (c >> 8) & 255; bg.b = c & 255;
	}
	url = nscss_gradient_image(text, w, h, bg, &solid, &is_solid, &repeat,
							   &fill);
	if (is_solid) {
		PR_snprintf(buf, sizeof buf, "#%02x%02x%02x", (int) (solid.r + 0.5),
					(int) (solid.g + 0.5), (int) (solid.b + 0.5));
		nscss_set(style, BGCOLOR_PROP, buf);
		nscss_set(style, BGIMAGE_PROP, "none");
		return;
	}
	if (!url)
		return;
	{
		char *v = PR_smprintf("url(%s)", url);

		if (v) {
			nscss_set(style, BGIMAGE_PROP, v);
			XP_FREE(v);
		}
	}
	XP_FREE(url);
	nscss_set(style, BGREPEAT_PROP, repeat);
	/* beyond the image: the last colour (unless the box has one) */
	if (!(css_computed_background_color(st, &c) == CSS_BACKGROUND_COLOR_COLOR &&
		  c != NSCSS_COLOR_MARK && (c >> 24) != 0)) {
		PR_snprintf(buf, sizeof buf, "#%02x%02x%02x", (int) (fill.r + 0.5),
					(int) (fill.g + 0.5), (int) (fill.b + 0.5));
		nscss_set(style, BGCOLOR_PROP, buf);
	}
	(void) doc;
}

/* replaced elements: sized by the style sheet even inline */
static const char *const nscss_replaced_tags[] = {
	"img", "object", "embed", "video", "canvas", "iframe", NULL
};

static void
nscss_export(NSCSS_Doc *doc, NSCSS_Node *node, const css_computed_style *st,
			 const css_computed_style *ua_root, StyleStruct *style)
{
	static const char *const img_tags[] = { "img", NULL };
	/* Layout paints background images on these, and on blocks (their
	 * box is a table cell); an inline element's would wrap it in a table. */
	static const char *const bg_image_tags[] = {
		"body", "table", "td", "th", NULL
	};
	char buf[200], ubuf[200], *v, *u;
	css_fixed len;
	css_unit unit;
	css_color c;
	lwc_string *url = NULL;
	uint8_t t;
	size_t i;
	XP_Bool block;

	if (!st)
		return;
	block = nscss_is_block(node, css_computed_display_static(st));

	/* display */
	t = css_computed_display_static(st);
	if (t == CSS_DISPLAY_NONE || nscss_visually_hidden(st) ||
		css_computed_visibility(st) == CSS_VISIBILITY_HIDDEN ||
		nscss_invisible_control(node, st)) {
		nscss_set(style, DISPLAY_PROP, "none");
		return;					/* layout applies nothing else */
	}
	/* A flex container's children are its items: blocks (CSS
	 * "blockifies" them), in a row in layout's flex table, or one below
	 * the other (a column). */
	if (node->parent && node->parent->flex != NSCSS_FLEX_NONE &&
		css_computed_position(st) != CSS_POSITION_ABSOLUTE &&
		css_computed_position(st) != CSS_POSITION_FIXED) {
		nscss_export_flex_item(doc, st, style);
		if (node->parent->flex == NSCSS_FLEX_GRID)
			nscss_export_grid_item(st, style);
		nscss_set(style, DISPLAY_PROP, "block");
		block = TRUE;
		t = CSS_DISPLAY_BLOCK;
	}
	if (t == CSS_DISPLAY_FLEX) {
		uint8_t fd = css_computed_flex_direction(st);

		node->flex = (fd == CSS_FLEX_DIRECTION_COLUMN ||
					  fd == CSS_FLEX_DIRECTION_COLUMN_REVERSE) ?
			NSCSS_FLEX_COLUMN : NSCSS_FLEX_ROW;
		nscss_export_flex_container(doc, st, style);
	}
	/* a grid (an inline one too: laid out as a block) */
	if (t == CSS_DISPLAY_GRID || t == CSS_DISPLAY_INLINE_GRID) {
		node->flex = NSCSS_FLEX_GRID;
		nscss_export_grid_container(doc, st, style);
		block = TRUE;
		t = CSS_DISPLAY_GRID;
	}
	/* Layout has no inline flex.  An inline flex container's items flow
	 * side by side as inline content does:
	 * closer than a column of blocks (a navigation bar laid out as a
	 * list). */
	if (t == CSS_DISPLAY_INLINE_FLEX) {
		uint8_t fd = css_computed_flex_direction(st);
		node->flex_row = fd == CSS_FLEX_DIRECTION_ROW ||
						 fd == CSS_FLEX_DIRECTION_ROW_REVERSE ||
						 fd == CSS_FLEX_DIRECTION_INHERIT;
	} else if (/* an inline block's own blocks (a details' summary, a
			    * badge's divs) stay on its line */
			   t == CSS_DISPLAY_INLINE_BLOCK) {
		node->flex_row = TRUE;
	}
	if ((node->parent && node->parent->flex_row &&
		 node->parent->flex == NSCSS_FLEX_NONE &&
		 !nscss_layout_block(lwc_string_data(node->name))) ||
		/* floated list items: a menu bar, laid out as a flex row would
		 * be (layout floats only images and tables) */
		(!strcasecomp(lwc_string_data(node->name), "li") &&
		 (css_computed_float(st) == CSS_FLOAT_LEFT ||
		  css_computed_float(st) == CSS_FLOAT_RIGHT))) {
		nscss_set(style, DISPLAY_PROP, "inline");
		block = FALSE;
		t = CSS_DISPLAY_INLINE;
	} else
	switch (t) {
	case CSS_DISPLAY_BLOCK:
	case CSS_DISPLAY_FLEX:
	case CSS_DISPLAY_GRID:
	case CSS_DISPLAY_TABLE:
		nscss_set(style, DISPLAY_PROP, "block");
		break;
	case CSS_DISPLAY_LIST_ITEM:
		nscss_set(style, DISPLAY_PROP, "list-item");
		break;
	case CSS_DISPLAY_INLINE:
	case CSS_DISPLAY_INLINE_BLOCK:
	case CSS_DISPLAY_INLINE_FLEX:
	case CSS_DISPLAY_INLINE_GRID:
	case CSS_DISPLAY_INLINE_TABLE:
		nscss_set(style, DISPLAY_PROP, "inline");
		break;
	default:					/* the marker, or table parts */
		break;
	}

	/* CSS tables: layout makes them of its own table parts */
	if (!nscss_name_in(node, nscss_table_tags_x)) {
		switch (css_computed_display_static(st)) {
		case CSS_DISPLAY_TABLE:
		case CSS_DISPLAY_INLINE_TABLE:
			nscss_set(style, TABLEPART_PROP, "table");
			nscss_table_spacing(doc, st, style);
			break;
		case CSS_DISPLAY_TABLE_ROW:
			nscss_set(style, TABLEPART_PROP, "row");
			break;
		case CSS_DISPLAY_TABLE_CELL:
			nscss_set(style, TABLEPART_PROP, "cell");
			break;
		default:
			break;
		}
	} else if (!strcasecomp(lwc_string_data(node->name), "table")) {
		/* an HTML table's border-spacing (its cellspacing attribute, a
		 * presentational hint, wins: nscss has no hints) */
		if (!nscss_attr_named(node, "cellspacing", 11))
			nscss_table_spacing(doc, st, style);
		else if (css_computed_table_layout(st) == CSS_TABLE_LAYOUT_FIXED)
			nscss_set(style, TABLELAYOUT_PROP, "fixed");
	}

	/* Inherited properties.  Under a parent, libcss leaves the ones no
	 * rule set as inherit; at the bottom of the stack, compare with the
	 * user agent sheet alone. */
	for (i = 0; i < sizeof nscss_inherited / sizeof nscss_inherited[0]; i++) {
		if (!block && (nscss_inherited[i].fmt == fmt_text_align ||
					   nscss_inherited[i].fmt == fmt_text_indent))
			continue;
		v = nscss_inherited[i].fmt(doc, st, buf);
		if (v && node->parent == NULL && ua_root) {
			u = nscss_inherited[i].fmt(doc, ua_root, ubuf);
			if (u && !XP_STRCMP(u, v))
				continue;
		}
		nscss_set(style, nscss_inherited[i].name, v);
	}
	/* Layout draws a list item's bullet whatever its display: an inline
	 * item has none (after the inherited list-style-type, which would
	 * give it one back). */
	if (!block && !strcasecomp(lwc_string_data(node->name), "li"))
		nscss_set(style, LISTSTYLETYPE_PROP, "none");

	/* Links take their colour from linkColor and visitedColor. */
	if (node->parent && (v = fmt_color(doc, st, buf)) != NULL) {
		bool link = false;
		h_node_is_link(doc, node, &link);
		if (link) {
			nscss_set(style, LINKCOLOR_PROP, v);
			nscss_set(style, VISITEDCOLOR_PROP, v);
		}
	}

	t = css_computed_text_decoration(st);
	if (t != CSS_TEXT_DECORATION_BLINK) {
		if (t == CSS_TEXT_DECORATION_NONE || t == CSS_TEXT_DECORATION_INHERIT) {
			nscss_set(style, TEXTDECORATION_PROP, "none");
		} else {
			PR_snprintf(buf, sizeof buf, "%s%s%s",
						t & CSS_TEXT_DECORATION_UNDERLINE ? "underline " : "",
						t & CSS_TEXT_DECORATION_LINE_THROUGH ? "line-through " : "",
						t & CSS_TEXT_DECORATION_BLINK ? "blink" : "");
			nscss_set(style, TEXTDECORATION_PROP, buf[0] ? buf : "none");
		}
	}

	/* text-shadow (the first; its blur is not drawn, its alpha blended
	 * with white) */
	{
		lwc_string *ts = NULL;

		css_computed_text_shadow(st, &ts);
		if (ts) {
			const char *p = lwc_string_data(ts);
			double num[3], pct, font = 16;
			int nn = 0;
			nscss_rgba c;
			XP_Bool have_color = FALSE;
			css_fixed fs;
			css_unit fu;
			css_color col;

			if (css_computed_font_size(st, &fs, &fu) ==
				CSS_FONT_SIZE_DIMENSION && fu == CSS_UNIT_PX)
				font = FIXTOFLT(fs);
			while (*p && *p != ',') {
				int depth = 0;

				while (*p == ' ')
					p++;
				if (!*p || *p == ',')
					break;
				if (isdigit((unsigned char) *p) || *p == '-' || *p == '.') {
					if (nn < 3)
						num[nn++] = nscss_text_len(doc, p, font, &pct);
				} else if (nscss_parse_color(p, &c))
					have_color = TRUE;
				while (*p && (depth > 0 || (*p != ' ' && *p != ','))) {
					if (*p == '(')
						depth++;
					else if (*p == ')')
						depth--;
					p++;
				}
			}
			if (nn >= 2) {
				if (!have_color && css_computed_color(st, &col) ==
					CSS_COLOR_COLOR) {
					c.r = (col >> 16) & 255; c.g = (col >> 8) & 255;
					c.b = col & 255; c.a = 1;
				} else if (!have_color) {
					c.r = c.g = c.b = 0;
					c.a = 1;
				}
				PR_snprintf(buf, sizeof buf, "%d %d #%02x%02x%02x",
							(int) num[0], (int) num[1],
							(int) (c.r * c.a + 255 * (1 - c.a) + 0.5),
							(int) (c.g * c.a + 255 * (1 - c.a) + 0.5),
							(int) (c.b * c.a + 255 * (1 - c.a) + 0.5));
				nscss_set(style, TEXTSHADOW_PROP, buf);
			}
		}
	}

	/* Positioned elements are layers (layout lays their content out apart
	 * and puts it where the offsets say); their box goes with them. */
	nscss_export_position(doc, st, style);
	if (block) {
		nscss_export_box(doc, node, st, style);
		nscss_export_decorations(doc, st, style);
	}

	/* Images and the like are sized by the style sheet as they are,
	 * inline: width, height, max-width (layimage.c). */
	if (nscss_name_in(node, nscss_replaced_tags)) {
		css_fixed rlen;
		css_unit runit;
		uint8_t rt;

		/* (names of their own: layout's box code would take width as a
		 * block's, set margins for it and start the line over) */
		rt = css_computed_width(st, &rlen, &runit);
		nscss_box_len(doc, style, IMGWIDTH_PROP, rt, CSS_WIDTH_SET, rlen, runit,
					  NSCSS_FIX_MARK_POS);
		rt = css_computed_height(st, &rlen, &runit);
		nscss_box_len(doc, style, IMGHEIGHT_PROP, rt, CSS_HEIGHT_SET, rlen, runit,
					  NSCSS_FIX_MARK_POS);
		rt = css_computed_max_width(st, &rlen, &runit);
		nscss_box_len(doc, style, MAXWIDTH_PROP, rt, CSS_MAX_WIDTH_SET, rlen,
					  runit, NSCSS_FIX_MARK_POS);
	}

	switch (css_computed_vertical_align(st, &len, &unit)) {
	case CSS_VERTICAL_ALIGN_BASELINE: nscss_set(style, VALIGN_PROP, "baseline"); break;
	case CSS_VERTICAL_ALIGN_TOP: nscss_set(style, VALIGN_PROP, "top"); break;
	case CSS_VERTICAL_ALIGN_TEXT_TOP: nscss_set(style, VALIGN_PROP, "text-top"); break;
	case CSS_VERTICAL_ALIGN_MIDDLE: nscss_set(style, VALIGN_PROP, "middle"); break;
	case CSS_VERTICAL_ALIGN_BOTTOM: nscss_set(style, VALIGN_PROP, "bottom"); break;
	case CSS_VERTICAL_ALIGN_TEXT_BOTTOM: nscss_set(style, VALIGN_PROP, "text-bottom"); break;
	/* sub, super and lengths raise or lower an inline element (not HTML's
	 * sub and sup: their tags do it themselves) */
	case CSS_VERTICAL_ALIGN_SUB:
	case CSS_VERTICAL_ALIGN_SUPER:
		if (strcasecomp(lwc_string_data(node->name), "sub") &&
			strcasecomp(lwc_string_data(node->name), "sup"))
			nscss_set(style, VALIGN_PROP,
					  css_computed_vertical_align(st, &len, &unit) ==
					  CSS_VERTICAL_ALIGN_SUB ? "sub" : "super");
		break;
	case CSS_VERTICAL_ALIGN_SET:
		if (!nscss_marked(len, unit, NSCSS_FIX_MARK_LEN) &&
			!nscss_marked(len, unit, NSCSS_FIX_MARK_POS) && len != 0) {
			if (unit == CSS_UNIT_PCT)
				PR_snprintf(buf, sizeof buf, "%g%%", FIXTOFLT(len));
			if (unit == CSS_UNIT_PCT || nscss_len(doc, buf, len, unit))
				nscss_set(style, VALIGN_PROP, buf);
		}
		break;
	default: break;				/* the marker */
	}

	if (css_computed_background_color(st, &c) == CSS_BACKGROUND_COLOR_COLOR &&
		c != NSCSS_COLOR_MARK)
		nscss_set(style, BGCOLOR_PROP, nscss_color(buf, c));

	/* (a block's box is a table cell too) */
	if (nscss_name_in(node, bg_image_tags) || block) {
		css_computed_background_image(st, &url);
		if (url == NULL) {
			nscss_set(style, BGIMAGE_PROP, "none");
		} else if (!strncmp(lwc_string_data(url), "nscss-gradient:", 15)) {
			nscss_export_gradient(doc, st, style, lwc_string_data(url) + 15);
		} else if (strncmp(lwc_string_data(url), "about:nscss-unset", 17)) {
			PR_snprintf(buf, sizeof buf, "url(%.*s)",
						(int) lwc_string_length(url), lwc_string_data(url));
			if (lwc_string_length(url) < sizeof buf - 6)
				nscss_set(style, BGIMAGE_PROP, buf);
			switch (css_computed_background_repeat(st)) {
			case CSS_BACKGROUND_REPEAT_REPEAT_X: v = "repeat-x"; break;
			case CSS_BACKGROUND_REPEAT_REPEAT_Y: v = "repeat-y"; break;
			case CSS_BACKGROUND_REPEAT_NO_REPEAT: v = "no-repeat"; break;
			default: v = "repeat"; break;
			}
			nscss_set(style, BGREPEAT_PROP, v);
		}
	}

	/* Images float; layout floats other elements only as blocks.  An
	 * absolutely positioned element does not float (CSS 2.1 9.7). */
	if ((block || nscss_name_in(node, img_tags)) &&
		css_computed_position(st) != CSS_POSITION_ABSOLUTE &&
		css_computed_position(st) != CSS_POSITION_FIXED) {
		switch (css_computed_float(st)) {
		case CSS_FLOAT_LEFT: nscss_set(style, FLOAT_PROP, "left"); break;
		case CSS_FLOAT_RIGHT: nscss_set(style, FLOAT_PROP, "right"); break;
		default: break;
		}
	}
}

/* Margins, padding, sizes, borders, clear: for blocks only. */
static void
nscss_export_box(NSCSS_Doc *doc, NSCSS_Node *node, const css_computed_style *st,
				 StyleStruct *style)
{
	char buf[200];
	css_fixed len;
	css_unit unit;
	css_color c;
	uint8_t t;

	nscss_set(style, BLOCKBOX_PROP, "1");

	/* Margins ("auto" would read as 0px: leave it to layout). */
	t = css_computed_margin_top(st, &len, &unit);
	nscss_box_len(doc, style, TOPMARGIN_PROP, t, CSS_MARGIN_SET, len, unit,
				  NSCSS_FIX_MARK_LEN);
	t = css_computed_margin_right(st, &len, &unit);
	nscss_box_len(doc, style, RIGHTMARGIN_PROP, t, CSS_MARGIN_SET, len, unit,
				  NSCSS_FIX_MARK_LEN);
	t = css_computed_margin_bottom(st, &len, &unit);
	nscss_box_len(doc, style, BOTTOMMARGIN_PROP, t, CSS_MARGIN_SET, len, unit,
				  NSCSS_FIX_MARK_LEN);
	t = css_computed_margin_left(st, &len, &unit);
	nscss_box_len(doc, style, LEFTMARGIN_PROP, t, CSS_MARGIN_SET, len, unit,
				  NSCSS_FIX_MARK_LEN);

	t = css_computed_padding_top(st, &len, &unit);
	nscss_box_len(doc, style, TOPPADDING_PROP, t, CSS_PADDING_SET, len, unit,
				  NSCSS_FIX_MARK_POS);
	t = css_computed_padding_right(st, &len, &unit);
	nscss_box_len(doc, style, RIGHTPADDING_PROP, t, CSS_PADDING_SET, len, unit,
				  NSCSS_FIX_MARK_POS);
	t = css_computed_padding_bottom(st, &len, &unit);
	nscss_box_len(doc, style, BOTTOMPADDING_PROP, t, CSS_PADDING_SET, len, unit,
				  NSCSS_FIX_MARK_POS);
	t = css_computed_padding_left(st, &len, &unit);
	nscss_box_len(doc, style, LEFTPADDING_PROP, t, CSS_PADDING_SET, len, unit,
				  NSCSS_FIX_MARK_POS);

	t = css_computed_width(st, &len, &unit);
	nscss_box_len(doc, style, WIDTH_PROP, t, CSS_WIDTH_SET, len, unit,
				  NSCSS_FIX_MARK_POS);
	t = css_computed_height(st, &len, &unit);
	nscss_box_len(doc, style, HEIGHT_PROP, t, CSS_HEIGHT_SET, len, unit,
				  NSCSS_FIX_MARK_POS);
	/* min- and max- sizes: layout clamps the box's size (none of them is
	 * the initial value) */
	len = 0;
	t = css_computed_min_width(st, &len, &unit);
	if (len > 0)
		nscss_box_len(doc, style, MINWIDTH_PROP, t, CSS_MIN_WIDTH_SET, len,
					  unit, NSCSS_FIX_MARK_POS);
	t = css_computed_max_width(st, &len, &unit);
	nscss_box_len(doc, style, MAXWIDTH_BOX_PROP, t, CSS_MAX_WIDTH_SET, len,
				  unit, NSCSS_FIX_MARK_POS);
	len = 0;
	t = css_computed_min_height(st, &len, &unit);
	if (len > 0)
		nscss_box_len(doc, style, MINHEIGHT_PROP, t, CSS_MIN_HEIGHT_SET, len,
					  unit, NSCSS_FIX_MARK_POS);
	t = css_computed_max_height(st, &len, &unit);
	nscss_box_len(doc, style, MAXHEIGHT_PROP, t, CSS_MAX_HEIGHT_SET, len,
				  unit, NSCSS_FIX_MARK_POS);

	/* Borders: layout draws a border by wrapping the element in a table,
	 * each side its own width (none: 0), one style and one colour (the
	 * first side's that has a border). */
	{
		css_fixed w[4];
		css_unit wu[4];
		uint8_t ws[4], bs[4], ct[4];
		css_color cc[4];
		int k, first = -1;
		static const char *const bstyle[] = {
			NULL, "none", "none", "dotted", "dashed", "solid", "double",
			"groove", "ridge", "inset", "outset"
		};
		static char *const names[4] = {
			BORDERTOPWIDTH_PROP, BORDERRIGHTWIDTH_PROP,
			BORDERBOTTOMWIDTH_PROP, BORDERLEFTWIDTH_PROP
		};

		ws[0] = css_computed_border_top_width(st, &w[0], &wu[0]);
		ws[1] = css_computed_border_right_width(st, &w[1], &wu[1]);
		ws[2] = css_computed_border_bottom_width(st, &w[2], &wu[2]);
		ws[3] = css_computed_border_left_width(st, &w[3], &wu[3]);
		bs[0] = css_computed_border_top_style(st);
		bs[1] = css_computed_border_right_style(st);
		bs[2] = css_computed_border_bottom_style(st);
		bs[3] = css_computed_border_left_style(st);
		ct[0] = css_computed_border_top_color(st, &cc[0]);
		ct[1] = css_computed_border_right_color(st, &cc[1]);
		ct[2] = css_computed_border_bottom_color(st, &cc[2]);
		ct[3] = css_computed_border_left_color(st, &cc[3]);
		for (k = 0; k < 4; k++) {
			if (ws[k] != CSS_BORDER_WIDTH_WIDTH ||
				nscss_marked(w[k], wu[k], NSCSS_FIX_MARK_POS) || w[k] <= 0 ||
				bs[k] == CSS_BORDER_STYLE_NONE || bs[k] == CSS_BORDER_STYLE_HIDDEN)
				w[k] = 0;
			else if (first < 0)
				first = k;
		}
		if (first >= 0) {
			for (k = 0; k < 4; k++)
				nscss_set(style, names[k], w[k] > 0
						  ? nscss_len(doc, buf, w[k], wu[k]) : "0px");
			if (bs[first] < sizeof bstyle / sizeof bstyle[0])
				nscss_set(style, BORDERSTYLE_PROP, bstyle[bs[first]]);
			if (ct[first] == CSS_BORDER_COLOR_COLOR &&
				cc[first] != NSCSS_COLOR_MARK)
				nscss_set(style, BORDERCOLOR_PROP, nscss_color(buf, cc[first]));
			else if (ct[first] == CSS_BORDER_COLOR_CURRENT_COLOR &&
					 css_computed_color(st, &cc[first]) == CSS_COLOR_COLOR)
				/* currentColor (the initial value): the text's */
				nscss_set(style, BORDERCOLOR_PROP, nscss_color(buf, cc[first]));
		}
	}

	switch (css_computed_clear(st)) {
	case CSS_CLEAR_LEFT: nscss_set(style, CLEAR_PROP, "left"); break;
	case CSS_CLEAR_RIGHT: nscss_set(style, CLEAR_PROP, "right"); break;
	case CSS_CLEAR_BOTH: nscss_set(style, CLEAR_PROP, "both"); break;
	default: break;
	}
}

static const char *const nscss_table_tags[] = {
	"table", "caption", "thead", "tbody", "tfoot", "tr", "td", "th", NULL
};

/* A block that encloses its floats: overflow other than visible (a new
 * block formatting context), or a clearfix (an ::after with content that
 * clears).  Layout ends it below the floats that started inside it. */
static void
nscss_encloses_floats(NSCSS_Node *node, const css_computed_style *st,
					  const css_computed_style *after, StyleStruct *style)
{
	uint8_t d;

	if (!st)
		return;
	d = css_computed_display_static(st);
	if (d == CSS_DISPLAY_NONE || !nscss_is_block(node, d))
		return;
	/* table parts: their cells hold their floats already */
	if (d == CSS_DISPLAY_TABLE || d == CSS_DISPLAY_TABLE_CELL ||
		d == CSS_DISPLAY_TABLE_ROW || nscss_name_in(node, nscss_table_tags))
		return;
	if (after) {
		const css_computed_content_item *items = NULL;

		if (css_computed_content(after, &items) == CSS_CONTENT_SET &&
			css_computed_clear(after) != CSS_CLEAR_NONE &&
			css_computed_clear(after) != CSS_CLEAR_INHERIT) {
			nscss_set(style, NS_CLEAR_AFTER_PROP, "both");
			return;
		}
	}
	switch (css_computed_overflow_y(st)) {
	case CSS_OVERFLOW_HIDDEN:
	case CSS_OVERFLOW_SCROLL:
	case CSS_OVERFLOW_AUTO:
		nscss_set(style, NS_CLEAR_AFTER_PROP, "both");
		break;
	default:
		break;
	}
}

/* An attribute of NODE by its name, or NULL */
static const char *
nscss_attr_named(NSCSS_Node *node, const char *name, size_t len)
{
	int32 i;

	for (i = 0; i < node->n_attrs; i++)
		if (strlen(node->attrs[i].name) == len &&
			!strncasecomp(node->attrs[i].name, name, len))
			return node->attrs[i].value ? node->attrs[i].value : "";
	return NULL;
}

/* The ::before or ::after (PS) of NODE: its text from 'content' (strings,
 * attr(), quotes; no counters yet: layout replays tags in tables, which
 * would count them twice) and the styles layout gives it, as PREFIX,
 * PREFIX+"Color", "Bg", "Font" and "Block". */
static void
nscss_generated(NSCSS_Node *node, const css_computed_style *ps,
				StyleStruct *style, char *prefix)
{
	const css_computed_content_item *it = NULL;
	char text[1024], name[48], buf[48];
	size_t n = 0, k;
	css_color c;
	uint8_t d;
	int depth = 0;

	if (!ps || css_computed_content(ps, &it) != CSS_CONTENT_SET || !it)
		return;
	d = css_computed_display_static(ps);
	if (d == CSS_DISPLAY_NONE)
		return;
	for (; it->type != CSS_COMPUTED_CONTENT_NONE; it++) {
		const char *add = NULL;
		size_t len = 0;

		switch (it->type) {
		case CSS_COMPUTED_CONTENT_STRING:
			add = lwc_string_data(it->data.string);
			len = lwc_string_length(it->data.string);
			break;
		case CSS_COMPUTED_CONTENT_ATTR:
			add = nscss_attr_named(node, lwc_string_data(it->data.attr),
								   lwc_string_length(it->data.attr));
			len = add ? strlen(add) : 0;
			break;
		case CSS_COMPUTED_CONTENT_OPEN_QUOTE:
		case CSS_COMPUTED_CONTENT_CLOSE_QUOTE: {
			/* the quotes property's pair (the first), or straight ones */
			lwc_string **q = NULL;
			int open = it->type == CSS_COMPUTED_CONTENT_OPEN_QUOTE;

			css_computed_quotes(ps, &q);
			if (q && q[0] && q[1]) {
				add = lwc_string_data(q[open ? 0 : 1]);
				len = lwc_string_length(q[open ? 0 : 1]);
			} else {
				add = "\"";
				len = 1;
			}
			depth += open ? 1 : -1;
			break;
		}
		default:
			break;
		}
		for (k = 0; add && k < len && n < sizeof text - 1; k++)
			text[n++] = add[k];
	}
	text[n] = '\0';
	if (n == 0)
		return;
	nscss_set(style, prefix, text);
	if (css_computed_color(ps, &c) == CSS_COLOR_COLOR && nscss_color(buf, c)) {
		PR_snprintf(name, sizeof name, "%sColor", prefix);
		nscss_set(style, name, buf);
	}
	if (css_computed_background_color(ps, &c) == CSS_BACKGROUND_COLOR_COLOR &&
		c != NSCSS_COLOR_MARK && nscss_color(buf, c)) {
		PR_snprintf(name, sizeof name, "%sBg", prefix);
		nscss_set(style, name, buf);
	}
	buf[0] = '\0';
	switch (css_computed_font_weight(ps)) {
	case CSS_FONT_WEIGHT_BOLD: case CSS_FONT_WEIGHT_BOLDER:
	case CSS_FONT_WEIGHT_600: case CSS_FONT_WEIGHT_700:
	case CSS_FONT_WEIGHT_800: case CSS_FONT_WEIGHT_900:
		XP_STRCAT(buf, "bold ");
		break;
	default:
		break;
	}
	if (css_computed_font_style(ps) == CSS_FONT_STYLE_ITALIC ||
		css_computed_font_style(ps) == CSS_FONT_STYLE_OBLIQUE)
		XP_STRCAT(buf, "italic");
	PR_snprintf(name, sizeof name, "%sFont", prefix);
	nscss_set(style, name, buf[0] ? buf : "normal");
	if (d == CSS_DISPLAY_BLOCK || d == CSS_DISPLAY_LIST_ITEM ||
		d == CSS_DISPLAY_TABLE || d == CSS_DISPLAY_FLEX ||
		d == CSS_DISPLAY_GRID) {
		PR_snprintf(name, sizeof name, "%sBlock", prefix);
		nscss_set(style, name, "1");
	}
	(void)depth;
}

/* An element's ::first-letter (PS, if a rule styles it) */
static void
nscss_first_letter(NSCSS_Doc *doc, const css_computed_style *ps,
				   StyleStruct *style)
{
	char color[40] = "", bg[40] = "", font[40] = "", size[40] = "";
	char out[200];
	css_color c;
	css_fixed len;
	css_unit unit;

	if (!ps)
		return;
	if (css_computed_color(ps, &c) == CSS_COLOR_COLOR && !nscss_color(color, c))
		color[0] = '\0';
	if (css_computed_background_color(ps, &c) == CSS_BACKGROUND_COLOR_COLOR &&
		c != NSCSS_COLOR_MARK && !nscss_color(bg, c))
		bg[0] = '\0';
	switch (css_computed_font_weight(ps)) {
	case CSS_FONT_WEIGHT_BOLD: case CSS_FONT_WEIGHT_BOLDER:
	case CSS_FONT_WEIGHT_600: case CSS_FONT_WEIGHT_700:
	case CSS_FONT_WEIGHT_800: case CSS_FONT_WEIGHT_900:
		XP_STRCAT(font, "bold ");
		break;
	default:
		XP_STRCAT(font, "normal ");
		break;
	}
	if (css_computed_font_style(ps) == CSS_FONT_STYLE_ITALIC ||
		css_computed_font_style(ps) == CSS_FONT_STYLE_OBLIQUE)
		XP_STRCAT(font, "italic");
	if (css_computed_font_size(ps, &len, &unit) == CSS_FONT_SIZE_DIMENSION &&
		!nscss_len(doc, size, len, unit))
		size[0] = '\0';
	PR_snprintf(out, sizeof out, "%s|%s|%s|%s", color, bg, font, size);
	nscss_set(style, FIRST_LETTER_PROP, out);
}

void
NSCSS_StyleNode(NSCSS_Doc *doc, NSCSS_Node *node, StyleStruct *style)
{
	css_select_results *res = NULL, *ua = NULL;

	if (!doc || !node || !style)
		return;
	if (css_select_style(doc->ctx, node, &doc->unit, &doc->media,
						 node->inline_style, &nscss_handler, doc, &res) != CSS_OK)
		return;
	if (node->parent == NULL) {
		/* The same tag, with its own libcss node data: libcss caches
		 * styles there per selection context. */
		NSCSS_Node probe = *node;

		probe.node_data = NULL;
		probe.prev = NULL;
		if (css_select_style(doc->ua_ctx, &probe, &doc->unit, &doc->media,
							 NULL, &nscss_handler, doc, &ua) != CSS_OK)
			ua = NULL;
		if (probe.node_data)
			css_libcss_node_data_handler(&nscss_handler, CSS_NODE_DELETED,
										 doc, &probe, NULL, probe.node_data);
	}
	nscss_log_node(node);
	nscss_export(doc, node, res->styles[CSS_PSEUDO_ELEMENT_NONE],
				 ua ? ua->styles[CSS_PSEUDO_ELEMENT_NONE] : NULL, style);
	nscss_encloses_floats(node, res->styles[CSS_PSEUDO_ELEMENT_NONE],
						  res->styles[CSS_PSEUDO_ELEMENT_AFTER], style);
	/* (not for replaced elements, nor hidden ones) */
	if (res->styles[CSS_PSEUDO_ELEMENT_NONE] &&
		css_computed_display_static(res->styles[CSS_PSEUDO_ELEMENT_NONE]) !=
		CSS_DISPLAY_NONE && !nscss_name_in(node, nscss_replaced_tags)) {
		nscss_generated(node, res->styles[CSS_PSEUDO_ELEMENT_BEFORE], style,
						GEN_BEFORE_PROP);
		nscss_generated(node, res->styles[CSS_PSEUDO_ELEMENT_AFTER], style,
						GEN_AFTER_PROP);
		nscss_first_letter(doc, res->styles[CSS_PSEUDO_ELEMENT_FIRST_LETTER],
						   style);
	}
	if (ua)
		css_select_results_destroy(ua);
	css_select_results_destroy(res);
}

