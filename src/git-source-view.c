#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtkwidget.h>
#include <string.h>
#include <ctype.h>

#include "git-source-view.h"
#include "git-annotated-source.h"
#include "git-marshal.h"

static void git_source_view_dispose (GObject *object);
static void git_source_view_realize (GtkWidget *widget);
static gboolean git_source_view_expose_event (GtkWidget *widget,
					      GdkEventExpose *event);
static void git_source_view_set_scroll_adjustments (GtkWidget *widget,
						    GtkAdjustment *hadjustment,
						    GtkAdjustment *vadjustment);
static void git_source_view_size_allocate (GtkWidget *widget,
					   GtkAllocation *allocation);

G_DEFINE_TYPE (GitSourceView, git_source_view, GTK_TYPE_WIDGET);

#define GIT_SOURCE_VIEW_GET_PRIVATE(obj) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GIT_TYPE_SOURCE_VIEW, \
				GitSourceViewPrivate))

/* Number of digits of the commit hash to show */
#define GIT_SOURCE_VIEW_COMMIT_HASH_LENGTH 6

#define GIT_SOURCE_VIEW_GAP 3

struct _GitSourceViewPrivate
{
  GitAnnotatedSource *paint_source, *load_source;
  guint loading_completed_handler;

  guint line_height, max_line_width, max_hash_length;

  GtkAdjustment *hadjustment, *vadjustment;
  guint hadjustment_value_changed_handler;
  guint vadjustment_value_changed_handler;
  guint hadjustment_changed_handler;
  guint vadjustment_changed_handler;
  
  gint x_offset, y_offset;
};

enum
  {
    SET_SCROLL_ADJUSTMENTS,

    LAST_SIGNAL
  };

static guint client_signals[LAST_SIGNAL];

static void
git_source_view_class_init (GitSourceViewClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GtkWidgetClass *widget_class = (GtkWidgetClass *) klass;

  gobject_class->dispose = git_source_view_dispose;

  widget_class->realize = git_source_view_realize;
  widget_class->expose_event = git_source_view_expose_event;
  widget_class->size_allocate = git_source_view_size_allocate;

  klass->set_scroll_adjustments = git_source_view_set_scroll_adjustments;

  client_signals[SET_SCROLL_ADJUSTMENTS]
    = g_signal_new ("set_scroll_adjustments",
		    G_OBJECT_CLASS_TYPE (gobject_class),
		    G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		    G_STRUCT_OFFSET (GitSourceViewClass,
				     set_scroll_adjustments),
		    NULL, NULL,
		    _git_marshal_VOID__OBJECT_OBJECT,
		    G_TYPE_NONE, 2,
		    GTK_TYPE_ADJUSTMENT,
		    GTK_TYPE_ADJUSTMENT);
  widget_class->set_scroll_adjustments_signal
    = client_signals[SET_SCROLL_ADJUSTMENTS];

  g_type_class_add_private (klass, sizeof (GitSourceViewPrivate));
}

static void
git_source_view_init (GitSourceView *self)
{
  self->priv = GIT_SOURCE_VIEW_GET_PRIVATE (self);
}

static void
git_source_view_unref_loading_source (GitSourceView *sview)
{
  GitSourceViewPrivate *priv = sview->priv;

  if (priv->load_source)
    {
      g_signal_handler_disconnect (priv->load_source,
				   priv->loading_completed_handler);
      g_object_unref (priv->load_source);
      priv->load_source = NULL;
    }
}

static void
git_source_view_unref_hadjustment (GitSourceView *sview)
{
  GitSourceViewPrivate *priv = sview->priv;

  if (priv->hadjustment)
    {
      g_signal_handler_disconnect (priv->hadjustment,
				   priv->hadjustment_value_changed_handler);
      g_signal_handler_disconnect (priv->hadjustment,
				   priv->hadjustment_changed_handler);
      g_object_unref (priv->hadjustment);
      priv->hadjustment = NULL;
    }
}

static void
git_source_view_unref_vadjustment (GitSourceView *sview)
{
  GitSourceViewPrivate *priv = sview->priv;

  if (priv->vadjustment)
    {
      g_signal_handler_disconnect (priv->vadjustment,
				   priv->vadjustment_value_changed_handler);
      g_signal_handler_disconnect (priv->vadjustment,
				   priv->vadjustment_changed_handler);
      g_object_unref (priv->vadjustment);
      priv->vadjustment = NULL;
    }
}

static void
git_source_view_dispose (GObject *object)
{
  GitSourceView *self = (GitSourceView *) object;
  GitSourceViewPrivate *priv = self->priv;
  
  if (priv->paint_source)
    {
      g_object_unref (priv->paint_source);
      priv->paint_source = NULL;
    }

  git_source_view_unref_hadjustment (self);
  git_source_view_unref_vadjustment (self);

  git_source_view_unref_loading_source (self);

  G_OBJECT_CLASS (git_source_view_parent_class)->dispose (object);
}

