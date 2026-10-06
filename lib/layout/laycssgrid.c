/* -*- Mode: C; tab-width: 4 -*-
 *   laycssgrid.c --- CSS grid layout, in a flex table.
 *
 * A grid container is a flex table (lo_TableRec.flex LO_FLEX_GRID) whose
 * items are the cells of its one row, as a flex container's are.  Here are
 * the grid's own parts: its track lists, areas and line names (nscss
 * exports them as text, lo_cssgrid_new reads them), an item's lines
 * (lo_cssgrid_item_lines), auto-placement and the sizing of the columns
 * (lo_cssgrid_widths, before the items are laid out at their widths) and of
 * the rows (lo_cssgrid_heights, after).  laytable.c then puts each item's
 * cell in its area as it puts a flex item on its line.
 */

#include "xp.h"
#include "pa_parse.h"
#include "layout.h"
#include "laystyle.h"

#define GRID_MAX_TRACKS	1000	/* repeat() and implicit tracks */
#define GRID_INF		0x3fffffff

/* ---- reading the container's text ---- */

typedef struct {
	MWContext *context;
	lo_DocState *state;
	StyleStruct *style;
	const char *p;
	lo_CSSGridTrack *tracks;
	int32 n, cap;
	lo_CSSGridName *names;
	int32 nnames, names_cap;
} lo_GridParse;

static void
grid_skip(lo_GridParse *g)
{
	while (*g->p == ' ' || *g->p == ',')
		g->p++;
}

/* the next word: up to a space, a bracket, a comma or a parenthesis */
static int32
grid_word(lo_GridParse *g, char *buf, int32 size)
{
	int32 n = 0;

	grid_skip(g);
	while (*g->p && !strchr(" ,()[]", *g->p))
	{
		if (n < size - 1)
			buf[n++] = *g->p;
		g->p++;
	}
	buf[n] = '\0';
	return n;
}

/* a length ("10px", "2em") in pixels */
static double
grid_length(lo_GridParse *g, char *word)
{
	SS_Number *num = STYLESTRUCT_StringToSSNumber(g->style, word);
	double v = 0;

	if (num)
	{
		LO_AdjustSSUnits(num, WIDTH_STYLE, g->context, g->state);
		v = num->value;
		STYLESTRUCT_FreeSSNumber(g->style, num);
	}
	return v;
}

/* one sizing function: a length, a percentage, fr or a keyword */
static void
grid_breadth(lo_GridParse *g, char *word, intn *kind, double *v)
{
	int32 len = XP_STRLEN(word);

	*v = 0;
	if (!strcasecomp(word, "auto"))
		*kind = LO_GT_AUTO;
	else if (!strcasecomp(word, "min-content"))
		*kind = LO_GT_MIN;
	else if (!strcasecomp(word, "max-content"))
		*kind = LO_GT_MAX;
	else if (len > 2 && !strcasecomp(word + len - 2, "fr"))
	{
		*kind = LO_GT_FR;
		*v = atof(word);
	}
	else if (len > 1 && word[len - 1] == '%')
	{
		*kind = LO_GT_PCT;
		*v = atof(word);
	}
	else
	{
		*kind = LO_GT_PX;
		*v = grid_length(g, word);
		if (*v < 0)
			*v = 0;
	}
}

static void
grid_add_name(lo_GridParse *g, char *name, int32 line)
{
	if (g->nnames == g->names_cap)
	{
		lo_CSSGridName *n = (lo_CSSGridName *)XP_REALLOC(g->names,
			(g->names_cap * 2 + 8) * sizeof(lo_CSSGridName));

		if (!n)
			return;
		g->names = n;
		g->names_cap = g->names_cap * 2 + 8;
	}
	g->names[g->nnames].name = XP_STRDUP(name);
	g->names[g->nnames].line = line;
	if (g->names[g->nnames].name)
		g->nnames++;
}

static void
grid_add_track(lo_GridParse *g, lo_CSSGridTrack *t)
{
	if (g->n >= GRID_MAX_TRACKS)
		return;
	if (g->n == g->cap)
	{
		lo_CSSGridTrack *n = (lo_CSSGridTrack *)XP_REALLOC(g->tracks,
			(g->cap * 2 + 8) * sizeof(lo_CSSGridTrack));

		if (!n)
			return;
		g->tracks = n;
		g->cap = g->cap * 2 + 8;
	}
	g->tracks[g->n++] = *t;
}

/* "[a b]": names for the line before the next track */
static void
grid_line_names(lo_GridParse *g)
{
	char word[128];

	g->p++;						/* [ */
	while (*g->p && *g->p != ']')
	{
		if (grid_word(g, word, sizeof word) > 0)
			grid_add_name(g, word, g->n + 1);
		else if (*g->p && *g->p != ']')
			g->p++;
	}
	if (*g->p == ']')
		g->p++;
}

/* one track: a breadth, minmax(min, max) or fit-content(length) */
static Bool
grid_track(lo_GridParse *g, lo_CSSGridTrack *t)
{
	char word[128], a[128], b[128];

	if (grid_word(g, word, sizeof word) == 0)
		return FALSE;
	if (*g->p == '(' && !strcasecomp(word, "minmax"))
	{
		g->p++;
		grid_word(g, a, sizeof a);
		grid_word(g, b, sizeof b);
		while (*g->p && *g->p != ')')
			g->p++;
		if (*g->p)
			g->p++;
		grid_breadth(g, a, &t->min_kind, &t->min_v);
		grid_breadth(g, b, &t->max_kind, &t->max_v);
		/* a flexible minimum is not allowed: auto */
		if (t->min_kind == LO_GT_FR)
			t->min_kind = LO_GT_AUTO;
		return TRUE;
	}
	if (*g->p == '(' && !strcasecomp(word, "fit-content"))
	{
		g->p++;
		grid_word(g, a, sizeof a);
		while (*g->p && *g->p != ')')
			g->p++;
		if (*g->p)
			g->p++;
		t->min_kind = LO_GT_AUTO;
		t->min_v = 0;
		t->max_kind = LO_GT_FIT;
		t->max_v = grid_length(g, a);
		return TRUE;
	}
	if (*g->p == '(')
	{
		/* an unknown function: skip it */
		int depth = 0;

		do
		{
			if (*g->p == '(')
				depth++;
			else if (*g->p == ')')
				depth--;
			g->p++;
		} while (*g->p && depth > 0);
		return FALSE;
	}
	grid_breadth(g, word, &t->max_kind, &t->max_v);
	if (t->max_kind == LO_GT_FR)
	{
		/* Nfr is minmax(auto, Nfr) */
		t->min_kind = LO_GT_AUTO;
		t->min_v = 0;
	}
	else
	{
		t->min_kind = t->max_kind;
		t->min_v = t->max_v;
	}
	return TRUE;
}

