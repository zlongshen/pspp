/* PSPP - a program for statistical analysis.
   Copyright (C) 2009 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <config.h>

#include <output/cairo.h>

#include <libpspp/assertion.h>
#include <libpspp/cast.h>
#include <libpspp/start-date.h>
#include <libpspp/str.h>
#include <libpspp/string-map.h>
#include <libpspp/version.h>
#include <output/cairo-chart.h>
#include <output/chart-item-provider.h>
#include <output/charts/boxplot.h>
#include <output/charts/np-plot.h>
#include <output/charts/piechart.h>
#include <output/charts/plot-hist.h>
#include <output/charts/roc-chart.h>
#include <output/charts/scree.h>
#include <output/driver-provider.h>
#include <output/options.h>
#include <output/render.h>
#include <output/tab.h>
#include <output/table-item.h>
#include <output/table.h>
#include <output/text-item.h>

#include <cairo/cairo-pdf.h>
#include <cairo/cairo-ps.h>
#include <cairo/cairo-svg.h>
#include <cairo/cairo.h>
#include <pango/pango-font.h>
#include <pango/pango-layout.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <stdlib.h>

#include "error.h"
#include "intprops.h"
#include "minmax.h"
#include "xalloc.h"

#include "gettext.h"
#define _(msgid) gettext (msgid)

/* This file uses TABLE_HORZ and TABLE_VERT enough to warrant abbreviating. */
#define H TABLE_HORZ
#define V TABLE_VERT

/* Measurements as we present to the rest of PSPP. */
#define XR_POINT PANGO_SCALE
#define XR_INCH (XR_POINT * 72)

/* Conversions to and from points. */
static double
xr_to_pt (int x)
{
  return x / (double) XR_POINT;
}

/* Output types. */
enum xr_output_type
  {
    XR_PDF,
    XR_PS,
    XR_SVG
  };

/* Cairo fonts. */
enum xr_font_type
  {
    XR_FONT_PROPORTIONAL,
    XR_FONT_EMPHASIS,
    XR_FONT_FIXED,
    XR_N_FONTS
  };

/* A font for use with Cairo. */
struct xr_font
  {
    char *string;
    PangoFontDescription *desc;
    PangoLayout *layout;
    PangoFontMetrics *metrics;
  };

/* Cairo output driver. */
struct xr_driver
  {
    struct output_driver driver;

    /* User parameters. */
    bool headers;               /* Draw headers at top of page? */

    struct xr_font fonts[XR_N_FONTS];
    int font_height;            /* In XR units. */

    int width;                  /* Page width minus margins. */
    int length;                 /* Page length minus margins and header. */

    int left_margin;            /* Left margin in XR units. */
    int right_margin;           /* Right margin in XR units. */
    int top_margin;             /* Top margin in XR units. */
    int bottom_margin;          /* Bottom margin in XR units. */

    int line_gutter;		/* Space around lines. */
    int line_space;		/* Space between lines. */
    int line_width;		/* Width of lines. */

    enum xr_output_type file_type; /* Type of output file. */

    /* Internal state. */
    struct render_params *params;
    char *title;
    char *subtitle;
    cairo_t *cairo;
    int page_number;		/* Current page number. */
    int y;
  };

static void xr_show_page (struct xr_driver *);
static void draw_headers (struct xr_driver *);

static bool load_font (struct xr_driver *, struct xr_font *);
static void free_font (struct xr_font *);

static void xr_draw_line (void *, int bb[TABLE_N_AXES][2],
                          enum render_line_style styles[TABLE_N_AXES][2]);
static void xr_measure_cell_width (void *, const struct table_cell *,
                                   int *min, int *max);
static int xr_measure_cell_height (void *, const struct table_cell *,
                                   int width);
static void xr_draw_cell (void *, const struct table_cell *,
                          int bb[TABLE_N_AXES][2],
                          int clip[TABLE_N_AXES][2]);

/* Driver initialization. */

static struct xr_driver *
xr_driver_cast (struct output_driver *driver)
{
  assert (driver->class == &cairo_class);
  return UP_CAST (driver, struct xr_driver, driver);
}

static struct driver_option *
opt (struct output_driver *d, struct string_map *options, const char *key,
     const char *default_value)
{
  return driver_option_get (d, options, key, default_value);
}

