/* PSPPIRE - a graphical user interface for PSPP.
   Copyright (C) 2008, 2009  Free Software Foundation

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

#include <gtk/gtksignal.h>
#include <gtk/gtkbox.h>
#include "helper.h"

#include <libpspp/cast.h>
#include <libpspp/message.h>
#include <output/cairo.h>
#include <output/driver-provider.h>
#include <output/tab.h>
#include <stdlib.h>

#include "about.h"

#include "psppire-output-window.h"


#include "xalloc.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gettext.h>
#define _(msgid) gettext (msgid)
#define N_(msgid) msgid

enum
  {
    COL_TITLE,                  /* Table title. */
    COL_Y,                      /* Y position of top of title. */
    N_COLS
  };

static void psppire_output_window_base_finalize (PsppireOutputWindowClass *, gpointer);
static void psppire_output_window_base_init     (PsppireOutputWindowClass *class);
static void psppire_output_window_class_init    (PsppireOutputWindowClass *class);
static void psppire_output_window_init          (PsppireOutputWindow      *window);


GType
psppire_output_window_get_type (void)
{
  static GType psppire_output_window_type = 0;

  if (!psppire_output_window_type)
    {
      static const GTypeInfo psppire_output_window_info =
      {
	sizeof (PsppireOutputWindowClass),
	(GBaseInitFunc) psppire_output_window_base_init,
        (GBaseFinalizeFunc) psppire_output_window_base_finalize,
	(GClassInitFunc)psppire_output_window_class_init,
	(GClassFinalizeFunc) NULL,
	NULL,
        sizeof (PsppireOutputWindow),
	0,
	(GInstanceInitFunc) psppire_output_window_init,
      };

      psppire_output_window_type =
	g_type_register_static (PSPPIRE_TYPE_WINDOW, "PsppireOutputWindow",
				&psppire_output_window_info, 0);
    }

  return psppire_output_window_type;
}

static GObjectClass *parent_class;

static void
psppire_output_window_finalize (GObject *object)
{
  if (G_OBJECT_CLASS (parent_class)->finalize)
    (*G_OBJECT_CLASS (parent_class)->finalize) (object);
}


static void
psppire_output_window_class_init (PsppireOutputWindowClass *class)
{
  parent_class = g_type_class_peek_parent (class);
}


static void
psppire_output_window_base_init (PsppireOutputWindowClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = psppire_output_window_finalize;
}



static void
psppire_output_window_base_finalize (PsppireOutputWindowClass *class,
				     gpointer class_data)
{
}

/* Output driver class. */

struct psppire_output_driver
  {
    struct output_driver driver;
    PsppireOutputWindow *viewer;
    struct xr_driver *xr;
  };

static struct output_driver_class psppire_output_class;

static struct psppire_output_driver *
psppire_output_cast (struct output_driver *driver)
{
  assert (driver->class == &psppire_output_class);
  return UP_CAST (driver, struct psppire_output_driver, driver);
}

static gboolean
expose_event_callback (GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
  struct xr_rendering *r = g_object_get_data (G_OBJECT (widget), "rendering");
  cairo_t *cr;

  cr = gdk_cairo_create (widget->window);
  xr_rendering_draw (r, cr);
  cairo_destroy (cr);

  return TRUE;
}

static void
psppire_output_submit (struct output_driver *this,
                       const struct output_item *item)
{
  struct psppire_output_driver *pod = psppire_output_cast (this);
  GtkWidget *drawing_area;
  struct xr_rendering *r;
  cairo_t *cr;
  int tw, th;

  if (pod->viewer == NULL)
    {
      pod->viewer = PSPPIRE_OUTPUT_WINDOW (psppire_output_window_new ());
      gtk_widget_show_all (GTK_WIDGET (pod->viewer));
      pod->viewer->driver = pod;
    }

  cr = gdk_cairo_create (GTK_WIDGET (pod->viewer)->window);
  if (pod->xr == NULL)
    pod->xr = xr_create_driver (cr);

  r = xr_rendering_create (pod->xr, item, cr);
  if (r == NULL)
    goto done;

  xr_rendering_measure (r, &tw, &th);

  drawing_area = gtk_drawing_area_new ();
  gtk_widget_modify_bg (
    GTK_WIDGET (drawing_area), GTK_STATE_NORMAL,
    &gtk_widget_get_style (drawing_area)->base[GTK_STATE_NORMAL]);
  g_object_set_data (G_OBJECT (drawing_area), "rendering", r);
  gtk_widget_set_size_request (drawing_area, tw, th);
  gtk_layout_put (pod->viewer->output, drawing_area, 0, pod->viewer->y);
  gtk_widget_show (drawing_area);
  g_signal_connect (G_OBJECT (drawing_area), "expose_event",
                     G_CALLBACK (expose_event_callback), NULL);

  if (pod->viewer->max_width < tw)
    pod->viewer->max_width = tw;
  pod->viewer->y += th;

  gtk_layout_set_size (pod->viewer->output,
                       pod->viewer->max_width, pod->viewer->y);

  gtk_window_set_urgency_hint (GTK_WINDOW (pod->viewer), TRUE);

done:
  cairo_destroy (cr);
}