/* the fixed size of a track, for repeat(auto-fill): its max if fixed,
 * else its min if fixed, else 0 */
static double
grid_fixed(lo_CSSGridTrack *t, int32 avail)
{
	if (t->max_kind == LO_GT_PX)
		return t->max_v;
	if (t->max_kind == LO_GT_PCT && avail >= 0)
		return t->max_v * avail / 100;
	if (t->min_kind == LO_GT_PX)
		return t->min_v;
	if (t->min_kind == LO_GT_PCT && avail >= 0)
		return t->min_v * avail / 100;
	return 0;
}

static void grid_list(lo_GridParse *g, int32 avail, int32 gap, Bool in_repeat);

/* repeat(count, tracks) */
static void
grid_repeat(lo_GridParse *g, int32 avail, int32 gap)
{
	char word[64];
	int32 count, first = g->n, nnames = g->nnames, i, k, size, per;
	double sum = 0;
	Bool fill = FALSE;

	grid_word(g, word, sizeof word);
	if (!strcasecomp(word, "auto-fill") || !strcasecomp(word, "auto-fit"))
		fill = TRUE, count = 1;
	else
		count = atoi(word);
	if (count < 1)
		count = 1;
	grid_list(g, avail, gap, TRUE);	/* the first repetition */
	if (*g->p == ')')
		g->p++;
	per = g->n - first;
	if (per <= 0)
		return;
	if (fill)
	{
		/* as many as fit (at least one) */
		for (i = first; i < g->n; i++)
			sum += grid_fixed(&g->tracks[i], avail);
		sum += per * gap;
		count = 1;
		if (avail > 0 && sum > 0)
		{
			count = (int32)((avail + gap) / sum);
			if (count < 1)
				count = 1;
		}
	}
	if (count * per > GRID_MAX_TRACKS)
		count = GRID_MAX_TRACKS / per;
	size = g->nnames - nnames;
	for (k = 1; k < count; k++)
	{
		for (i = 0; i < per; i++)
			grid_add_track(g, &g->tracks[first + i]);
		for (i = 0; i < size; i++)
			grid_add_name(g, g->names[nnames + i].name,
						  g->names[nnames + i].line + k * per);
	}
}

static void
grid_list(lo_GridParse *g, int32 avail, int32 gap, Bool in_repeat)
{
	lo_CSSGridTrack t;
	const char *before;

	for (;;)
	{
		grid_skip(g);
		if (!*g->p || (in_repeat && *g->p == ')'))
			return;
		if (*g->p == '[')
		{
			grid_line_names(g);
			continue;
		}
		if (!strncasecomp(g->p, "repeat(", 7) && !in_repeat)
		{
			g->p += 7;
			grid_repeat(g, avail, gap);
			continue;
		}
		before = g->p;
		if (grid_track(g, &t))
			grid_add_track(g, &t);
		if (g->p == before)
			g->p++;				/* a stray ')' */
	}
}

static void
grid_parse_tracks(lo_CSSGridRec *grid, int axis, MWContext *context,
				  lo_DocState *state, StyleStruct *style_struct,
				  char *list, char *autos, int32 avail)
{
	lo_GridParse g;

	XP_MEMSET(&g, 0, sizeof g);
	g.context = context;
	g.state = state;
	g.style = style_struct;
	if (list && strcasecomp(list, "none"))
	{
		g.p = list;
		grid_list(&g, avail, grid->gap[axis], FALSE);
	}
	grid->tracks[axis] = g.tracks;
	grid->ntracks[axis] = g.n;
	grid->names[axis] = g.names;
	grid->nnames[axis] = g.nnames;
	XP_MEMSET(&g, 0, sizeof g);
	g.context = context;
	g.state = state;
	g.style = style_struct;
	if (autos)
	{
		g.p = autos;
		grid_list(&g, avail, grid->gap[axis], TRUE);
	}
	grid->autos[axis] = g.tracks;
	grid->nautos[axis] = g.n;
	while (g.nnames > 0)
		XP_FREE(g.names[--g.nnames].name);
	XP_FREEIF(g.names);
}

/* grid-template-areas: '"a a b" "c . b"'.  Each area's lines are named
 * NAME-start and NAME-end; the areas make the explicit grid at least as
 * big as they are. */