static struct xr_driver *
xr_allocate (const char *name, int device_type, struct string_map *o)
{
  struct output_driver *d;
  struct xr_driver *xr;

  xr = xzalloc (sizeof *xr);
  d = &xr->driver;
  output_driver_init (d, &cairo_class, name, device_type);
  xr->headers = true;
  xr->font_height = XR_POINT * 10;
  xr->fonts[XR_FONT_FIXED].string
    = parse_string (opt (d, o, "fixed-font", "monospace"));
  xr->fonts[XR_FONT_PROPORTIONAL].string
    = parse_string (opt (d, o, "prop-font", "serif"));
  xr->fonts[XR_FONT_EMPHASIS].string
    = parse_string (opt (d, o, "emph-font", "serif italic"));
  xr->line_gutter = XR_POINT;
  xr->line_space = XR_POINT;
  xr->line_width = XR_POINT / 2;
  xr->page_number = 1;

  return xr;
}

static bool
xr_set_cairo (struct xr_driver *xr, cairo_t *cairo)
{
  int i;

  xr->cairo = cairo;

  cairo_set_line_width (xr->cairo, xr_to_pt (xr->line_width));

  for (i = 0; i < XR_N_FONTS; i++)
    if (!load_font (xr, &xr->fonts[i]))
      return false;

  if (xr->params == NULL)
    {
      int single_width, double_width;

      xr->params = xmalloc (sizeof *xr->params);
      xr->params->draw_line = xr_draw_line;
      xr->params->measure_cell_width = xr_measure_cell_width;
      xr->params->measure_cell_height = xr_measure_cell_height;
      xr->params->draw_cell = xr_draw_cell;
      xr->params->aux = xr;
      xr->params->size[H] = xr->width;
      xr->params->size[V] = xr->length;
      xr->params->font_size[H] = xr->font_height / 2; /* XXX */
      xr->params->font_size[V] = xr->font_height;

      single_width = 2 * xr->line_gutter + xr->line_width;
      double_width = 2 * xr->line_gutter + xr->line_space + 2 * xr->line_width;
      for (i = 0; i < TABLE_N_AXES; i++)
        {
          xr->params->line_widths[i][RENDER_LINE_NONE] = 0;
          xr->params->line_widths[i][RENDER_LINE_SINGLE] = single_width;
          xr->params->line_widths[i][RENDER_LINE_DOUBLE] = double_width;
        }
    }

  return true;
}

static struct output_driver *
xr_create (const char *name, enum output_device_type device_type,
           struct string_map *o)
{
  enum { MIN_LENGTH = 3 };
  struct output_driver *d;
  struct xr_driver *xr;
  cairo_surface_t *surface;
  cairo_status_t status;
  double width_pt, length_pt;
  int paper_width, paper_length;
  char *file_name;

  xr = xr_allocate (name, device_type, o);
  d = &xr->driver;

  xr->headers = parse_boolean (opt (d, o, "headers", "true"));

  xr->file_type = parse_enum (opt (d, o, "output-type", "pdf"),
                              "pdf", XR_PDF,
                              "ps", XR_PS,
                              "svg", XR_SVG,
                              (char *) NULL);
  file_name = parse_string (opt (d, o, "output-file",
                                 (xr->file_type == XR_PDF ? "pspp.pdf"
                                  : xr->file_type == XR_PS ? "pspp.ps"
                                  : "pspp.svg")));

  parse_paper_size (opt (d, o, "paper-size", ""), &paper_width, &paper_length);
  xr->left_margin = parse_dimension (opt (d, o, "left-margin", ".5in"));
  xr->right_margin = parse_dimension (opt (d, o, "right-margin", ".5in"));
  xr->top_margin = parse_dimension (opt (d, o, "top-margin", ".5in"));
  xr->bottom_margin = parse_dimension (opt (d, o, "bottom-margin", ".5in"));

  if (xr->headers)
    xr->top_margin += 3 * xr->font_height;
  xr->width = paper_width - xr->left_margin - xr->right_margin;
  xr->length = paper_length - xr->top_margin - xr->bottom_margin;

