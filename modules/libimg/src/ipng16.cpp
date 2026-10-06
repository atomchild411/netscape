/* -*- Mode: C; tab-width: 4 -*- */
/*
 * PNG images by today's libpng (1.6, from pkgsrc), for builds that use the
 * system's image libraries (NS_SYSTEM_IMGLIBS); ipng.cpp and png_png.cpp
 * drive the tree's 1997 libpng otherwise.  The same entry points.
 *
 * Every image comes out as 8-bit RGB, or RGBA when it has transparency
 * (an alpha channel, or a tRNS chunk).  Interlaced images are combined
 * as the passes arrive and shown when the last has.
 * A damaged image ends the decode (libpng's errors return here through
 * setjmp; without that libpng would abort the program).
 */
#ifdef NS_SYSTEM_IMGLIBS

#include "if.h"
#include <png.h>

extern "C" void qjs_Log(const char *fmt, ...);

extern void il_emit_row(il_container *ic, uint8 *buf, uint8 *rgbbuf,
                        int start_column, int len, int row, int row_count,
                        il_draw_mode draw_mode, int ipass);

typedef struct il_png16 {
	png_structp		png;
	png_infop		info;
	il_container	*ic;
	png_uint_32		width, height;
	png_size_t		rowbytes;
	png_bytep		image;		/* interlaced: the whole image so far */
	int				failed;
} il_png16;

static void
il_png16_error(png_structp png, png_const_charp msg)
{
	longjmp(png_jmpbuf(png), 1);
}

static void
il_png16_warning(png_structp png, png_const_charp msg)
{
}

/* (also used by ibuf.cpp; png_png.cpp has it otherwise) */
void
il_create_alpha_mask(il_container *ic, int xoffset, int width, int height)
{
	NI_PixmapHeader *mask_header;

	if (ic->mask)
		return;
	if (!(ic->mask = PR_NEWZAP(IL_Pixmap)))
		return;
	mask_header = &ic->mask->header;
	mask_header->color_space = IL_CreateGreyScaleColorSpace(1, 1);
	if (!mask_header->color_space)
		return;
	mask_header->width = width;
	mask_header->height = height;
	/* mask rows quadlet aligned, as the other decoders make them */
	mask_header->widthBytes = (((width + 7) / 8) + 3) / 4 * 4;
}

static void
il_png16_info(png_structp png, png_infop info)
{
	il_png16 *p = (il_png16 *) png_get_progressive_ptr(png);
	il_container *ic = p->ic;
	NI_PixmapHeader *src_hdr = ic->src_header;
	int depth, ctype, interlace;
	double gamma;

	png_get_IHDR(png, info, &p->width, &p->height, &depth, &ctype, &interlace,
				 NULL, NULL);
	if (depth == 16)
		png_set_strip_16(png);
	if (ctype == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (!(ctype & PNG_COLOR_MASK_COLOR)) {
		if (depth < 8)
			png_set_expand_gray_1_2_4_to_8(png);
		png_set_gray_to_rgb(png);
	}
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (png_get_gAMA(png, info, &gamma))
		png_set_gamma(png, 2.2, gamma);
	if (interlace != PNG_INTERLACE_NONE)
		(void) png_set_interlace_handling(png);
	png_read_update_info(png, info);
	p->rowbytes = png_get_rowbytes(png, info);

	/* the source's size only: il_size sets the image's (as for JPEG) */
	src_hdr->width = p->width;
	src_hdr->height = p->height;
	if (png_get_channels(png, info) == 4) {
		il_create_alpha_mask(ic, 0, p->width, p->height);
		ic->image->header.is_interleaved_alpha = TRUE;
		il_init_image_transparent_pixel(ic);
	}
	if (interlace != PNG_INTERLACE_NONE &&
		(p->image = (png_bytep) PR_Calloc(p->height, p->rowbytes)) == NULL)
		png_error(png, "out of memory");

	/* the image library converts RGB(A) to the display's depth */
	{
		int st = il_size(ic);

		if (getenv("NS_PNG_TRACE"))
			qjs_Log("png: %ux%u il_size %d bits %p sized %d dest %dx%d img %dx%d",
					(unsigned) p->width, (unsigned) p->height, st,
					ic->image->bits, (int) ic->sized, ic->dest_width,
					ic->dest_height, (int) ic->image->header.width,
					(int) ic->image->header.height);
		if (st < 0)
			png_error(png, "image library could not size the image");
	}
	il_setup_color_space_converter(ic);
}

static void
il_png16_row(png_structp png, png_bytep new_row, png_uint_32 row_num, int pass)
{
	il_png16 *p = (il_png16 *) png_get_progressive_ptr(png);
	png_bytep row = new_row;

	if (!new_row || row_num >= p->height)
		return;
	if (p->image) {
		/* interlaced: gathered, and shown whole at the end (the image
		 * library takes each row once) */
		png_progressive_combine_row(png, p->image + row_num * p->rowbytes,
									new_row);
		return;
	}
	il_emit_row(p->ic, 0, row, 0, (int) p->width, (int) row_num, 1,
				ilErase, 0);
}

static void
il_png16_end(png_structp png, png_infop info)
{
	il_png16 *p = (il_png16 *) png_get_progressive_ptr(png);
	png_uint_32 y;

	if (p->image)
		for (y = 0; y < p->height; y++)
			il_emit_row(p->ic, 0, p->image + y * p->rowbytes, 0,
						(int) p->width, (int) y, 1, ilErase, 0);
	il_flush_image_data(p->ic);
}

static void
il_png16_free(il_png16 *p)
{
	if (p->png)
		png_destroy_read_struct(&p->png, p->info ? &p->info : (png_infopp) NULL,
								(png_infopp) NULL);
	p->png = NULL;
	p->info = NULL;
	PR_FREEIF(p->image);
	p->image = NULL;
}

int
il_png_init(il_container *ic)
{
	il_png16 *p = PR_NEWZAP(il_png16);
	NI_ColorSpace *src_color_space = ic->src_header->color_space;

	if (!p)
		return 0;
	p->ic = ic;
	ic->ds = p;
	src_color_space->type = NI_TrueColor;
	src_color_space->pixmap_depth = 24;
	src_color_space->bit_alloc.index_depth = 0;
	return 1;
}

int
il_png_write(il_container *ic, const unsigned char *buf, int32 len)
{
	il_png16 *p = (il_png16 *) ic->ds;

	if (!p || p->failed)
		return -1;
	if (!p->png) {
		p->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
										il_png16_error, il_png16_warning);
		if (!p->png || !(p->info = png_create_info_struct(p->png))) {
			p->failed = 1;
			il_png16_free(p);
			return -1;
		}
		png_set_progressive_read_fn(p->png, p, il_png16_info, il_png16_row,
									il_png16_end);
	}
	if (setjmp(png_jmpbuf(p->png))) {
		/* a damaged image: what was decoded stays */
		p->failed = 1;
		il_png16_free(p);
		return -1;
	}
	png_process_data(p->png, p->info, (png_bytep) buf, (png_size_t) len);
	return 1;
}

void
il_png_complete(il_container *ic)
{
	il_png16 *p = (il_png16 *) ic->ds;

	if (p)
		il_png16_free(p);
	il_frame_complete_notify(ic);
}

void
il_png_abort(il_container *ic)
{
	il_png16 *p = (il_png16 *) ic->ds;

	if (p) {
		il_png16_free(p);
		PR_FREEIF(p);
		ic->ds = NULL;
	}
}

#endif /* NS_SYSTEM_IMGLIBS */