static void
grid_parse_areas(lo_CSSGridRec *grid, char *areas)
{
	char *cells[64][64];
	int32 rows = 0, cols = 0, r, c, r2, c2, k;
	char *p = areas, word[128];
	lo_GridParse g[2];

	if (!areas)
		return;
	XP_MEMSET(cells, 0, sizeof cells);
	while (*p && rows < 64)
	{
		int32 n = 0;

		while (*p && *p != '"')
			p++;
		if (!*p)
			break;
		p++;
		while (*p && *p != '"')
		{
			int32 w = 0;

			while (*p == ' ')
				p++;
			if (*p == '"')
				break;
			if (*p == '.')
			{
				while (*p == '.')
					p++;
				word[0] = '\0';
			}
			else
			{
				while (*p && *p != ' ' && *p != '"')
				{
					if (w < (int32)sizeof word - 1)
						word[w++] = *p;
					p++;
				}
				word[w] = '\0';
			}
			if (n < 64)
				cells[rows][n] = word[0] ? XP_STRDUP(word) : NULL;
			n++;
		}
		if (*p == '"')
			p++;
		if (n > cols)
			cols = n > 64 ? 64 : n;
		rows++;
	}
	/* the names, as the template's are */
	for (k = 0; k < 2; k++)
	{
		XP_MEMSET(&g[k], 0, sizeof g[k]);
		g[k].names = grid->names[k];
		g[k].nnames = g[k].names_cap = grid->nnames[k];
	}
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++)
		{
			char *name = cells[r][c], buf[140];

			if (!name)
				continue;
			/* the first cell of an area: its extent */
			for (r2 = 0; r2 < r; r2++)
				for (c2 = 0; c2 < cols; c2++)
					if (cells[r2][c2] && !strcmp(cells[r2][c2], name))
						goto seen;
			for (c2 = 0; c2 < c; c2++)
				if (cells[r][c2] && !strcmp(cells[r][c2], name))
					goto seen;
			r2 = r;
			while (r2 + 1 < rows && cells[r2 + 1][c] &&
				   !strcmp(cells[r2 + 1][c], name))
				r2++;
			c2 = c;
			while (c2 + 1 < cols && cells[r][c2 + 1] &&
				   !strcmp(cells[r][c2 + 1], name))
				c2++;
			PR_snprintf(buf, sizeof buf, "%s-start", name);
			grid_add_name(&g[0], buf, r + 1);
			grid_add_name(&g[1], buf, c + 1);
			PR_snprintf(buf, sizeof buf, "%s-end", name);
			grid_add_name(&g[0], buf, r2 + 2);
			grid_add_name(&g[1], buf, c2 + 2);
		  seen:
			;
		}
	for (k = 0; k < 2; k++)
	{
		grid->names[k] = g[k].names;
		grid->nnames[k] = g[k].nnames;
	}
	for (r = 0; r < 64; r++)
		for (c = 0; c < 64; c++)
			XP_FREEIF(cells[r][c]);
	/* the explicit grid: at least the areas' rows and columns, sized as
	 * implicit tracks are */
	for (k = 0; k < 2; k++)
	{
		int32 want = k == 0 ? rows : cols;

		if (grid->ntracks[k] < want)
		{
			lo_CSSGridTrack *t = (lo_CSSGridTrack *)XP_REALLOC(grid->tracks[k],
				want * sizeof(lo_CSSGridTrack));

			if (!t)
				continue;
			for (r = grid->ntracks[k]; r < want; r++)
			{
				if (grid->nautos[k] > 0)
					t[r] = grid->autos[k][(r - grid->ntracks[k]) %
										  grid->nautos[k]];
				else
				{
					t[r].min_kind = t[r].max_kind = LO_GT_AUTO;
					t[r].min_v = t[r].max_v = 0;
				}
			}
			grid->tracks[k] = t;
			grid->ntracks[k] = want;
		}
	}
}

static intn
grid_align_value(char *v, intn dflt)
{
	if (!v)
		return dflt;
	if (!strcmp(v, "start"))
		return LO_FLEX_ALIGN_START;
	if (!strcmp(v, "center"))
		return LO_FLEX_ALIGN_CENTER;
	if (!strcmp(v, "end"))
		return LO_FLEX_ALIGN_END;
	return LO_FLEX_ALIGN_STRETCH;
}

static intn
grid_content_value(char *v)
{
	if (!v)
		return -1;
	if (!strcmp(v, "end"))
		return LO_FLEX_JUSTIFY_END;
	if (!strcmp(v, "center"))
		return LO_FLEX_JUSTIFY_CENTER;
	if (!strcmp(v, "space-between"))
		return LO_FLEX_JUSTIFY_BETWEEN;
	if (!strcmp(v, "space-around"))
		return LO_FLEX_JUSTIFY_AROUND;
	if (!strcmp(v, "space-evenly"))
		return LO_FLEX_JUSTIFY_EVENLY;
	return LO_FLEX_JUSTIFY_START;
}

/* a gap: pixels, or a percentage of the grid's size (in GAP_PCT) */
static void
grid_gap(MWContext *context, lo_DocState *state, StyleStruct *style_struct,
		 char *name, int32 *gap, double *gap_pct)
{
	SS_Number *num = STYLESTRUCT_GetNumber(style_struct, name);

	*gap = 0;
	*gap_pct = -1;
	if (!num)
		return;
	if (num->units && !strcasecomp(num->units, "%"))
		*gap_pct = num->value;
	else
	{
		LO_AdjustSSUnits(num, WIDTH_STYLE, context, state);
		if (num->value > 0)
			*gap = (int32)num->value;
	}
	STYLESTRUCT_FreeSSNumber(style_struct, num);
}

/*
 * The grid of a container whose content is WIDTH wide (for auto-fill
 * repetitions and percentage gaps), from nscss's text.
 */