  width_pt = paper_width / 1000.0;
  length_pt = paper_length / 1000.0;
  if (xr->file_type == XR_PDF)
    surface = cairo_pdf_surface_create (file_name, width_pt, length_pt);
  else if (xr->file_type == XR_PS)
    surface = cairo_ps_surface_create (file_name, width_pt, length_pt);
  else if (xr->file_type == XR_SVG)
    surface = cairo_svg_surface_create (file_name, width_pt, length_pt);
  else
    NOT_REACHED ();

  status = cairo_surface_status (surface);
  if (status != CAIRO_STATUS_SUCCESS)
    {
      error (0, 0, _("opening output file \"%s\": %s"),
             file_name, cairo_status_to_string (status));
      cairo_surface_destroy (surface);
      goto error;
    }

  xr->cairo = cairo_create (surface);
  cairo_surface_destroy (surface);

  cairo_translate (xr->cairo,
                   xr_to_pt (xr->left_margin),
                   xr_to_pt (xr->top_margin));

  if (!xr_set_cairo (xr, xr->cairo))
    goto error;

  if (xr->length / xr->font_height < MIN_LENGTH)
    {
      error (0, 0, _("The defined page is not long "
                     "enough to hold margins and headers, plus least %d "
                     "lines of the default fonts.  In fact, there's only "
                     "room for %d lines."),
             MIN_LENGTH,
             xr->length / xr->font_height);
      goto error;
    }

  free (file_name);
  return &xr->driver;

 error:
  output_driver_destroy (&xr->driver);
  return NULL;
}

static void
xr_destroy (struct output_driver *driver)
{
  struct xr_driver *xr = xr_driver_cast (driver);
  size_t i;

  if (xr->cairo != NULL)
    {
      cairo_status_t status;

      if (xr->y > 0)
        xr_show_page (xr);

      cairo_surface_finish (cairo_get_target (xr->cairo));
      status = cairo_status (xr->cairo);
      if (status != CAIRO_STATUS_SUCCESS)
        error (0, 0, _("error drawing output for %s driver: %s"),
               output_driver_get_name (driver),
               cairo_status_to_string (status));
      cairo_destroy (xr->cairo);
    }

  for (i = 0; i < XR_N_FONTS; i++)
    free_font (&xr->fonts[i]);
  free (xr->params);
  free (xr);
}

static void
xr_flush (struct output_driver *driver)
{
  struct xr_driver *xr = xr_driver_cast (driver);

  cairo_surface_flush (cairo_get_target (xr->cairo));
}

static void
xr_init_caption_cell (const char *caption, struct table_cell *cell)
{
  cell->contents = caption;
  cell->options = TAB_LEFT;
  cell->destructor = NULL;
}

static struct render_page *
xr_render_table_item (struct xr_driver *xr, const struct table_item *item,
                      int *caption_heightp)
{
  const char *caption = table_item_get_caption (item);

  if (caption != NULL)
    {
      /* XXX doesn't do well with very large captions */
      struct table_cell cell;
      xr_init_caption_cell (caption, &cell);
      *caption_heightp = xr_measure_cell_height (xr, &cell, xr->width);
    }
  else
    *caption_heightp = 0;

  return render_page_create (xr->params, table_item_get_table (item));
}