static void
git_source_view_set_text_for_line (PangoLayout *layout,
				   const GitAnnotatedSourceLine *line)
{
  int len = strlen (line->text);

  /* Remove any trailing spaces in the text */
  while (len > 0 && isspace (line->text[len - 1]))
    len--;

  pango_layout_set_text (layout, line->text, len);
}

static void
git_source_view_set_text_for_commit (PangoLayout *layout, GitCommit *commit)
{
  const gchar *hash = git_commit_get_hash (commit);
  int len = strlen (hash);

  if (len > GIT_SOURCE_VIEW_COMMIT_HASH_LENGTH)
    len = GIT_SOURCE_VIEW_COMMIT_HASH_LENGTH;

  pango_layout_set_text (layout, hash, len);
}

static void
git_source_view_update_scroll_adjustments (GitSourceView *sview)
{
  GitSourceViewPrivate *priv = sview->priv;
  GtkWidget *widget = (GtkWidget *) sview;
  gsize n_lines = 0;

  if (priv->paint_source)
    n_lines = git_annotated_source_get_n_lines (priv->paint_source);

  if (priv->hadjustment)
    {
      priv->hadjustment->lower = 0.0;
      priv->hadjustment->upper = priv->max_line_width;
      priv->hadjustment->step_increment = 10.0;
      priv->hadjustment->page_increment = widget->allocation.width;
      priv->hadjustment->page_size = widget->allocation.width
	- priv->max_hash_length - GIT_SOURCE_VIEW_GAP;

      if (priv->hadjustment->value + priv->hadjustment->page_size
	  > priv->hadjustment->upper)
	priv->hadjustment->value
	  = priv->hadjustment->upper - priv->hadjustment->page_size;
      if (priv->hadjustment->value < 0.0)
	priv->hadjustment->value = 0.0;

      gtk_adjustment_changed (priv->hadjustment);
    }
  if (priv->vadjustment)
    {
      priv->vadjustment->lower = 0.0;
      priv->vadjustment->upper = priv->line_height * n_lines;
      priv->vadjustment->step_increment = priv->line_height;
      priv->vadjustment->page_increment = widget->allocation.height;
      priv->vadjustment->page_size = widget->allocation.height;

      if (priv->vadjustment->value + priv->vadjustment->page_size
	  > priv->vadjustment->upper)
	priv->vadjustment->value
	  = priv->vadjustment->upper - priv->vadjustment->page_size;
      if (priv->vadjustment->value < 0.0)
	priv->vadjustment->value = 0.0;

      gtk_adjustment_changed (priv->vadjustment);
    }
}

static void
git_source_view_calculate_line_height (GitSourceView *sview)
{
  GitSourceViewPrivate *priv = sview->priv;

  if (priv->line_height == 0 && GTK_WIDGET_REALIZED (sview)
      && priv->paint_source)
    {
      PangoLayout *layout = gtk_widget_create_pango_layout (GTK_WIDGET (sview),
							    NULL);
      int line_num;
      PangoRectangle logical_rect;
      guint line_height = 1, max_line_width = 1, max_hash_length = 1;

      for (line_num = 0;
	   line_num < git_annotated_source_get_n_lines (priv->paint_source);
	   line_num++)
	{
	  const GitAnnotatedSourceLine *line
	    = git_annotated_source_get_line (priv->paint_source, line_num);

	  git_source_view_set_text_for_line (layout, line);
	  pango_layout_get_pixel_extents (layout, NULL, &logical_rect);
	  
	  if (logical_rect.height > line_height)
	    line_height = logical_rect.height;
	  if (logical_rect.width > max_line_width)
	    max_line_width = logical_rect.width;

	  git_source_view_set_text_for_commit (layout, line->commit);
	  pango_layout_get_pixel_extents (layout, NULL, &logical_rect);
	  
	  if (logical_rect.height > line_height)
	    line_height = logical_rect.height;
	  if (logical_rect.width > max_hash_length)
	    max_hash_length = logical_rect.width;
	}

      priv->line_height = line_height;
      priv->max_line_width = max_line_width;
      priv->max_hash_length = max_hash_length + GIT_SOURCE_VIEW_GAP * 2;

      g_object_unref (layout);

      git_source_view_update_scroll_adjustments (sview);
    }
}

