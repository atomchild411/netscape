/* -*- Mode: C; tab-width: 8 -*- */
/*
 * Page text drawn with FreeType from TrueType fonts that come with the
 * browser, instead of the X server's fonts: antialiased, today's faces,
 * and the glyphs Unicode text needs.
 *
 * The X server here has no RENDER extension, so text is blended on the
 * client: the strip under a run of text is read back (XGetImage), the
 * glyphs' coverage mixed into it with the text colour, and the strip put
 * back (XPutImage, through the caller's GC, so its clipping holds).  Where
 * that cannot be done (not a TrueColor drawable, or the strip is off the
 * drawable) the glyphs are drawn solid, as points.
 *
 * Where the fonts are looked for: fe_ft_dir.  Faces:
 * Arimo, Tinos and Cousine (metric-compatible with Arial, Times New Roman
 * and Courier New) for sans-serif, serif and monospace, and DejaVu Sans
 * for the characters those lack.  NS_FREETYPE=0 in the environment turns
 * this off (the X server's fonts are used).
 */
#ifdef NS_FREETYPE

#include "mozilla.h"
#include "xfe.h"
#include "fonts.h"
#include "libi18n.h"
#include <math.h>
#include <unistd.h>
#include <sys/time.h>

extern void qjs_Log(const char *fmt, ...);

/* NS_FT_STATS: time spent drawing text, logged (NETSCAPE_JS_LOG) */
static int fe_ft_stats = -1;
static long fe_ft_calls, fe_ft_fallbacks;
static double fe_ft_total, fe_ft_xwait;

static double
fe_ft_now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}
#include <ft2build.h>
#include FT_FREETYPE_H
#include <X11/Xutil.h>

#define FT_FACES	14		/* 3 families x 4 styles + the fallback + Ahem */
#define FT_FALLBACK	12
#define FT_AHEM		13		/* the CSS test suites' font, if it is there */

static const char *const fe_ft_files[FT_FACES] = {
	"Arimo-Regular.ttf", "Arimo-Bold.ttf", "Arimo-Italic.ttf", "Arimo-BoldItalic.ttf",
	"Tinos-Regular.ttf", "Tinos-Bold.ttf", "Tinos-Italic.ttf", "Tinos-BoldItalic.ttf",
	"Cousine-Regular.ttf", "Cousine-Bold.ttf", "Cousine-Italic.ttf", "Cousine-BoldItalic.ttf",
	"DejaVuSans.ttf", "Ahem.ttf"
};

typedef struct fe_FTGlyph {
	uint32		code;
	short		left, top;		/* bitmap origin from the pen */
	unsigned short	w, h;
	short		advance;
	unsigned char	*bits;		/* w * h coverage, 0..255 */
	struct fe_FTGlyph *next;
} fe_FTGlyph;

#define FT_GLYPH_HASH 256

typedef struct fe_FTFont {
	struct fe_FTFont *next;
	int		face;			/* index into fe_ft_files */
	int		px;
	int		ascent, descent;
	fe_FTGlyph	*glyphs[FT_GLYPH_HASH];
} fe_FTFont;

static FT_Library fe_ft_lib;
static FT_Face fe_ft_face[FT_FACES];
static int fe_ft_state;			/* 0 untried, 1 on, -1 off */
static fe_FTFont *fe_ft_fonts;
static unsigned char fe_ft_gamma[256];

/* Where the fonts are: NS_FONT_DIR, else, from the program's prefix
 * (.../bin/netscape5), lib/netscape5/fonts (the tardist brings them) or
 * share/fonts/X11/TTF (pkgsrc's croscorefonts and dejavu-ttf), else
 * /usr/local/lib/netscape5/fonts.  The first with Arimo in it. */
static char *
fe_ft_dir(void)
{
	static char dir[1024];
	static const char *const subdirs[] = {
		"/lib/netscape5/fonts", "/share/fonts/X11/TTF", NULL
	};
	char *e = getenv("NS_FONT_DIR");
	char prefix[900], probe[1100];
	int i;

	if (e && *e)
		return e;
	if (fe_progname_long && strrchr(fe_progname_long, '/')) {
		char *slash;

		strncpy(prefix, fe_progname_long, sizeof prefix - 1);
		prefix[sizeof prefix - 1] = '\0';
		slash = strrchr(prefix, '/');
		*slash = '\0';
		if ((slash = strrchr(prefix, '/')) != NULL && !strcmp(slash, "/bin")) {
			*slash = '\0';
			for (i = 0; subdirs[i]; i++) {
				PR_snprintf(probe, sizeof probe, "%s%s/Arimo-Regular.ttf",
							prefix, subdirs[i]);
				if (access(probe, R_OK) == 0) {
					PR_snprintf(dir, sizeof dir, "%s%s", prefix, subdirs[i]);
					return dir;
				}
			}
		}
	}
	return "/usr/local/lib/netscape5/fonts";
}