static void
xr_submit (struct output_driver *driver, const struct output_item *output_item)
{
  struct xr_driver *xr = xr_driver_cast (driver);
  if (is_table_item (output_item))
    {
      struct table_item *table_item = to_table_item (output_item);
      struct render_break x_break;
      struct render_page *page;
      int caption_height;

      if (xr->y > 0)
        xr->y += xr->font_height;

      page = xr_render_table_item (xr, table_item, &caption_height);
      xr->params->size[V] = xr->length - caption_height;
      for (render_break_init (&x_break, page, H);
           render_break_has_next (&x_break); )
        {
          struct render_page *x_slice;
          struct render_break y_break;

          x_slice = render_break_next (&x_break, xr->width);
          for (render_break_init (&y_break, x_slice, V);
               render_break_has_next (&y_break); )
            {
              int space = xr->length - xr->y;
              struct render_page *y_slice;

              /* XXX doesn't allow for caption or space between segments */
              if (render_break_next_size (&y_break) > space)
                {
                  assert (xr->y > 0);
                  xr_show_page (xr);
                  continue;
                }

              y_slice = render_break_next (&y_break, space);
              if (caption_height)
                {
                  struct table_cell cell;
                  int bb[TABLE_N_AXES][2];

                  xr_init_caption_cell (table_item_get_caption (table_item),
                                        &cell);
                  bb[H][0] = 0;
                  bb[H][1] = xr->width;
                  bb[V][0] = 0;
                  bb[V][1] = caption_height;
                  xr_draw_cell (xr, &cell, bb, bb);
                  xr->y += caption_height;
                  caption_height = 0;
                }

              render_page_draw (y_slice);
              xr->y += render_page_get_size (y_slice, V);
              render_page_unref (y_slice);
            }
          render_break_destroy (&y_break);
        }
      render_break_destroy (&x_break);
    }
  else if (is_chart_item (output_item))
    {
      if (xr->y > 0)
        xr_show_page (xr);
      xr_draw_chart (to_chart_item (output_item), xr->cairo, 0.0, 0.0,
                     xr_to_pt (xr->width), xr_to_pt (xr->length));
      xr_show_page (xr);
    }
  else if (is_text_item (output_item))
    {
      const struct text_item *text_item = to_text_item (output_item);
      enum text_item_type type = text_item_get_type (text_item);
      const char *text = text_item_get_text (text_item);

      switch (type)
        {
        case TEXT_ITEM_TITLE:
          free (xr->title);
          xr->title = xstrdup (text);
          break;

        case TEXT_ITEM_SUBTITLE:
          free (xr->subtitle);
          xr->subtitle = xstrdup (text);
          break;

        case TEXT_ITEM_COMMAND_CLOSE:
          break;

        case TEXT_ITEM_BLANK_LINE:
          if (xr->y > 0)
            xr->y += xr->font_height;
          break;

        case TEXT_ITEM_EJECT_PAGE:
          if (xr->y > 0)
            xr_show_page (xr);
          break;

        default:
          {
            struct table_item *item;

            item = table_item_create (table_from_string (0, text), NULL);
            xr_submit (&xr->driver, &item->output_item);
            table_item_unref (item);
          }
          break;
        }

    }
}

static void
xr_show_page (struct xr_driver *xr)
{
  if (xr->headers)
    {
      xr->y = 0;
      draw_headers (xr);
    }
  cairo_show_page (xr->cairo);

  xr->page_number++;
  xr->y = 0;
}

static void
xr_layout_cell (struct xr_driver *, const struct table_cell *,
                int bb[TABLE_N_AXES][2], int clip[TABLE_N_AXES][2],
                PangoWrapMode, int *width, int *height);

static void
dump_line (struct xr_driver *xr, int x0, int y0, int x1, int y1)
{
  cairo_new_path (xr->cairo);
  cairo_move_to (xr->cairo, xr_to_pt (x0), xr_to_pt (y0 + xr->y));
  cairo_line_to (xr->cairo, xr_to_pt (x1), xr_to_pt (y1 + xr->y));
  cairo_stroke (xr->cairo);
}

/* Draws a horizontal line X0...X2 at Y if LEFT says so,
   shortening it to X0...X1 if SHORTEN is true.
   Draws a horizontal line X1...X3 at Y if RIGHT says so,
   shortening it to X2...X3 if SHORTEN is true. */
static void
horz_line (struct xr_driver *xr, int x0, int x1, int x2, int x3, int y,
           enum render_line_style left, enum render_line_style right,
           bool shorten)
{
  if (left != RENDER_LINE_NONE && right != RENDER_LINE_NONE && !shorten)
    dump_line (xr, x0, y, x3, y);
  else
    {
      if (left != RENDER_LINE_NONE)
        dump_line (xr, x0, y, shorten ? x1 : x2, y);
      if (right != RENDER_LINE_NONE)
        dump_line (xr, shorten ? x2 : x1, y, x3, y);
    }
}

/* Draws a vertical line Y0...Y2 at X if TOP says so,
   shortening it to Y0...Y1 if SHORTEN is true.
   Draws a vertical line Y1...Y3 at X if BOTTOM says so,
   shortening it to Y2...Y3 if SHORTEN is true. */