static void
git_source_view_realize (GtkWidget *widget)
{
  GdkWindowAttr attribs;

  GTK_WIDGET_SET_FLAGS (widget, GTK_REALIZED);

  memset (&attribs, 0, sizeof (attribs));
  attribs.x = widget->allocation.x;
  attribs.y = widget->allocation.y;
  attribs.width = widget->allocation.width;
  attribs.height = widget->allocation.height;
  attribs.wclass = GDK_INPUT_OUTPUT;
  attribs.window_type = GDK_WINDOW_CHILD;
  attribs.visual = gtk_widget_get_visual (widget);
  attribs.colormap = gtk_widget_get_colormap (widget);
  attribs.event_mask = gtk_widget_get_events (widget)
    | GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK
    | GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK
    | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK;

  widget->window = gdk_window_new (gtk_widget_get_parent_window (widget),
				   &attribs,
				   GDK_WA_X | GDK_WA_Y
				   | GDK_WA_VISUAL | GDK_WA_COLORMAP);

  gdk_window_set_user_data (widget->window, widget);
  widget->style = gtk_style_attach (widget->style, widget->window);

  gdk_window_set_background (widget->window,
			     &widget->style->base[GTK_WIDGET_STATE (widget)]);

  git_source_view_calculate_line_height (GIT_SOURCE_VIEW (widget));
}

static gboolean
git_source_view_expose_event (GtkWidget *widget,
			      GdkEventExpose *event)
{
  GitSourceView *sview = (GitSourceView *) widget;
  GitSourceViewPrivate *priv = sview->priv;
  gsize line_start, line_end, line_num, n_lines;
  gint y;
  PangoLayout *layout;
  cairo_t *cr;

  if (priv->paint_source && priv->line_height)
    {
      layout = gtk_widget_create_pango_layout (widget, NULL);
      cr = gdk_cairo_create (widget->window);

      n_lines = git_annotated_source_get_n_lines (priv->paint_source);
      line_start = (event->area.y + priv->y_offset) / priv->line_height;
      line_end = (event->area.y + priv->y_offset
		  + event->area.height + priv->line_height - 1)
	/ priv->line_height;

      if (line_end > n_lines)
	line_end = n_lines;
      if (line_start > line_end)
	line_start = line_end;

      for (line_num = line_start; line_num < line_end; line_num++)
	{
	  GdkRectangle clip_rect;
	  const GitAnnotatedSourceLine *line
	    = git_annotated_source_get_line (priv->paint_source, line_num);
	  GdkColor color;
	  y = line_num * priv->line_height - priv->y_offset;

	  git_source_view_set_text_for_commit (layout, line->commit);
	  git_commit_get_color (line->commit, &color);

	  cairo_set_source_rgb (cr, color.red / 65535.0, color.green / 65535.0,
				color.blue / 65535.0);
	  cairo_rectangle (cr, 0, y, priv->max_hash_length, priv->line_height);
	  cairo_fill_preserve (cr);

	  /* Invert the color so that the text is guaranteed to be a
	     different (albeit clashing) colour */
	  color.red = ~color.red;
	  color.green = ~color.green;
	  color.blue = ~color.blue;
	  cairo_set_source_rgb (cr, color.red / 65535.0, color.green / 65535.0,
				color.blue / 65535.0);
	  cairo_save (cr);
	  cairo_clip (cr);
	  cairo_move_to (cr, 0, y);
	  pango_cairo_show_layout (cr, layout);
	  cairo_restore (cr);

	  git_source_view_set_text_for_line (layout, line);

	  clip_rect.x = priv->max_hash_length + GIT_SOURCE_VIEW_GAP;
	  clip_rect.width = widget->allocation.width;
	  clip_rect.y = y;
	  clip_rect.height = priv->line_height;

	  gtk_paint_layout (widget->style,
			    widget->window,
			    GTK_WIDGET_STATE (widget),
			    TRUE,
			    &clip_rect,
			    widget,
			    NULL,
			    -priv->x_offset + priv->max_hash_length
			    + GIT_SOURCE_VIEW_GAP,
			    y,
			    layout);
	}

      cairo_destroy (cr);
      g_object_unref (layout);
    }

  return FALSE;
}

static void
git_source_view_on_adj_changed (GtkAdjustment *adj, GitSourceView *sview)
{
  GitSourceViewPrivate *priv = sview->priv;

  if (priv->hadjustment)
    priv->x_offset = priv->hadjustment->value;
  if (priv->vadjustment)
    priv->y_offset = priv->vadjustment->value;

  if (GTK_WIDGET_REALIZED (GTK_WIDGET (sview)))
    gdk_window_invalidate_rect (GTK_WIDGET (sview)->window, NULL, FALSE);
}