XP_Bool
fe_FTEnabled(void)
{
	if (fe_ft_state == 0) {
		char *e = getenv("NS_FREETYPE");
		char path[1200];
		int i;

		fe_ft_state = -1;
		if (e && *e == '0')
			return FALSE;
		if (FT_Init_FreeType(&fe_ft_lib) != 0)
			return FALSE;
		for (i = 0; i < FT_FACES; i++) {
			PR_snprintf(path, sizeof path, "%s/%s", fe_ft_dir(), fe_ft_files[i]);
			if (FT_New_Face(fe_ft_lib, path, 0, &fe_ft_face[i]) != 0)
				fe_ft_face[i] = NULL;
		}
		/* the regular faces are needed; the others fall back to them */
		if (!fe_ft_face[0] || !fe_ft_face[4] || !fe_ft_face[8])
			return FALSE;
		/* a little gamma: thin antialiased stems look washed out */
		for (i = 0; i < 256; i++) {
			double c = i / 255.0;

			fe_ft_gamma[i] = (unsigned char) (255.0 * pow(c, 0.8) + 0.5);
		}
		fe_ft_state = 1;
	}
	return fe_ft_state > 0;
}

/* Which of our families a page's font-family name means: 0 sans-serif,
 * 1 serif, 2 monospace. */
static int
fe_ft_family(const char *name, int fontmask)
{
	static const char *const serif[] = {
		"serif", "times", "georgia", "palatino", "garamond", "cambria",
		"book", "tinos", "charter", "baskerville", "minion", "merriweather",
		"utopia", "schoolbook", "roman", NULL
	};
	static const char *const mono[] = {
		"mono", "courier", "consolas", "menlo", "monaco", "console",
		"code", "cousine", "terminal", "fixed", "typewriter", NULL
	};
	char low[128];
	int i;

	if (!name || !*name)
		return (fontmask & LO_FONT_FIXED) ? 2 : 1;
	for (i = 0; name[i] && i < (int) sizeof low - 1; i++)
		low[i] = tolower((unsigned char) name[i]);
	low[i] = '\0';
	for (i = 0; mono[i]; i++)
		if (strstr(low, mono[i]))
			return 2;
	if (strstr(low, "sans"))
		return 0;
	for (i = 0; serif[i]; i++)
		if (strstr(low, serif[i]))
			return 1;
	return 0;
}

/* Does the font-family list name Ahem first (before any family we have)? */
static XP_Bool
fe_ft_names_ahem(const char *name)
{
	while (*name == ' ' || *name == '"' || *name == '\'')
		name++;
	return !strncasecomp(name, "ahem", 4);
}

/* Pixels for HTML's font sizes 1..7 (the usual 16px at 3). */
static const int fe_ft_html_px[8] = { 16, 10, 13, 16, 18, 24, 32, 48 };

fe_Font
fe_FTLoadFont(MWContext *context, const char *family, int points, int sizeNum,
			  int fontmask)
{
	int face, px;
	fe_FTFont *f;
	FT_Face ftf;

	if (!fe_FTEnabled())
		return NULL;
	face = fe_ft_family(family, fontmask) * 4 +
		((fontmask & LO_FONT_BOLD) ? 1 : 0) +
		((fontmask & LO_FONT_ITALIC) ? 2 : 0);
	if (!fe_ft_face[face])
		face &= ~3;
	/* Ahem (the W3C test suites' font): all styles in its one face */
	if (family && fe_ft_face[FT_AHEM] && fe_ft_names_ahem(family))
		face = FT_AHEM;
	if (points > 0) {
		double ppp = context && context->YpixelsPerPoint > 0 ?
			context->YpixelsPerPoint : 1.0;

		px = (int) (points * ppp + 0.5);
	} else
		px = fe_ft_html_px[sizeNum >= 1 && sizeNum <= 7 ? sizeNum : 3];
	if (px < 6)
		px = 6;
	if (px > 200)
		px = 200;

	for (f = fe_ft_fonts; f; f = f->next)
		if (f->face == face && f->px == px)
			return (fe_Font) f;
	f = XP_NEW_ZAP(fe_FTFont);
	if (!f)
		return NULL;
	f->face = face;
	f->px = px;
	ftf = fe_ft_face[face];
	FT_Set_Pixel_Sizes(ftf, 0, px);
	f->ascent = (int) ((ftf->size->metrics.ascender + 63) >> 6);
	f->descent = (int) ((-ftf->size->metrics.descender + 63) >> 6);
	f->next = fe_ft_fonts;
	fe_ft_fonts = f;
	return (fe_Font) f;
}