static void
vert_line (struct xr_driver *xr, int y0, int y1, int y2, int y3, int x,
           enum render_line_style top, enum render_line_style bottom,
           bool shorten)
{
  if (top != RENDER_LINE_NONE && bottom != RENDER_LINE_NONE && !shorten)
    dump_line (xr, x, y0, x, y3);
  else
    {
      if (top != RENDER_LINE_NONE)
        dump_line (xr, x, y0, x, shorten ? y1 : y2);
      if (bottom != RENDER_LINE_NONE)
        dump_line (xr, x, shorten ? y2 : y1, x, y3);
    }
}

static void
xr_draw_line (void *xr_, int bb[TABLE_N_AXES][2],
              enum render_line_style styles[TABLE_N_AXES][2])
{
  const int x0 = bb[H][0];
  const int y0 = bb[V][0];
  const int x3 = bb[H][1];
  const int y3 = bb[V][1];
  const int top = styles[H][0];
  const int left = styles[V][0];
  const int bottom = styles[H][1];
  const int right = styles[V][1];

  /* The algorithm here is somewhat subtle, to allow it to handle
     all the kinds of intersections that we need.

     Three additional ordinates are assigned along the x axis.  The
     first is xc, midway between x0 and x3.  The others are x1 and
     x2; for a single vertical line these are equal to xc, and for
     a double vertical line they are the ordinates of the left and
     right half of the double line.

     yc, y1, and y2 are assigned similarly along the y axis.

     The following diagram shows the coordinate system and output
     for double top and bottom lines, single left line, and no
     right line:

                 x0       x1 xc  x2      x3
               y0 ________________________
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
     y1 = y2 = yc |#########     #       |
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
                  |        #     #       |
               y3 |________#_____#_______|
  */
  struct xr_driver *xr = xr_;

  /* Offset from center of each line in a pair of double lines. */
  int double_line_ofs = (xr->line_space + xr->line_width) / 2;

  /* Are the lines along each axis single or double?
     (It doesn't make sense to have different kinds of line on the
     same axis, so we don't try to gracefully handle that case.) */
  bool double_vert = top == RENDER_LINE_DOUBLE || bottom == RENDER_LINE_DOUBLE;
  bool double_horz = left == RENDER_LINE_DOUBLE || right == RENDER_LINE_DOUBLE;

  /* When horizontal lines are doubled,
     the left-side line along y1 normally runs from x0 to x2,
     and the right-side line along y1 from x3 to x1.
     If the top-side line is also doubled, we shorten the y1 lines,
     so that the left-side line runs only to x1,
     and the right-side line only to x2.
     Otherwise, the horizontal line at y = y1 below would cut off
     the intersection, which looks ugly:
               x0       x1     x2      x3
             y0 ________________________
                |        #     #       |
                |        #     #       |
                |        #     #       |
                |        #     #       |
             y1 |#########     ########|
                |                      |
                |                      |
             y2 |######################|
                |                      |
                |                      |
             y3 |______________________|
     It is more of a judgment call when the horizontal line is
     single.  We actually choose to cut off the line anyhow, as
     shown in the first diagram above.
  */
  bool shorten_y1_lines = top == RENDER_LINE_DOUBLE;
  bool shorten_y2_lines = bottom == RENDER_LINE_DOUBLE;
  bool shorten_yc_line = shorten_y1_lines && shorten_y2_lines;
  int horz_line_ofs = double_vert ? double_line_ofs : 0;
  int xc = (x0 + x3) / 2;
  int x1 = xc - horz_line_ofs;
  int x2 = xc + horz_line_ofs;

  bool shorten_x1_lines = left == RENDER_LINE_DOUBLE;
  bool shorten_x2_lines = right == RENDER_LINE_DOUBLE;
  bool shorten_xc_line = shorten_x1_lines && shorten_x2_lines;
  int vert_line_ofs = double_horz ? double_line_ofs : 0;
  int yc = (y0 + y3) / 2;
  int y1 = yc - vert_line_ofs;
  int y2 = yc + vert_line_ofs;

  if (!double_horz)
    horz_line (xr, x0, x1, x2, x3, yc, left, right, shorten_yc_line);
  else
    {
      horz_line (xr, x0, x1, x2, x3, y1, left, right, shorten_y1_lines);
      horz_line (xr, x0, x1, x2, x3, y2, left, right, shorten_y2_lines);
    }