lo_CSSGridRec *
lo_cssgrid_new(MWContext *context, lo_DocState *state, StyleStruct *style_struct,
			int32 width)
{
	lo_CSSGridRec *grid = XP_NEW_ZAP(lo_CSSGridRec);
	char *cols, *rows, *areas, *acols, *arows, *v;

	if (!grid)
		return NULL;
	grid_gap(context, state, style_struct, GRIDROWGAP_STYLE,
			 &grid->gap[0], &grid->gap_pct[0]);
	grid_gap(context, state, style_struct, FLEXGAP_STYLE,
			 &grid->gap[1], &grid->gap_pct[1]);
	if (grid->gap_pct[1] >= 0 && width > 0)
		grid->gap[1] = (int32)(grid->gap_pct[1] * width / 100);
	cols = STYLESTRUCT_GetString(style_struct, GRIDCOLS_STYLE);
	rows = STYLESTRUCT_GetString(style_struct, GRIDROWS_STYLE);
	areas = STYLESTRUCT_GetString(style_struct, GRIDAREAS_STYLE);
	acols = STYLESTRUCT_GetString(style_struct, GRIDAUTOCOLS_STYLE);
	arows = STYLESTRUCT_GetString(style_struct, GRIDAUTOROWS_STYLE);
	grid_parse_tracks(grid, 1, context, state, style_struct, cols, acols, width);
	grid_parse_tracks(grid, 0, context, state, style_struct, rows, arows, -1);
	grid_parse_areas(grid, areas);
	XP_FREEIF(cols);
	XP_FREEIF(rows);
	XP_FREEIF(areas);
	XP_FREEIF(acols);
	XP_FREEIF(arows);
	if ((v = STYLESTRUCT_GetString(style_struct, GRIDFLOW_STYLE)) != NULL)
	{
		grid->flow_column = !strncmp(v, "column", 6);
		grid->dense = strstr(v, "dense") != NULL;
		XP_FREE(v);
	}
	v = STYLESTRUCT_GetString(style_struct, GRIDJUSTIFY_STYLE);
	grid->justify_items = grid_align_value(v, LO_FLEX_ALIGN_STRETCH);
	XP_FREEIF(v);
	v = STYLESTRUCT_GetString(style_struct, GRIDALIGNCONTENT_STYLE);
	grid->align_content = grid_content_value(v);
	XP_FREEIF(v);
	if (lo_TableTrace())
		fprintf(lo_TableTrace(), "grid %p: %ld rows %ld cols (auto %ld/%ld) gaps %ld/%ld flow %s%s\n",
			(void *)grid, (long)grid->ntracks[0], (long)grid->ntracks[1],
			(long)grid->nautos[0], (long)grid->nautos[1],
			(long)grid->gap[0], (long)grid->gap[1],
			grid->flow_column ? "column" : "row", grid->dense ? " dense" : "");
	return grid;
}

void
lo_cssgrid_free(lo_CSSGridRec *grid)
{
	int k;

	if (!grid)
		return;
	for (k = 0; k < 2; k++)
	{
		XP_FREEIF(grid->tracks[k]);
		XP_FREEIF(grid->autos[k]);
		while (grid->nnames[k] > 0)
			XP_FREE(grid->names[k][--grid->nnames[k]].name);
		XP_FREEIF(grid->names[k]);
		XP_FREEIF(grid->size[k]);
		XP_FREEIF(grid->pos[k]);
	}
	XP_FREEIF(grid->area);
	XP_FREE(grid);
}

/* ---- an item's lines ---- */

/* the COUNTth line named NAME (from the end if COUNT < 0), or 0 */
static int32
grid_named_line(lo_CSSGridRec *grid, int axis, char *name, int32 count)
{
	int32 lines[GRID_MAX_TRACKS + 1], n = 0, i, k;

	for (i = 0; i < grid->nnames[axis]; i++)
		if (!strcmp(grid->names[axis][i].name, name) &&
			n < GRID_MAX_TRACKS + 1)
		{
			/* in order, without repeats */
			int32 l = grid->names[axis][i].line;

			for (k = n; k > 0 && lines[k - 1] > l; k--)
				lines[k] = lines[k - 1];
			if (k > 0 && lines[k - 1] == l)
			{
				XP_MEMMOVE(&lines[k], &lines[k + 1], (n - k) * sizeof(int32));
				continue;
			}
			lines[k] = l;
			n++;
		}
	if (n == 0 || count == 0)
		return 0;
	if (count > 0)
		return count <= n ? lines[count - 1] : 0;
	return -count <= n ? lines[n + count] : 0;
}