static struct output_driver_class psppire_output_class =
  {
    "PSPPIRE",                  /* name */
    NULL,                       /* create */
    NULL,                       /* destroy */
    psppire_output_submit,      /* submit */
    NULL,                       /* flush */
  };

void
psppire_output_window_setup (void)
{
  struct psppire_output_driver *pod;
  struct output_driver *d;

  pod = xzalloc (sizeof *pod);
  d = &pod->driver;
  output_driver_init (d, &psppire_output_class, "PSPPIRE", 0);
  output_driver_register (d);
}

int viewer_length = 16;
int viewer_width = 59;

/* Callback for the "delete" action (clicking the x on the top right
   hand corner of the window) */
static gboolean
on_delete (GtkWidget *w, GdkEvent *event, gpointer user_data)
{
  PsppireOutputWindow *ow = PSPPIRE_OUTPUT_WINDOW (user_data);

  gtk_widget_destroy (GTK_WIDGET (ow));

  ow->driver->viewer = NULL;

  return FALSE;
}



static void
cancel_urgency (GtkWindow *window,  gpointer data)
{
  gtk_window_set_urgency_hint (window, FALSE);
}

static void
on_row_activate (GtkTreeView *overview,
                 GtkTreePath *path,
                 GtkTreeViewColumn *column,
                 PsppireOutputWindow *window)
{
  GtkTreeModel *model;
  GtkTreeIter iter;
  GtkAdjustment *vadj;
  GValue value = {0};
  double y, min, max;

  model = gtk_tree_view_get_model (overview);
  if (!gtk_tree_model_get_iter (model, &iter, path))
    return;

  gtk_tree_model_get_value (model, &iter, COL_Y, &value);
  y = g_value_get_long (&value);
  g_value_unset (&value);

  vadj = gtk_layout_get_vadjustment (window->output);
  min = vadj->lower;
  max = vadj->upper - vadj->page_size;
  if (y < min)
    y = min;
  else if (y > max)
    y = max;
  gtk_adjustment_set_value (vadj, y);
}

static void
psppire_output_window_init (PsppireOutputWindow *window)
{
  GtkTreeViewColumn *column;
  GtkCellRenderer *renderer;
  GtkBuilder *xml;

  xml = builder_new ("output-viewer.ui");

  gtk_widget_reparent (get_widget_assert (xml, "vbox1"), GTK_WIDGET (window));

  window->output = GTK_LAYOUT (get_widget_assert (xml, "output"));
  window->y = 0;

  window->overview = GTK_TREE_VIEW (get_widget_assert (xml, "overview"));
  gtk_tree_view_set_model (window->overview,
                           GTK_TREE_MODEL (gtk_tree_store_new (
                                             N_COLS,
                                             G_TYPE_STRING, /* COL_TITLE */
                                             G_TYPE_LONG))); /* COL_Y */
  window->last_table_num = -1;

  column = gtk_tree_view_column_new ();
  gtk_tree_view_append_column (GTK_TREE_VIEW (window->overview), column);
  renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_add_attribute (column, renderer, "text", COL_TITLE);

  g_signal_connect (GTK_TREE_VIEW (window->overview),
                    "row-activated", G_CALLBACK (on_row_activate), window);

  gtk_widget_modify_bg (GTK_WIDGET (window->output), GTK_STATE_NORMAL,
                        &gtk_widget_get_style (GTK_WIDGET (window->output))->base[GTK_STATE_NORMAL]);

  connect_help (xml);

  g_signal_connect (window,
		    "focus-in-event",
		    G_CALLBACK (cancel_urgency),
		    NULL);

  g_signal_connect (get_action_assert (xml,"help_about"),
		    "activate",
		    G_CALLBACK (about_new),
		    window);

  g_signal_connect (get_action_assert (xml,"help_reference"),
		    "activate",
		    G_CALLBACK (reference_manual),
		    NULL);

  g_signal_connect (get_action_assert (xml,"windows_minimise-all"),
		    "activate",
		    G_CALLBACK (psppire_window_minimise_all),
		    NULL);

  {
    GtkUIManager *uim = GTK_UI_MANAGER (get_object_assert (xml, "uimanager1", GTK_TYPE_UI_MANAGER));

    PSPPIRE_WINDOW (window)->menu =
      GTK_MENU_SHELL (gtk_ui_manager_get_widget (uim,"/ui/menubar1/windows_menuitem/windows_minimise-all")->parent);
  }

  g_object_unref (xml);

  g_signal_connect (window, "delete-event",
		    G_CALLBACK (on_delete), window);
}


GtkWidget*
psppire_output_window_new (void)
{
  return GTK_WIDGET (g_object_new (psppire_output_window_get_type (),
				   "filename", "Output",
				   "description", _("Output Viewer"),
				   NULL));
}