  if (!double_vert)
    vert_line (xr, y0, y1, y2, y3, xc, top, bottom, shorten_xc_line);
  else
    {
      vert_line (xr, y0, y1, y2, y3, x1, top, bottom, shorten_x1_lines);
      vert_line (xr, y0, y1, y2, y3, x2, top, bottom, shorten_x2_lines);
    }
}

static void
xr_measure_cell_width (void *xr_, const struct table_cell *cell,
                       int *min_width, int *max_width)
{
  struct xr_driver *xr = xr_;
  int bb[TABLE_N_AXES][2];
  int clip[TABLE_N_AXES][2];
  int h;

  bb[H][0] = 0;
  bb[H][1] = INT_MAX;
  bb[V][0] = 0;
  bb[V][1] = INT_MAX;
  clip[H][0] = clip[H][1] = clip[V][0] = clip[V][1] = 0;
  xr_layout_cell (xr, cell, bb, clip, PANGO_WRAP_WORD, max_width, &h);

  bb[H][1] = 1;
  xr_layout_cell (xr, cell, bb, clip, PANGO_WRAP_WORD, min_width, &h);
}

static int
xr_measure_cell_height (void *xr_, const struct table_cell *cell, int width)
{
  struct xr_driver *xr = xr_;
  int bb[TABLE_N_AXES][2];
  int clip[TABLE_N_AXES][2];
  int w, h;

  bb[H][0] = 0;
  bb[H][1] = width;
  bb[V][0] = 0;
  bb[V][1] = INT_MAX;
  clip[H][0] = clip[H][1] = clip[V][0] = clip[V][1] = 0;
  xr_layout_cell (xr, cell, bb, clip, PANGO_WRAP_WORD, &w, &h);
  return h;
}

static void
xr_draw_cell (void *xr_, const struct table_cell *cell,
              int bb[TABLE_N_AXES][2], int clip[TABLE_N_AXES][2])
{
  struct xr_driver *xr = xr_;
  int w, h;

  xr_layout_cell (xr, cell, bb, clip, PANGO_WRAP_WORD, &w, &h);
}

/* Writes STRING at location (X,Y) trimmed to the given MAX_WIDTH
   and with the given cell OPTIONS for XR. */
static int
draw_text (struct xr_driver *xr, const char *string, int x, int y,
           int max_width, unsigned int options)
{
  struct table_cell cell;
  int bb[TABLE_N_AXES][2];
  int w, h;

  cell.contents = string;
  cell.options = options;
  bb[H][0] = x;
  bb[V][0] = y;
  bb[H][1] = x + max_width;
  bb[V][1] = xr->font_height;
  xr_layout_cell (xr, &cell, bb, bb, PANGO_WRAP_WORD_CHAR, &w, &h);
  return w;
}

/* Writes LEFT left-justified and RIGHT right-justified within
   (X0...X1) at Y.  LEFT or RIGHT or both may be null. */
static void
draw_header_line (struct xr_driver *xr, const char *left, const char *right,
                  int x0, int x1, int y)
{
  int right_width = 0;
  if (right != NULL)
    right_width = (draw_text (xr, right, x0, y, x1 - x0, TAB_RIGHT)
                   + xr->font_height / 2);
  if (left != NULL)
    draw_text (xr, left, x0, y, x1 - x0 - right_width, TAB_LEFT);
}

/* Draw top of page headers for XR. */
static void
draw_headers (struct xr_driver *xr)
{
  char *r1, *r2;
  int x0, x1;
  int y;

  y = -3 * xr->font_height;
  x0 = xr->font_height / 2;
  x1 = xr->width - xr->font_height / 2;

  /* Draw box. */
  cairo_rectangle (xr->cairo, 0, xr_to_pt (y), xr_to_pt (xr->width),
                   xr_to_pt (2 * (xr->font_height
                                  + xr->line_width + xr->line_gutter)));
  cairo_save (xr->cairo);
  cairo_set_source_rgb (xr->cairo, 0.9, 0.9, 0.9);
  cairo_fill_preserve (xr->cairo);
  cairo_restore (xr->cairo);
  cairo_stroke (xr->cairo);

  y += xr->line_width + xr->line_gutter;

  r1 = xasprintf (_("%s - Page %d"), get_start_date (), xr->page_number);
  r2 = xasprintf ("%s - %s", version, host_system);

  draw_header_line (xr, xr->title, r1, x0, x1, y);
  y += xr->font_height;

  draw_header_line (xr, xr->subtitle, r2, x0, x1, y);

  free (r1);
  free (r2);
}