/* one line of an item: "auto", "3", "-1", "span 2", "name", "name 2" */
static int32
grid_line(lo_CSSGridRec *grid, int axis, Bool end, char *spec)
{
	char *w[3], *p, buf[160];
	int32 nw = 0, i, count = 0, line;
	Bool span = FALSE;
	char *name = NULL;

	strncpy(buf, spec, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	for (p = XP_STRTOK(buf, " "); p && nw < 3; p = XP_STRTOK(NULL, " "))
		w[nw++] = p;
	for (i = 0; i < nw; i++)
	{
		if (!strcasecomp(w[i], "span"))
			span = TRUE;
		else if (!strcasecomp(w[i], "auto"))
			return 0;
		else if (isdigit((unsigned char)w[i][0]) || w[i][0] == '-' ||
				 w[i][0] == '+')
			count = atoi(w[i]);
		else
			name = w[i];
	}
	if (span)
		return -(count > 0 ? count : 1);	/* span name: one track */
	if (name)
	{
		char implicit[180];

		/* NAME-start / NAME-end (an area's), then NAME */
		PR_snprintf(implicit, sizeof implicit, "%s-%s", name,
					end ? "end" : "start");
		line = grid_named_line(grid, axis, implicit, count ? count : 1);
		if (line == 0)
			line = grid_named_line(grid, axis, name, count ? count : 1);
		return line;
	}
	if (count < 0)
	{
		/* from the end of the explicit grid */
		line = grid->ntracks[axis] + 2 + count;
		return line >= 1 ? line : 1;
	}
	return count;
}

/* nscss's "row-start / column-start / row-end / column-end" into LINES
 * (as lo_TableCell.grid_lines) */
void
lo_cssgrid_item_lines(lo_CSSGridRec *grid, char *spec, int32 *lines)
{
	char *parts[4], *p;
	int32 k;

	for (k = 0; k < 4; k++)
		lines[k] = 0;
	if (!grid || !spec)
		return;
	parts[0] = spec;
	for (k = 1, p = spec; k < 4 && (p = strchr(p, '/')) != NULL; k++)
	{
		*p++ = '\0';
		parts[k] = p;
	}
	for (; k < 4; k++)
		parts[k] = "auto";
	for (k = 0; k < 4; k++)
	{
		while (*parts[k] == ' ')
			parts[k]++;
		p = parts[k] + XP_STRLEN(parts[k]);
		while (p > parts[k] && p[-1] == ' ')
			*--p = '\0';
		lines[k] = grid_line(grid, k & 1, k >= 2, parts[k]);
	}
}

/* ---- placement ---- */

/* the definite span on one axis from its two lines: START (from 0) and
 * END (exclusive); or auto (*start -1) with its span in *end */
static void
grid_resolve(int32 s, int32 e, int32 *start, int32 *end)
{
	if (s > 0 && e > 0)
	{
		*start = s < e ? s - 1 : e - 1;
		*end = s < e ? e - 1 : s - 1;
		if (*end == *start)
			(*end)++;
	}
	else if (s > 0)
	{
		*start = s - 1;
		*end = *start + (e < 0 ? -e : 1);
	}
	else if (e > 0)
	{
		*end = e - 1;
		*start = *end - (s < 0 ? -s : 1);
		if (*start < 0)
		{
			/* before the grid's first line: from it */
			*end -= *start;
			*start = 0;
		}
		if (*end <= *start)
			*end = *start + 1;
	}
	else
	{
		*start = -1;
		*end = s < 0 ? -s : (e < 0 ? -e : 1);
	}
}

static Bool
grid_free_at(int32 *area, Bool *placed, int32 n, int32 r0, int32 c0,
			 int32 r1, int32 c1)
{
	int32 i;

	for (i = 0; i < n; i++)
	{
		int32 *a = &area[4 * i];

		if (!placed[i])
			continue;
		if (a[0] < r1 && r0 < a[2] && a[1] < c1 && c0 < a[3])
			return FALSE;
	}
	return TRUE;
}

/*
 * Grid item placement (CSS grid 8.5), in the flow's terms: M is the axis
 * the flow fills (columns for row flow), N the one it moves along (rows).
 */
static void
grid_place(lo_CSSGridRec *grid, int32 n, lo_TableCell **cells)
{
	int32 *area = grid->area, i, k, seq[256], m_count;
	int32 ms[256], me[256], ns[256], ne[256];
	Bool placed[256];
	int M = grid->flow_column ? 0 : 1, N = 1 - M;
	int32 cur_n = 0, cur_m = 0;

	/* the items in order-modified document order */
	for (i = 0; i < n; i++)
	{
		int32 o = cells[i] ? cells[i]->flex_order : 0;

		for (k = i; k > 0 && (cells[seq[k - 1]] ? cells[seq[k - 1]]->flex_order
							  : 0) > o; k--)
			seq[k] = seq[k - 1];
		seq[k] = i;
	}
	for (i = 0; i < n; i++)
	{
		int32 *l = cells[i] ? cells[i]->grid_lines : NULL;
		int32 r0, r1, c0, c1;

		placed[i] = FALSE;
		grid_resolve(l ? l[0] : 0, l ? l[2] : 0, &r0, &r1);
		grid_resolve(l ? l[1] : 0, l ? l[3] : 0, &c0, &c1);
		ns[i] = N == 0 ? r0 : c0;
		ne[i] = N == 0 ? r1 : c1;
		ms[i] = M == 0 ? r0 : c0;
		me[i] = M == 0 ? r1 : c1;
	}
#define AREA_SET(i, n0, m0, n1, m1) do { \
		int32 *a_ = &area[4 * (i)]; \
		a_[N] = (n0); a_[M] = (m0); a_[2 + N] = (n1); a_[2 + M] = (m1); \
		placed[i] = TRUE; } while (0)

	/* 1. definite on both axes */
	for (i = 0; i < n; i++)
		if (ns[i] >= 0 && ms[i] >= 0)
			AREA_SET(i, ns[i], ms[i], ne[i], me[i]);
	/* 2. locked to a track of N (a row, for row flow) */
	{
		int32 cursor[GRID_MAX_TRACKS];

		XP_MEMSET(cursor, 0, sizeof cursor);
		for (k = 0; k < n; k++)
		{
			int32 span, m;

			i = seq[k];
			if (placed[i] || ns[i] < 0)
				continue;
			span = me[i];
			m = grid->dense || ns[i] >= GRID_MAX_TRACKS ? 0 : cursor[ns[i]];
			while (!grid_free_at(area, placed, n,
					N == 0 ? ns[i] : m, N == 0 ? m : ns[i],
					N == 0 ? ne[i] : m + span, N == 0 ? m + span : ne[i]))
				m++;
			AREA_SET(i, ns[i], m, ne[i], m + span);
			if (ns[i] < GRID_MAX_TRACKS)
				cursor[ns[i]] = m + span;
		}
	}
	/* 3. the tracks along M: the explicit ones, and enough for every
	 * item placed or to place */
	m_count = grid->ntracks[M];
	for (i = 0; i < n; i++)
	{
		/* placed: its end; else its end, or (auto) its span */
		int32 want = placed[i] ? area[4 * i + 2 + M] : me[i];

		if (want > m_count)
			m_count = want;
	}
	if (m_count < 1)
		m_count = 1;
	/* 4. the rest, from the cursor */
	for (k = 0; k < n; k++)
	{
		int32 span_n, span_m;

		i = seq[k];
		if (placed[i])
			continue;
		span_n = ne[i];			/* auto on N: its span */
		if (grid->dense)
			cur_n = cur_m = 0;
		if (ms[i] >= 0)
		{
			/* definite on M */
			if (!grid->dense && ms[i] < cur_m)
				cur_n++;
			cur_m = ms[i];
			while (!grid_free_at(area, placed, n,
					N == 0 ? cur_n : ms[i], N == 0 ? ms[i] : cur_n,
					N == 0 ? cur_n + span_n : me[i],
					N == 0 ? me[i] : cur_n + span_n))
				cur_n++;
			AREA_SET(i, cur_n, ms[i], cur_n + span_n, me[i]);
			cur_m = me[i];
			continue;
		}
		span_m = me[i];
		for (;;)
		{
			if (cur_m + span_m > m_count && cur_m > 0)
			{
				cur_n++;
				cur_m = 0;
				continue;
			}
			if (grid_free_at(area, placed, n,
					N == 0 ? cur_n : cur_m, N == 0 ? cur_m : cur_n,
					N == 0 ? cur_n + span_n : cur_m + span_m,
					N == 0 ? cur_m + span_m : cur_n + span_n))
				break;
			cur_m++;
		}
		AREA_SET(i, cur_n, cur_m, cur_n + span_n, cur_m + span_m);
		cur_m += span_m;
	}
#undef AREA_SET
	/* the grid's tracks: the explicit ones and the items' */
	for (k = 0; k < 2; k++)
	{
		grid->n[k] = grid->ntracks[k];
		for (i = 0; i < n; i++)
			if (area[4 * i + 2 + k] > grid->n[k])
				grid->n[k] = area[4 * i + 2 + k];
		if (grid->n[k] > GRID_MAX_TRACKS)
			grid->n[k] = GRID_MAX_TRACKS;
	}
}

