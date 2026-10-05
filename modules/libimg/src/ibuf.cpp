/* -*- Mode: C; tab-width: 4 -*-
 *   ibuf.cpp --- image formats decoded whole, once all their bytes are in:
 *   SVG (nanosvg, nanosvg/README), and WebP (libwebp, with NS_WEBP).
 *
 * The bytes are kept as they come; at the end the image is decoded to
 * RGBA in one go and handed to the image library row by row with its
 * alpha, as the PNG decoder does.
 */

#include "if.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* C99's roundf, which nanosvg uses, is not in every libm */
static float
il_roundf(float x)
{
	return x < 0 ? -floorf(-x + 0.5f) : floorf(x + 0.5f);
}
#define roundf il_roundf

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#ifdef NS_WEBP
#include <webp/decode.h>
#endif

extern void il_emit_row(il_container *ic, uint8 *buf, uint8 *rgbbuf,
						int start_column, int len, int row, int row_count,
						il_draw_mode draw_mode, int ipass);
extern void il_create_alpha_mask(il_container *ic, int xoffset,
								 int destwidth, int destheight);

/* SVG images with no size of their own, and the largest drawn */
#define IL_SVG_DEFAULT_W	300
#define IL_SVG_DEFAULT_H	150
#define IL_BUF_MAX_SIDE		2048
#define IL_BUF_MAX_BYTES	(16 * 1024 * 1024)

typedef struct {
	unsigned char *buf;
	int32 len, size;
	XP_Bool failed;
} il_buf_struct;

int
il_buf_init(il_container *ic)
{
	il_buf_struct *b = PR_NEWZAP(il_buf_struct);
	NI_ColorSpace *src_color_space = ic->src_header->color_space;

	if (!b)
		return 0;
	ic->ds = b;
	/* RGB (with interleaved alpha) like PNG */
	src_color_space->type = NI_TrueColor;
	src_color_space->pixmap_depth = 24;
	src_color_space->bit_alloc.index_depth = 0;
	return 1;
}

int
il_buf_write(il_container *ic, const unsigned char *buf, int32 len)
{
	il_buf_struct *b = (il_buf_struct *) ic->ds;

	if (!b || b->failed)
		return 0;
	if (b->len + len + 1 > b->size) {
		int32 size = (b->len + len + 1) * 2 + 4096;
		unsigned char *p;
		if (size > IL_BUF_MAX_BYTES) {
			b->failed = TRUE;
			return 0;
		}
		p = (unsigned char *) PR_Realloc(b->buf, size);
		if (!p) {
			b->failed = TRUE;
			return 0;
		}
		b->buf = p;
		b->size = size;
	}
	memcpy(b->buf + b->len, buf, len);
	b->len += len;
	b->buf[b->len] = '\0';
	return 0;
}

/* Hand RGBA (W x H, straight alpha) to the image library. */
static void
il_buf_emit(il_container *ic, unsigned char *rgba, int w, int h)
{
	NI_PixmapHeader *img_hdr = &ic->image->header;
	NI_PixmapHeader *src_hdr = ic->src_header;
	int y;

	src_hdr->width = img_hdr->width = w;
	src_hdr->height = img_hdr->height = h;
	il_create_alpha_mask(ic, 0, w, h);
	ic->image->header.is_interleaved_alpha = TRUE;
	il_init_image_transparent_pixel(ic);
	if (il_size(ic) < 0)
		return;
	il_setup_color_space_converter(ic);
	for (y = 0; y < h; y++)
		il_emit_row(ic, NULL, rgba + (size_t) y * w * 4, 0, w, y, 1, ilErase, 0);
}

static void
il_buf_svg(il_container *ic, il_buf_struct *b)
{
	NSVGimage *img = nsvgParse((char *) b->buf, "px", 96.0f);
	NSVGrasterizer *r;
	float w, h, scale = 1.0f;
	int iw, ih;
	unsigned char *rgba;

	if (!img)
		return;
	w = img->width > 0 ? img->width : IL_SVG_DEFAULT_W;
	h = img->height > 0 ? img->height : IL_SVG_DEFAULT_H;
	if (w > IL_BUF_MAX_SIDE || h > IL_BUF_MAX_SIDE)
		scale = (float) IL_BUF_MAX_SIDE / (w > h ? w : h);
	iw = (int) (w * scale + 0.5f);
	ih = (int) (h * scale + 0.5f);
	if (iw < 1)
		iw = 1;
	if (ih < 1)
		ih = 1;
	rgba = (unsigned char *) PR_Calloc((size_t) iw * ih, 4);
	r = nsvgCreateRasterizer();
	if (rgba && r) {
		nsvgRasterize(r, img, 0, 0, scale, rgba, iw, ih, iw * 4);
		il_buf_emit(ic, rgba, iw, ih);
	}
	if (r)
		nsvgDeleteRasterizer(r);
	PR_FREEIF(rgba);
	nsvgDelete(img);
}

#ifdef NS_WEBP
static void
il_buf_webp(il_container *ic, il_buf_struct *b)
{
	int w = 0, h = 0;
	uint8_t *rgba = WebPDecodeRGBA(b->buf, b->len, &w, &h);

	if (!rgba)
		return;
	if (w > 0 && h > 0)
		il_buf_emit(ic, rgba, w, h);
	WebPFree(rgba);
}
#endif

void
il_buf_complete(il_container *ic)
{
	il_buf_struct *b = (il_buf_struct *) ic->ds;

	if (b && !b->failed && b->buf) {
		if (ic->type == IL_SVG)
			il_buf_svg(ic, b);
#ifdef NS_WEBP
		else if (ic->type == IL_WEBP)
			il_buf_webp(ic, b);
#endif
	}
	il_buf_abort(ic);			/* the bytes are not needed any more */
	il_frame_complete_notify(ic);
}

void
il_buf_abort(il_container *ic)
{
	il_buf_struct *b = (il_buf_struct *) ic->ds;

	if (b) {
		PR_FREEIF(b->buf);
		PR_DELETE(b);
		ic->ds = NULL;
	}
}