static void
git_source_view_on_adj_value_changed (GtkAdjustment *adj, GitSourceView *sview)
{
  GitSourceViewPrivate *priv = sview->priv;
  gint dx = 0, dy = 0;

  if (priv->hadjustment)
    {
      gint new_offset = (gint) priv->hadjustment->value;
      dx = priv->x_offset - new_offset;
      priv->x_offset = new_offset;
    }
  if (priv->vadjustment)
    {
      gint new_offset = (gint) priv->vadjustment->value;
      dy = priv->y_offset - new_offset;
      priv->y_offset = new_offset;
    }

  if (GTK_WIDGET_REALIZED (GTK_WIDGET (sview)))
    {
      /* If dx has changed then we have to redraw the whole window
	 because the commit hashes on the left side shouldn't
	 scroll */
      if (dx)
	gdk_window_invalidate_rect (GTK_WIDGET (sview)->window, NULL, FALSE);
      else if (dy)
	gdk_window_scroll (GTK_WIDGET (sview)->window, dx, dy);
    }
}

static void
git_source_view_set_scroll_adjustments (GtkWidget *widget,
					GtkAdjustment *hadjustment,
					GtkAdjustment *vadjustment)
{
  GitSourceView *sview = (GitSourceView *) widget;
  GitSourceViewPrivate *priv = sview->priv;

  if (hadjustment)
    g_return_if_fail (GTK_IS_ADJUSTMENT (hadjustment));
  else
    hadjustment
      = GTK_ADJUSTMENT (gtk_adjustment_new (0.0, 0.0, 0.0, 0.0, 0.0, 0.0));
  if (vadjustment)
    g_return_if_fail (GTK_IS_ADJUSTMENT (vadjustment));
  else
    vadjustment
      = GTK_ADJUSTMENT (gtk_adjustment_new (0.0, 0.0, 0.0, 0.0, 0.0, 0.0));  

  g_object_ref_sink (hadjustment);
  g_object_ref_sink (vadjustment);

  git_source_view_unref_hadjustment (sview);
  priv->hadjustment = hadjustment;
  priv->hadjustment_changed_handler
    = g_signal_connect (hadjustment, "changed",
			G_CALLBACK (git_source_view_on_adj_changed),
			sview);
  priv->hadjustment_value_changed_handler
    = g_signal_connect (hadjustment, "value-changed",
			G_CALLBACK (git_source_view_on_adj_value_changed),
			sview);

  git_source_view_unref_vadjustment (sview);
  priv->vadjustment = vadjustment;
  priv->vadjustment_changed_handler
    = g_signal_connect (vadjustment, "changed",
			G_CALLBACK (git_source_view_on_adj_changed),
			sview);
  priv->vadjustment_value_changed_handler
    = g_signal_connect (vadjustment, "value-changed",
			G_CALLBACK (git_source_view_on_adj_value_changed),
			sview);


  git_source_view_update_scroll_adjustments (sview);
}

static void
git_source_view_size_allocate (GtkWidget *widget,
			       GtkAllocation *allocation)
{
  /* Chain up */
  GTK_WIDGET_CLASS (git_source_view_parent_class)
    ->size_allocate (widget, allocation);

  git_source_view_update_scroll_adjustments (GIT_SOURCE_VIEW (widget));
}

GtkWidget *
git_source_view_new (void)
{
  GtkWidget *widget = g_object_new (GIT_TYPE_SOURCE_VIEW, NULL);

  return widget;
}

static void
git_source_view_show_error (GitSourceView *sview, const GError *error)
{
  /* STUB: should show a dialog box */
  g_warning ("error: %s\n", error->message);
}

static void
git_source_view_on_completed (GitAnnotatedSource *source,
			      const GError *error,
			      GitSourceView *sview)
{
  GitSourceViewPrivate *priv = sview->priv;

  if (error)
    git_source_view_show_error (sview, error);
  else
    {
      /* Forget the old painting source */
      if (priv->paint_source)
	g_object_unref (priv->paint_source);
      /* Use the loading source to paint with */
      priv->paint_source = g_object_ref (source);

      /* Recalculate the line height */
      priv->line_height = 0;
      git_source_view_calculate_line_height (sview);

      gdk_window_invalidate_rect (GTK_WIDGET (sview)->window, NULL, FALSE);
      git_source_view_update_scroll_adjustments (sview);
    }

  git_source_view_unref_loading_source (sview);
}

void
git_source_view_set_file (GitSourceView *sview,
			  const gchar *filename,
			  const gchar *revision)
{
  GitSourceViewPrivate *priv;
  GError *error = NULL;

  g_return_if_fail (GIT_IS_SOURCE_VIEW (sview));

  priv = sview->priv;

  /* If we're currently trying to load some source then cancel it */
  git_source_view_unref_loading_source (sview);

  priv->load_source = git_annotated_source_new ();
  priv->loading_completed_handler
    = g_signal_connect (priv->load_source, "completed",
			G_CALLBACK (git_source_view_on_completed), sview);

  if (!git_annotated_source_fetch (priv->load_source,
				   filename, revision,
				   &error))
    {
      git_source_view_show_error (sview, error);
      git_source_view_unref_loading_source (sview);
      
      g_error_free (error);
    }
}