/* ---- track sizing ---- */

static lo_CSSGridTrack *
grid_track_at(lo_CSSGridRec *grid, int axis, int32 i, lo_CSSGridTrack *dflt)
{
	if (i < grid->ntracks[axis])
		return &grid->tracks[axis][i];
	if (grid->nautos[axis] > 0)
		return &grid->autos[axis][(i - grid->ntracks[axis]) %
								  grid->nautos[axis]];
	return dflt;
}

/* how alignment distributes LEFT over N tracks: before the first, and
 * between each */
static void
grid_distribute(intn how, int32 left, int32 n, int32 *lead, int32 *between)
{
	*lead = *between = 0;
	if (left <= 0 || n <= 0)
		return;
	switch (how)
	{
	case LO_FLEX_JUSTIFY_END: *lead = left; break;
	case LO_FLEX_JUSTIFY_CENTER: *lead = left / 2; break;
	case LO_FLEX_JUSTIFY_BETWEEN: if (n > 1) *between = left / (n - 1); break;
	case LO_FLEX_JUSTIFY_AROUND: *between = left / n; *lead = *between / 2; break;
	case LO_FLEX_JUSTIFY_EVENLY: *between = left / (n + 1); *lead = *between; break;
	default: break;
	}
}

/*
 * One axis's tracks (CSS grid 11, simplified): fixed sizes; intrinsic
 * ones from the items in them (one track first, then the spanning ones);
 * free space up to the growth limits; fr tracks share what is left (or,
 * with no definite size, are as big as their content needs); auto tracks
 * stretch into the rest when content alignment is normal.  Then the
 * offsets, with content alignment and the gaps.  AVAIL < 0: indefinite.
 * Returns the tracks' extent.
 */