static fe_FTGlyph *
fe_ft_glyph(fe_FTFont *f, uint32 code)
{
	unsigned h = code & (FT_GLYPH_HASH - 1);
	fe_FTGlyph *g;
	FT_Face ftf = fe_ft_face[f->face];
	FT_GlyphSlot slot;
	int i, n;

	for (g = f->glyphs[h]; g; g = g->next)
		if (g->code == code)
			return g;
	g = XP_NEW_ZAP(fe_FTGlyph);
	if (!g)
		return NULL;
	g->code = code;
	if (FT_Get_Char_Index(ftf, code) == 0 && fe_ft_face[FT_FALLBACK] &&
		FT_Get_Char_Index(fe_ft_face[FT_FALLBACK], code) != 0)
		ftf = fe_ft_face[FT_FALLBACK];
	FT_Set_Pixel_Sizes(ftf, 0, f->px);
	if (FT_Load_Char(ftf, code, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
		slot = ftf->glyph;
		g->left = slot->bitmap_left;
		g->top = slot->bitmap_top;
		g->w = slot->bitmap.width;
		g->h = slot->bitmap.rows;
		g->advance = (short) ((slot->advance.x + 32) >> 6);
		n = g->w * g->h;
		if (n > 0 && slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY &&
			(g->bits = (unsigned char *) XP_ALLOC(n)) != NULL) {
			for (i = 0; i < g->h; i++)
				memcpy(g->bits + i * g->w,
					   slot->bitmap.buffer + i * slot->bitmap.pitch, g->w);
		} else
			g->w = g->h = 0;
	}
	g->next = f->glyphs[h];
	f->glyphs[h] = g;
	return g;
}

/* The next character of TEXT in the document's charset. */
static uint32
fe_ft_next(int16 charset, const unsigned char **p, const unsigned char *end)
{
	const unsigned char *s = *p;
	uint32 c = *s++;

	if ((charset & 0xff) == (CS_UTF8 & 0xff) && c >= 0x80) {
		int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

		c &= more == 3 ? 0x07 : more == 2 ? 0x0f : 0x1f;
		while (more-- > 0 && s < end && (*s & 0xc0) == 0x80)
			c = (c << 6) | (*s++ & 0x3f);
	} else if (c >= 0x80 && c < 0xa0) {
		/* Windows-1252's punctuation, which pages call Latin-1 */
		static const unsigned short cp1252[32] = {
			0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
			0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
			0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
			0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
		};
		if (cp1252[c - 0x80])
			c = cp1252[c - 0x80];
	}
	*p = s;
	return c;
}

void
fe_FTFontExtents(fe_Font font, int *ascent, int *descent)
{
	fe_FTFont *f = (fe_FTFont *) font;

	*ascent = f->ascent;
	*descent = f->descent;
}

void
fe_FTTextExtents(int16 charset, fe_Font font, char *string, int len,
				 int *ascent, int *descent, XCharStruct *overall)
{
	fe_FTFont *f = (fe_FTFont *) font;
	const unsigned char *p = (const unsigned char *) string, *end = p + len;
	int x = 0, lb = 0, rb = 0, first = 1, top = 0, bottom = 0;

	while (p < end) {
		fe_FTGlyph *g = fe_ft_glyph(f, fe_ft_next(charset, &p, end));

		if (!g)
			continue;
		if (g->w) {
			if (first || x + g->left < lb)
				lb = x + g->left;
			if (first || x + g->left + g->w > rb)
				rb = x + g->left + g->w;
			if (g->top > top)
				top = g->top;
			if (g->h - g->top > bottom)
				bottom = g->h - g->top;
			first = 0;
		}
		x += g->advance;
	}
	*ascent = f->ascent;
	*descent = f->descent;
	overall->width = x;
	overall->lbearing = lb;
	overall->rbearing = first ? x : rb;
	overall->ascent = top;
	overall->descent = bottom;
}

/* Pixel <-> 8-bit channels for a TrueColor image. */
static int
fe_ft_shift(unsigned long mask)
{
	int s = 0;

	if (!mask)
		return 0;
	while (!(mask & 1)) {
		mask >>= 1;
		s++;
	}
	return s;
}

static int fe_ft_badmatch;

static int
fe_ft_xerror(Display *dpy, XErrorEvent *e)
{
	fe_ft_badmatch = 1;
	return 0;
}

static void
fe_ft_draw_points(Display *dpy, Drawable d, GC gc, fe_FTFont *f, int16 charset,
				  int x, int y, const unsigned char *p, const unsigned char *end)
{
	XPoint pts[512];
	int n = 0;

	while (p < end) {
		fe_FTGlyph *g = fe_ft_glyph(f, fe_ft_next(charset, &p, end));
		int i, j;

		if (!g)
			continue;
		for (j = 0; j < g->h; j++)
			for (i = 0; i < g->w; i++)
				if (g->bits[j * g->w + i] >= 112) {
					pts[n].x = x + g->left + i;
					pts[n].y = y - g->top + j;
					if (++n == 512) {
						XDrawPoints(dpy, d, gc, pts, n, CoordModeOrigin);
						n = 0;
					}
				}
		x += g->advance;
	}
	if (n)
		XDrawPoints(dpy, d, gc, pts, n, CoordModeOrigin);
}

void
fe_FTDrawString(int16 charset, Display *dpy, Drawable d, fe_Font font, GC gc,
				GC bg_gc, int x, int y, char *string, int len)
{
	fe_FTFont *f = (fe_FTFont *) font;
	const unsigned char *p = (const unsigned char *) string, *end = p + len;
	XCharStruct ov;
	int asc, desc, x0, y0, w, h;
	XImage *im;
	XGCValues v;
	int (*old)(Display *, XErrorEvent *);
	int rs, gs, bs, rb, gb, bb;
	unsigned long fr, fg, fb, rm, gm, bm;

	double t0 = 0, t1 = 0;

	if (len <= 0)
		return;
	if (fe_ft_stats < 0)
		fe_ft_stats = getenv("NS_FT_STATS") != NULL;
	if (fe_ft_stats)
		t0 = fe_ft_now();
	if (fe_ft_stats > 0 && getenv("NS_FT_TRACE"))
		qjs_Log("ft: draw %d,%d px %d '%.*s'", x, y, f->px, len, string);
	fe_FTTextExtents(charset, font, string, len, &asc, &desc, &ov);
	x0 = x + (ov.lbearing < 0 ? ov.lbearing : 0);
	w = (ov.rbearing > ov.width ? ov.rbearing : ov.width) - (x0 - x);
	y0 = y - (ov.ascent > f->ascent ? ov.ascent : f->ascent);
	h = (ov.descent > f->descent ? ov.descent : f->descent) + (y - y0);
	if (w <= 0 || h <= 0)
		return;

	/* the background, for XDrawImageString's sake */
	if (bg_gc) {
		XFillRectangle(dpy, d, bg_gc, x, y - f->ascent, ov.width,
					   f->ascent + f->descent);
	}

	/* read the strip back; off the drawable that is an X error, which
	 * must not reach the program's handler (XGetImage waits for its
	 * reply, so its error has come by the time it returns) */
	fe_ft_badmatch = 0;
	old = XSetErrorHandler(fe_ft_xerror);
	if (fe_ft_stats)
		t1 = fe_ft_now();
	im = XGetImage(dpy, d, x0, y0, (unsigned) w, (unsigned) h, AllPlanes, ZPixmap);
	if (!im) {
		/* partly off the drawable (text scrolled past an edge): read
		 * the part that is on it */
		Window root;
		int gx, gy;
		unsigned gw, gh, gb, gd;

		if (XGetGeometry(dpy, d, &root, &gx, &gy, &gw, &gh, &gb, &gd)) {
			int cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0;
			int cx1 = x0 + w > (int) gw ? (int) gw : x0 + w;
			int cy1 = y0 + h > (int) gh ? (int) gh : y0 + h;

			if (cx1 <= cx0 || cy1 <= cy0) {
				XSetErrorHandler(old);
				goto out;		/* nothing of it shows */
			}
			x0 = cx0;
			y0 = cy0;
			w = cx1 - cx0;
			h = cy1 - cy0;
			fe_ft_badmatch = 0;
			im = XGetImage(dpy, d, x0, y0, (unsigned) w, (unsigned) h,
						   AllPlanes, ZPixmap);
		}
	}
	XSetErrorHandler(old);
	if (fe_ft_stats)
		fe_ft_xwait += fe_ft_now() - t1;
	/* a pixmap has no visual, so its image comes without colour masks:
	 * take them from a TrueColor visual of its depth */
	if (im && !fe_ft_badmatch && !im->red_mask && im->bits_per_pixel >= 16) {
		static int depth;
		static unsigned long masks[3];

		if (depth != im->depth) {
			XVisualInfo vi;

			depth = im->depth;
			masks[0] = masks[1] = masks[2] = 0;
			if (XMatchVisualInfo(dpy, DefaultScreen(dpy), depth, TrueColor, &vi)) {
				masks[0] = vi.red_mask;
				masks[1] = vi.green_mask;
				masks[2] = vi.blue_mask;
			}
		}
		im->red_mask = masks[0];
		im->green_mask = masks[1];
		im->blue_mask = masks[2];
	}
	if (!im || fe_ft_badmatch || !im->red_mask || im->bits_per_pixel < 16) {
		if (im)
			XDestroyImage(im);
		fe_ft_draw_points(dpy, d, gc, f, charset, x, y, p, end);
		fe_ft_fallbacks++;
		goto out;
	}

	XGetGCValues(dpy, gc, GCForeground, &v);
	rm = im->red_mask;
	gm = im->green_mask;
	bm = im->blue_mask;
	rs = fe_ft_shift(rm);
	gs = fe_ft_shift(gm);
	bs = fe_ft_shift(bm);
	rb = (int) (rm >> rs);
	gb = (int) (gm >> gs);
	bb = (int) (bm >> bs);
	fr = (v.foreground & rm) >> rs;
	fg = (v.foreground & gm) >> gs;
	fb = (v.foreground & bm) >> bs;

	{
		int pen = x;

		while (p < end) {
			fe_FTGlyph *g = fe_ft_glyph(f, fe_ft_next(charset, &p, end));
			int i, j;

			if (!g)
				continue;
			for (j = 0; j < g->h; j++) {
				int py = y - g->top + j - y0;

				if (py < 0 || py >= h)
					continue;
				for (i = 0; i < g->w; i++) {
					int px = pen + g->left + i - x0;
					unsigned a = g->bits[j * g->w + i];
					unsigned long pix, r, gr, b;

					if (!a || px < 0 || px >= w)
						continue;
					a = fe_ft_gamma[a];
					pix = XGetPixel(im, px, py);
					r = (pix & rm) >> rs;
					gr = (pix & gm) >> gs;
					b = (pix & bm) >> bs;
					r = (r * (255 - a) + fr * a + 127) / 255;
					gr = (gr * (255 - a) + fg * a + 127) / 255;
					b = (b * (255 - a) + fb * a + 127) / 255;
					if ((int) r > rb) r = rb;
					if ((int) gr > gb) gr = gb;
					if ((int) b > bb) b = bb;
					XPutPixel(im, px, py,
							  (pix & ~(rm | gm | bm)) | (r << rs) | (gr << gs) | (b << bs));
				}
			}
			pen += g->advance;
		}
	}
	XPutImage(dpy, d, gc, im, 0, 0, x0, y0, (unsigned) w, (unsigned) h);
	XDestroyImage(im);
 out:
	if (fe_ft_stats) {
		fe_ft_total += fe_ft_now() - t0;
		if (++fe_ft_calls % 25 == 0)
			qjs_Log("ft: %ld draws, %.2fs (%.2fs waiting for XGetImage), %ld solid",
					fe_ft_calls, fe_ft_total, fe_ft_xwait, fe_ft_fallbacks);
	}
}

#else /* NS_FREETYPE */

#include "mozilla.h"
#include "xfe.h"
#include "fonts.h"

XP_Bool fe_FTEnabled(void) { return FALSE; }
fe_Font fe_FTLoadFont(MWContext *context, const char *family, int points,
					  int sizeNum, int fontmask) { return NULL; }
void fe_FTFontExtents(fe_Font font, int *ascent, int *descent) { }
void fe_FTTextExtents(int16 charset, fe_Font font, char *string, int len,
					  int *ascent, int *descent, XCharStruct *overall) { }
void fe_FTDrawString(int16 charset, Display *dpy, Drawable d, fe_Font font,
					 GC gc, GC bg_gc, int x, int y, char *string, int len) { }

#endif /* NS_FREETYPE */