static void
xr_layout_cell (struct xr_driver *xr, const struct table_cell *cell,
                int bb[TABLE_N_AXES][2], int clip[TABLE_N_AXES][2],
                PangoWrapMode wrap, int *width, int *height)
{
  struct xr_font *font;

  font = (cell->options & TAB_FIX ? &xr->fonts[XR_FONT_FIXED]
          : cell->options & TAB_EMPH ? &xr->fonts[XR_FONT_EMPHASIS]
          : &xr->fonts[XR_FONT_PROPORTIONAL]);

  pango_layout_set_text (font->layout, cell->contents, -1);

  pango_layout_set_alignment (
    font->layout,
    ((cell->options & TAB_ALIGNMENT) == TAB_RIGHT ? PANGO_ALIGN_RIGHT
     : (cell->options & TAB_ALIGNMENT) == TAB_LEFT ? PANGO_ALIGN_LEFT
     : PANGO_ALIGN_CENTER));
  pango_layout_set_width (font->layout,
                          bb[H][1] == INT_MAX ? -1 : bb[H][1] - bb[H][0]);
  pango_layout_set_wrap (font->layout, wrap);

  if (clip[H][0] != clip[H][1])
    {
      cairo_save (xr->cairo);

      if (clip[H][1] != INT_MAX || clip[V][1] != INT_MAX)
        {
          double x0 = xr_to_pt (clip[H][0]);
          double y0 = xr_to_pt (clip[V][0] + xr->y);
          double x1 = xr_to_pt (clip[H][1]);
          double y1 = xr_to_pt (clip[V][1] + xr->y);

          cairo_rectangle (xr->cairo, x0, y0, x1 - x0, y1 - y0);
          cairo_clip (xr->cairo);
        }

      cairo_translate (xr->cairo,
                       xr_to_pt (bb[H][0]),
                       xr_to_pt (bb[V][0] + xr->y));
      pango_cairo_show_layout (xr->cairo, font->layout);
      cairo_restore (xr->cairo);
    }

  if (width != NULL || height != NULL)
    {
      int w, h;

      pango_layout_get_size (font->layout, &w, &h);
      if (width != NULL)
        *width = w;
      if (height != NULL)
        *height = h;
    }
}

/* Attempts to load FONT, initializing its other members based on
   its 'string' member and the information in DRIVER.  Returns true
   if successful, otherwise false. */
static bool
load_font (struct xr_driver *xr, struct xr_font *font)
{
  PangoContext *context;
  PangoLanguage *language;

  font->desc = pango_font_description_from_string (font->string);
  if (font->desc == NULL)
    {
      error (0, 0, _("\"%s\": bad font specification"), font->string);
      return false;
    }
  pango_font_description_set_absolute_size (font->desc, xr->font_height);

  font->layout = pango_cairo_create_layout (xr->cairo);
  pango_layout_set_font_description (font->layout, font->desc);

  language = pango_language_get_default ();
  context = pango_layout_get_context (font->layout);
  font->metrics = pango_context_get_metrics (context, font->desc, language);

  return true;
}

/* Frees FONT. */
static void
free_font (struct xr_font *font)
{
  free (font->string);
  if (font->desc != NULL)
    pango_font_description_free (font->desc);
  pango_font_metrics_unref (font->metrics);
  g_object_unref (font->layout);
}

/* Cairo driver class. */
const struct output_driver_class cairo_class =
{
  "cairo",
  xr_create,
  xr_destroy,
  xr_submit,
  xr_flush,
};

/* GUI rendering helpers. */

struct xr_rendering
  {
    /* Table items. */
    struct render_page *page;
    struct xr_driver *xr;
    int title_height;

    /* Chart items. */
    struct chart_item *chart;
  };

#define CHART_WIDTH 500
#define CHART_HEIGHT 375

struct xr_driver *
xr_create_driver (cairo_t *cairo)
{
  struct xr_driver *xr;
  struct string_map map;

  string_map_init (&map);
  xr = xr_allocate ("cairo", 0, &map);
  string_map_destroy (&map);

  xr->width = INT_MAX / 8;
  xr->length = INT_MAX / 8;
  if (!xr_set_cairo (xr, cairo))
    {
      output_driver_destroy (&xr->driver);
      return NULL;
    }
  return xr;
}