static int32
grid_size_axis(lo_CSSGridRec *grid, int axis, int32 n, int32 *minc, int32 *maxc,
			   int32 avail, intn content_align)
{
	int32 count = grid->n[axis], gap = grid->gap[axis], i, k, t;
	int32 *size, *pos;
	double *base, *limit, frsum = 0;
	lo_CSSGridTrack auto_track, **tr;
	Bool any_fr = FALSE;
	int32 used, lead, between;

	auto_track.min_kind = auto_track.max_kind = LO_GT_AUTO;
	auto_track.min_v = auto_track.max_v = 0;
	if (count < 1)
		count = grid->n[axis] = 0;
	grid->size[axis] = size = (int32 *)XP_REALLOC(grid->size[axis],
		(count + 1) * sizeof(int32));
	grid->pos[axis] = pos = (int32 *)XP_REALLOC(grid->pos[axis],
		(count + 1) * sizeof(int32));
	base = (double *)XP_ALLOC((count + 1) * sizeof(double));
	limit = (double *)XP_ALLOC((count + 1) * sizeof(double));
	tr = (lo_CSSGridTrack **)XP_ALLOC((count + 1) * sizeof(lo_CSSGridTrack *));
	if (!size || !pos || !base || !limit || !tr)
	{
		XP_FREEIF(base);
		XP_FREEIF(limit);
		XP_FREEIF(tr);
		grid->n[axis] = 0;
		return 0;
	}
	if (grid->gap_pct[axis] >= 0)
		gap = avail > 0 ? (int32)(grid->gap_pct[axis] * avail / 100) : 0;
	grid->gap[axis] = gap;

	/* the sizing functions: fixed sizes, and where to grow */
	for (t = 0; t < count; t++)
	{
		lo_CSSGridTrack *g = grid_track_at(grid, axis, t, &auto_track);

		tr[t] = g;
		base[t] = 0;
		limit[t] = GRID_INF;
		switch (g->min_kind)
		{
		case LO_GT_PX: base[t] = g->min_v; break;
		case LO_GT_PCT: base[t] = avail >= 0 ? g->min_v * avail / 100 : 0; break;
		default: break;
		}
		switch (g->max_kind)
		{
		case LO_GT_PX: limit[t] = g->max_v; break;
		case LO_GT_PCT:
			if (avail >= 0)
				limit[t] = g->max_v * avail / 100;
			break;
		case LO_GT_FR: any_fr = TRUE; frsum += g->max_v; break;
		default: break;
		}
	}
	/* items in one track: intrinsic minimums and maximums */
	for (i = 0; i < n; i++)
	{
		int32 a0 = grid->area[4 * i + axis], a1 = grid->area[4 * i + 2 + axis];
		lo_CSSGridTrack *g;

		if (a1 - a0 != 1 || a0 >= count)
			continue;
		g = tr[a0];
		if (g->min_kind == LO_GT_AUTO || g->min_kind == LO_GT_MIN)
		{
			if (minc[i] > base[a0])
				base[a0] = minc[i];
		}
		else if (g->min_kind == LO_GT_MAX)
		{
			if (maxc[i] > base[a0])
				base[a0] = maxc[i];
		}
		else if (g->min_kind == LO_GT_PCT && avail < 0 && minc[i] > base[a0])
			base[a0] = minc[i];
		if (g->max_kind == LO_GT_AUTO || g->max_kind == LO_GT_MAX ||
			(g->max_kind == LO_GT_PCT && avail < 0))
		{
			if (limit[a0] >= GRID_INF || maxc[i] > limit[a0])
				limit[a0] = maxc[i];
		}
		else if (g->max_kind == LO_GT_MIN)
		{
			if (limit[a0] >= GRID_INF || minc[i] > limit[a0])
				limit[a0] = minc[i];
		}
		else if (g->max_kind == LO_GT_FIT)
		{
			double f = maxc[i] < g->max_v ? maxc[i] : g->max_v;

			if (f < minc[i])
				f = minc[i];
			if (limit[a0] >= GRID_INF || f > limit[a0])
				limit[a0] = f;
		}
	}
	/* items spanning tracks (none flexible): what the intrinsic ones
	 * lack, shared equally */
	for (i = 0; i < n; i++)
	{
		int32 a0 = grid->area[4 * i + axis], a1 = grid->area[4 * i + 2 + axis];
		int32 intrinsic = 0;
		double have = 0, have_l = 0, extra;
		Bool flexible = FALSE;

		if (a1 - a0 <= 1)
			continue;
		if (a1 > count)
			a1 = count;
		for (t = a0; t < a1; t++)
		{
			if (tr[t]->max_kind == LO_GT_FR)
				flexible = TRUE;
			if (tr[t]->min_kind >= LO_GT_AUTO)
				intrinsic++;
			have += base[t];
			have_l += limit[t] >= GRID_INF ? base[t] : limit[t];
		}
		if (flexible || intrinsic == 0)
			continue;
		have += (a1 - a0 - 1) * gap;
		have_l += (a1 - a0 - 1) * gap;
		extra = minc[i] - have;
		if (extra > 0)
			for (t = a0; t < a1; t++)
				if (tr[t]->min_kind >= LO_GT_AUTO)
					base[t] += extra / intrinsic;
		extra = maxc[i] - have_l;
		for (t = a0; t < a1; t++)
			if (tr[t]->max_kind >= LO_GT_AUTO && tr[t]->max_kind != LO_GT_FR)
			{
				double l = limit[t] >= GRID_INF ? base[t] : limit[t];

				limit[t] = l + (extra > 0 ? extra / intrinsic : 0);
			}
	}
	for (t = 0; t < count; t++)
	{
		if (limit[t] >= GRID_INF)
			limit[t] = base[t];
		if (limit[t] < base[t])
			limit[t] = base[t];
	}
	/* free space: grow the tracks to their limits */
	used = (count > 1 ? (count - 1) * gap : 0);
	for (t = 0; t < count; t++)
		used += (int32)base[t];
	if (avail >= 0)
	{
		double free_space = avail - used;
		int32 growing;

		while (free_space > 0.5)
		{
			double share, taken = 0;

			growing = 0;
			for (t = 0; t < count; t++)
				if (tr[t]->max_kind != LO_GT_FR && limit[t] > base[t])
					growing++;
			if (growing == 0)
				break;
			share = free_space / growing;
			for (t = 0; t < count; t++)
				if (tr[t]->max_kind != LO_GT_FR && limit[t] > base[t])
				{
					double add = limit[t] - base[t] < share
						? limit[t] - base[t] : share;

					base[t] += add;
					taken += add;
				}
			free_space -= taken;
			if (taken <= 0)
				break;
		}
	}
	else
	{
		/* no definite size: every track at its limit */
		for (t = 0; t < count; t++)
			if (tr[t]->max_kind != LO_GT_FR)
				base[t] = limit[t];
	}
	/* flexible tracks */
	if (any_fr)
	{
		double fr_size = 0, left;
		Bool *inflexible = (Bool *)XP_CALLOC(count + 1, sizeof(Bool));

		if (avail >= 0 && inflexible)
		{
			/* what is left, by fr; a track whose content needs more
			 * keeps that, and the rest share again */
			for (;;)
			{
				double sum_fr = 0;
				Bool again = FALSE;

				left = avail - (count > 1 ? (count - 1) * gap : 0);
				for (t = 0; t < count; t++)
					if (tr[t]->max_kind != LO_GT_FR || inflexible[t])
						left -= base[t];
					else
						sum_fr += tr[t]->max_v;
				if (sum_fr <= 0)
					break;
				fr_size = left / (sum_fr < 1 ? 1 : sum_fr);
				for (t = 0; t < count; t++)
					if (tr[t]->max_kind == LO_GT_FR && !inflexible[t] &&
						fr_size * tr[t]->max_v < base[t])
					{
						inflexible[t] = TRUE;
						again = TRUE;
					}
				if (!again)
					break;
			}
			for (t = 0; t < count; t++)
				if (tr[t]->max_kind == LO_GT_FR && !inflexible[t])
				{
					double s = fr_size * tr[t]->max_v;

					base[t] = s > base[t] ? s : base[t];
				}
		}
		else
		{
			/* as big as the content: the largest base per fr */
			for (t = 0; t < count; t++)
				if (tr[t]->max_kind == LO_GT_FR && tr[t]->max_v > 0)
				{
					double per = base[t] / (tr[t]->max_v < 1 ? 1 : tr[t]->max_v);

					if (per > fr_size)
						fr_size = per;
				}
			for (i = 0; i < n; i++)
			{
				int32 a0 = grid->area[4 * i + axis];
				int32 a1 = grid->area[4 * i + 2 + axis];
				double fixed = (a1 - a0 - 1) * gap, frs = 0;

				for (t = a0; t < a1 && t < count; t++)
					if (tr[t]->max_kind == LO_GT_FR)
						frs += tr[t]->max_v;
					else
						fixed += base[t];
				if (frs > 0 && (maxc[i] - fixed) / (frs < 1 ? 1 : frs) > fr_size)
					fr_size = (maxc[i] - fixed) / (frs < 1 ? 1 : frs);
			}
			for (t = 0; t < count; t++)
				if (tr[t]->max_kind == LO_GT_FR)
				{
					double s = fr_size * tr[t]->max_v;

					base[t] = s > base[t] ? s : base[t];
				}
		}
		XP_FREEIF(inflexible);
	}
	used = (count > 1 ? (count - 1) * gap : 0);
	for (t = 0; t < count; t++)
	{
		size[t] = (int32)(base[t] + 0.5);
		used += size[t];
	}
	/* content alignment normal: auto tracks stretch */
	if (avail > used && content_align < 0 && !any_fr)
	{
		int32 autos = 0, left = avail - used;

		for (t = 0; t < count; t++)
			if (tr[t]->max_kind == LO_GT_AUTO)
				autos++;
		for (t = 0; t < count && autos > 0; t++)
			if (tr[t]->max_kind == LO_GT_AUTO)
			{
				int32 add = left / autos;

				size[t] += add;
				left -= add;
				autos--;
			}
		used = avail;
	}
	grid_distribute(content_align, avail - used, count, &lead, &between);
	k = lead;
	for (t = 0; t < count; t++)
	{
		pos[t] = k;
		k += size[t] + gap + between;
	}
	pos[count] = k;
	XP_FREE(base);
	XP_FREE(limit);
	XP_FREE(tr);
	return count > 0 ? pos[count - 1] + size[count - 1] : 0;
}

