/* -*- Mode: C; tab-width: 4 -*-
 *   nscss.h --- CSS for layout, from libcss.
 *
 * Layout reads style properties for each open tag from a StyleStruct
 * (laystyle.h names them: fontSize, marginTop, ...).  With NS_LIBCSS the
 * style stack fills that StyleStruct from libcss (NetSurf's CSS engine)
 * instead of from JavaScript Style Sheets: <style>, <link rel=stylesheet>
 * and style="" are parsed by libcss, and each tag pushed on the style stack
 * is matched against them.  JavaScript is not involved.
 *
 * libcss sees the open tags and, for each, the tags closed before it under
 * the same parent: everything a selector can look at while the page is
 * still arriving, apart from what follows (:last-child and the like do not
 * match).
 */

#ifndef NSCSS_H
#define NSCSS_H

#include "xp.h"
#include "stystruc.h"

XP_BEGIN_PROTOS

typedef struct NSCSS_Doc NSCSS_Doc;
typedef struct NSCSS_Node NSCSS_Node;

/* One per document (per style stack). */
extern NSCSS_Doc *NSCSS_NewDoc(void);
extern void NSCSS_DestroyDoc(NSCSS_Doc *doc);

/* The window, for media queries (CSS pixels). */
extern void NSCSS_SetViewport(NSCSS_Doc *doc, int32 width, int32 height);

/* Add an author style sheet: the text of a <style> element or a linked
 * sheet, in document order.  URL is the sheet's own address (relative
 * url()s resolve against it), CHARSET its encoding or NULL to detect,
 * MEDIA the media attribute or NULL. */
extern void NSCSS_AddSheet(NSCSS_Doc *doc, const char *url,
						   const char *charset, const char *media,
						   const char *data, int32 len);

/* A tag opens under PARENT (NULL at the bottom of the stack).  ATTRS is
 * the tag's attribute text as the parser keeps it (tag->data), STYLE its
 * style attribute or NULL, BASE_URL the document's base for url()s in it. */
extern NSCSS_Node *NSCSS_OpenNode(NSCSS_Doc *doc, NSCSS_Node *parent,
								  const char *name, const char *class_name,
								  const char *id, const char *attrs,
								  int32 attrs_len, const char *style,
								  const char *base_url);

/* The tag closed (its stack entry is gone). */
extern void NSCSS_CloseNode(NSCSS_Doc *doc, NSCSS_Node *node);

/* Match NODE against the document's sheets and store the declared
 * properties in STYLE, under the names layout reads. */
extern void NSCSS_StyleNode(NSCSS_Doc *doc, NSCSS_Node *node,
							StyleStruct *style);

XP_END_PROTOS

#endif /* NSCSS_H */