struct xr_rendering *
xr_rendering_create (struct xr_driver *xr, const struct output_item *item,
                     cairo_t *cr)
{
  struct xr_rendering *r = NULL;

  if (is_text_item (item))
    {
      const struct text_item *text_item = to_text_item (item);
      const char *text = text_item_get_text (text_item);
      struct table_item *table_item;

      table_item = table_item_create (table_from_string (0, text), NULL);
      r = xr_rendering_create (xr, &table_item->output_item, cr);
      table_item_unref (table_item);
    }
  else if (is_table_item (item))
    {
      r = xzalloc (sizeof *r);
      r->xr = xr;
      xr_set_cairo (xr, cr);
      r->page = xr_render_table_item (xr, to_table_item (item),
                                      &r->title_height);
    }
  else if (is_chart_item (item))
    {
      r = xzalloc (sizeof *r);
      r->chart = to_chart_item (output_item_ref (item));
    }

  return r;
}

void
xr_rendering_measure (struct xr_rendering *r, int *w, int *h)
{
  if (r->chart == NULL)
    {
      *w = render_page_get_size (r->page, H) / 1024;
      *h = (render_page_get_size (r->page, V) + r->title_height) / 1024;
    }
  else
    {
      *w = CHART_WIDTH;
      *h = CHART_HEIGHT;
    }
}

void
xr_rendering_draw (struct xr_rendering *r, cairo_t *cr)
{
  if (r->chart == NULL)
    {
      struct xr_driver *xr = r->xr;

      xr_set_cairo (xr, cr);
      xr->y = 0;
      render_page_draw (r->page);
    }
  else
    xr_draw_chart (r->chart, cr, 0, 0, CHART_WIDTH, CHART_HEIGHT);
}

void
xr_draw_chart (const struct chart_item *chart_item, cairo_t *cr,
               double x, double y, double width, double height)
{
  struct xrchart_geometry geom;

  cairo_save (cr);
  cairo_translate (cr, x, y + height);
  cairo_scale (cr, 1.0, -1.0);
  xrchart_geometry_init (cr, &geom, width, height);
  if (is_boxplot (chart_item))
    xrchart_draw_boxplot (chart_item, cr, &geom);
  else if (is_histogram_chart (chart_item))
    xrchart_draw_histogram (chart_item, cr, &geom);
  else if (is_np_plot_chart (chart_item))
    xrchart_draw_np_plot (chart_item, cr, &geom);
  else if (is_piechart (chart_item))
    xrchart_draw_piechart (chart_item, cr, &geom);
  else if (is_roc_chart (chart_item))
    xrchart_draw_roc (chart_item, cr, &geom);
  else if (is_scree (chart_item))
    xrchart_draw_scree (chart_item, cr, &geom);
  else
    NOT_REACHED ();
  xrchart_geometry_free (cr, &geom);

  cairo_restore (cr);
}

char *
xr_draw_png_chart (const struct chart_item *item,
                   const char *file_name_template, int number)
{
  const int width = 640;
  const int length = 480;

  cairo_surface_t *surface;
  cairo_status_t status;
  const char *number_pos;
  char *file_name;
  cairo_t *cr;

  number_pos = strchr (file_name_template, '#');
  if (number_pos != NULL)
    file_name = xasprintf ("%.*s%d%s", (int) (number_pos - file_name_template),
                           file_name_template, number, number_pos + 1);
  else
    file_name = xstrdup (file_name_template);

  surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24, width, length);
  cr = cairo_create (surface);

  cairo_save (cr);
  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
  cairo_rectangle (cr, 0, 0, width, length);
  cairo_fill (cr);
  cairo_restore (cr);

  cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);

  xr_draw_chart (item, cr, 0.0, 0.0, width, length);

  status = cairo_surface_write_to_png (surface, file_name);
  if (status != CAIRO_STATUS_SUCCESS)
    error (0, 0, _("writing output file \"%s\": %s"),
           file_name, cairo_status_to_string (status));

  cairo_destroy (cr);
  cairo_surface_destroy (surface);

  return file_name;
}