/* an item's extent in its area: stretched, or its own size aligned */
static void
grid_align_item(intn align, int32 area_pos, int32 area_size, int32 own,
				int32 *pos, int32 *size)
{
	if (align == LO_FLEX_ALIGN_STRETCH || own < 0)
	{
		*pos = area_pos;
		*size = area_size;
		return;
	}
	*size = own;
	*pos = area_pos;
	if (align == LO_FLEX_ALIGN_CENTER)
		*pos += (area_size - own) / 2;
	else if (align == LO_FLEX_ALIGN_END)
		*pos += area_size - own;
}

static int32
grid_span(lo_CSSGridRec *grid, int axis, int32 i)
{
	int32 a0 = grid->area[4 * i + axis], a1 = grid->area[4 * i + 2 + axis];

	if (a1 > grid->n[axis])
		a1 = grid->n[axis];
	if (a0 >= a1)
		return 0;
	return grid->pos[axis][a1 - 1] + grid->size[axis][a1 - 1]
		- grid->pos[axis][a0];
}

void
lo_cssgrid_widths(lo_TableRec *table, int32 n, lo_TableCell **cells,
			   int32 *minw, int32 *maxw, int32 avail, int32 *w, int32 *x)
{
	lo_CSSGridRec *grid = table->grid;
	int32 i;

	if (!grid || n <= 0)
		return;
	if (n > 256)
		n = 256;
	grid->area = (int32 *)XP_REALLOC(grid->area, 4 * n * sizeof(int32));
	if (!grid->area)
	{
		grid->nitems = 0;
		return;
	}
	grid->nitems = n;
	grid_place(grid, n, cells);
	grid_size_axis(grid, 1, n, minw, maxw, avail,
				   table->flex_justify == LO_FLEX_JUSTIFY_START ? -1
				   : table->flex_justify);
	for (i = 0; i < n; i++)
	{
		int32 c0 = grid->area[4 * i + 1], own = -1, area_w;
		intn align = grid->justify_items;

		if (cells[i] && cells[i]->grid_justify >= 0)
			align = cells[i]->grid_justify;
		area_w = grid_span(grid, 1, i);
		if (cells[i] && cells[i]->flex_basis >= 0)
		{
			/* a width of its own: not stretched */
			own = cells[i]->flex_basis;
			if (align == LO_FLEX_ALIGN_STRETCH)
				align = LO_FLEX_ALIGN_START;
		}
		else if (align != LO_FLEX_ALIGN_STRETCH)
			own = maxw[i] < area_w ? maxw[i] : area_w;
		grid_align_item(align, c0 < grid->n[1] ? grid->pos[1][c0] : 0,
						area_w, own, &x[i], &w[i]);
	}
	if (lo_TableTrace())
	{
		fprintf(lo_TableTrace(), "grid %p: %ld items, %ldx%ld, cols:",
			(void *)grid, (long)n, (long)grid->n[0], (long)grid->n[1]);
		for (i = 0; i < grid->n[1]; i++)
			fprintf(lo_TableTrace(), " %ld@%ld", (long)grid->size[1][i],
				(long)grid->pos[1][i]);
		fprintf(lo_TableTrace(), "; items:");
		for (i = 0; i < n; i++)
			fprintf(lo_TableTrace(), " [%ld %ld %ld %ld] %ld@%ld",
				(long)grid->area[4 * i], (long)grid->area[4 * i + 1],
				(long)grid->area[4 * i + 2], (long)grid->area[4 * i + 3],
				(long)w[i], (long)x[i]);
		fputc('\n', lo_TableTrace());
	}
}

int32
lo_cssgrid_heights(lo_TableRec *table, int32 n, lo_TableCell **cells,
				int32 *content_h, int32 height, int32 *y, int32 *h)
{
	lo_CSSGridRec *grid = table->grid;
	int32 i, total;

	if (!grid || grid->nitems <= 0)
		return 0;
	if (n > grid->nitems)
		n = grid->nitems;
	if (grid->gap_pct[0] >= 0 && height < 0)
		grid->gap[0] = 0;
	total = grid_size_axis(grid, 0, n, content_h, content_h, height,
						   grid->align_content);
	for (i = 0; i < n; i++)
	{
		int32 r0 = grid->area[4 * i], own = -1, area_h;
		/* align-self, or the grid's align-items (lo_flex_item_begin) */
		intn align = cells[i] ? cells[i]->flex_align
			: table->flex_align_items;
		area_h = grid_span(grid, 0, i);
		if (cells[i] && cells[i]->specified_height > 0)
		{
			/* a height of its own: not stretched */
			own = content_h[i];
			if (align == LO_FLEX_ALIGN_STRETCH)
				align = LO_FLEX_ALIGN_START;
		}
		else if (align != LO_FLEX_ALIGN_STRETCH)
			own = content_h[i] < area_h ? content_h[i] : area_h;
		grid_align_item(align, r0 < grid->n[0] ? grid->pos[0][r0] : 0,
						area_h, own, &y[i], &h[i]);
	}
	if (height > total)
		total = height;
	if (lo_TableTrace())
	{
		fprintf(lo_TableTrace(), "grid %p: rows:", (void *)grid);
		for (i = 0; i < grid->n[0]; i++)
			fprintf(lo_TableTrace(), " %ld@%ld", (long)grid->size[0][i],
				(long)grid->pos[0][i]);
		fprintf(lo_TableTrace(), "; %ld high\n", (long)total);
	}
	return total;
}
