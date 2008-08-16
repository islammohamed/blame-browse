/* This file is part of blame-browse.
 * Copyright (C) 2008  Neil Roberts  <bpeeluk@yahoo.co.uk>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtkdialog.h>
#include <gtk/gtklabel.h>
#include <gtk/gtkbox.h>
#include <gtk/gtktable.h>
#include <gtk/gtktextview.h>
#include <gtk/gtkscrolledwindow.h>

#include "git-commit-dialog.h"
#include "intl.h"

static void git_commit_dialog_dispose (GObject *object);

static void git_commit_dialog_set_property (GObject *object,
					    guint property_id,
					    const GValue *value,
					    GParamSpec *pspec);
static void git_commit_dialog_get_property (GObject *object,
					    guint property_id,
					    GValue *value,
					    GParamSpec *pspec);

static void git_commit_dialog_update (GitCommitDialog *cdiag);

G_DEFINE_TYPE (GitCommitDialog, git_commit_dialog, GTK_TYPE_DIALOG);

#define GIT_COMMIT_DIALOG_GET_PRIVATE(obj) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GIT_TYPE_COMMIT_DIALOG, \
				GitCommitDialogPrivate))

struct _GitCommitDialogPrivate
{
  GitCommit *commit;
  guint has_log_data_handler;

  GtkWidget *table, *log_view;
};

enum
  {
    PROP_0,

    PROP_COMMIT
  };

static void
git_commit_dialog_class_init (GitCommitDialogClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GParamSpec *pspec;

  gobject_class->dispose = git_commit_dialog_dispose;
  gobject_class->set_property = git_commit_dialog_set_property;
  gobject_class->get_property = git_commit_dialog_get_property;

  pspec = g_param_spec_object ("commit",
			       "Commit",
			       "The commit object to display data for",
			       GIT_TYPE_COMMIT,
			       G_PARAM_READABLE | G_PARAM_WRITABLE);
  g_object_class_install_property (gobject_class, PROP_COMMIT, pspec);

  g_type_class_add_private (klass, sizeof (GitCommitDialogPrivate));
}

static void
git_commit_dialog_init (GitCommitDialog *self)
{
  GitCommitDialogPrivate *priv;
  GtkWidget *scrolled_window;

  priv = self->priv = GIT_COMMIT_DIALOG_GET_PRIVATE (self);

  priv->table = g_object_ref_sink (gtk_table_new (0, 2, FALSE));
  gtk_widget_show (priv->table);

  gtk_box_pack_start (GTK_BOX (GTK_DIALOG (self)->vbox), priv->table,
		      FALSE, FALSE, 0);

  scrolled_window = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window),
				  GTK_POLICY_AUTOMATIC,
				  GTK_POLICY_AUTOMATIC);

  priv->log_view = g_object_ref_sink (gtk_text_view_new ());
  g_object_set (priv->log_view,
		"editable", FALSE,
		"cursor-visible", FALSE,
		NULL);
  gtk_widget_show (priv->log_view);
  gtk_container_add (GTK_CONTAINER (scrolled_window), priv->log_view);
  
  gtk_widget_show (scrolled_window);
  gtk_box_pack_start (GTK_BOX (GTK_DIALOG (self)->vbox), scrolled_window,
		      TRUE, TRUE, 0);
}

static void
git_commit_dialog_unref_commit (GitCommitDialog *cdiag)
{
  GitCommitDialogPrivate *priv = cdiag->priv;

  if (priv->commit)
    {
      g_signal_handler_disconnect (priv->commit,
				   priv->has_log_data_handler);
      g_object_unref (priv->commit);
      priv->commit = NULL;
    }
}

static void
git_commit_dialog_dispose (GObject *object)
{
  GitCommitDialog *self = (GitCommitDialog *) object;
  GitCommitDialogPrivate *priv = self->priv;

  git_commit_dialog_unref_commit (self);

  if (priv->table)
    {
      g_object_unref (priv->table);
      priv->table = NULL;
    }

  if (priv->log_view)
    {
      g_object_unref (priv->log_view);
      priv->log_view = NULL;
    }

  G_OBJECT_CLASS (git_commit_dialog_parent_class)->dispose (object);
}

GtkWidget *
git_commit_dialog_new (void)
{
  GtkWidget *self = g_object_new (GIT_TYPE_COMMIT_DIALOG, NULL);

  return self;
}

static void
git_commit_dialog_update (GitCommitDialog *cdiag)
{
  GitCommitDialogPrivate *priv = cdiag->priv;
  GtkWidget *widget;

  if (priv->table)
    {
      /* Remove all existing parent commit labels */
      gtk_container_foreach (GTK_CONTAINER (priv->table),
			     (GtkCallback) gtk_widget_destroy, NULL);

      if (priv->commit)
	{
	  /* Add a row for the commit's own hash */
	  widget = gtk_label_new (NULL);
	  gtk_label_set_markup (GTK_LABEL (widget), _("<b>Commit</b>"));
	  gtk_widget_show (widget);
	  gtk_table_attach (GTK_TABLE (priv->table),
			    widget,
			    0, 1, 0, 1,
			    GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK,
			    0, 0);

	  widget = gtk_label_new (git_commit_get_hash (priv->commit));
	  gtk_widget_show (widget);
	  gtk_table_attach (GTK_TABLE (priv->table),
			    widget,
			    1, 2, 0, 1,
			    GTK_FILL | GTK_SHRINK | GTK_EXPAND,
			    GTK_FILL | GTK_SHRINK,
			    0, 0);

	  if (git_commit_get_has_log_data (priv->commit))
	    {
	      const GSList *node;
	      int y = 1;

	      for (node = git_commit_get_parents (priv->commit);
		   node;
		   node = node->next, y++)
		{
		  GitCommit *commit = (GitCommit *) node->data;

		  widget = gtk_label_new (NULL);
		  gtk_label_set_markup (GTK_LABEL (widget), _("<b>Parent</b>"));
		  gtk_widget_show (widget);
		  gtk_table_attach (GTK_TABLE (priv->table),
				    widget,
				    0, 1, y, y + 1,
				    GTK_FILL | GTK_SHRINK,
				    GTK_FILL | GTK_SHRINK,
				    0, 0);

		  widget = gtk_label_new (git_commit_get_hash (commit));
		  gtk_widget_show (widget);
		  gtk_table_attach (GTK_TABLE (priv->table),
				    widget,
				    1, 2, y, y + 1,
				    GTK_FILL | GTK_SHRINK | GTK_EXPAND,
				    GTK_FILL | GTK_SHRINK,
				    0, 0);
		}
	    }
	}
    }

  if (priv->log_view)
    {
      GtkTextBuffer *buffer
	= gtk_text_view_get_buffer (GTK_TEXT_VIEW (priv->log_view));

      gtk_text_buffer_set_text (buffer,
				priv->commit == NULL ? ""
				: git_commit_get_has_log_data (priv->commit)
				? git_commit_get_log_data (priv->commit)
				: _("Loading..."),
				-1);
    }      
}

void
git_commit_dialog_set_commit (GitCommitDialog *cdiag,
			      GitCommit *commit)
{
  GitCommitDialogPrivate *priv;

  g_return_if_fail (GIT_IS_COMMIT_DIALOG (cdiag));
  g_return_if_fail (commit == NULL || GIT_IS_COMMIT (commit));

  priv = cdiag->priv;

  if (commit)
    g_object_ref (commit);

  git_commit_dialog_unref_commit (cdiag);

  priv->commit = commit;

  if (commit)
    {
      priv->has_log_data_handler
	= g_signal_connect_swapped (commit, "notify::has-log-data",
				    G_CALLBACK (git_commit_dialog_update),
				    cdiag);
      git_commit_fetch_log_data (commit);
    }

  git_commit_dialog_update (cdiag);

  g_object_notify (G_OBJECT (cdiag), "commit");
}

GitCommit *
git_commit_dialog_get_commit (GitCommitDialog *cdiag)
{
  g_return_val_if_fail (GIT_IS_COMMIT_DIALOG (cdiag), NULL);

  return cdiag->priv->commit;
}

static void
git_commit_dialog_set_property (GObject *object,
				guint property_id,
				const GValue *value,
				GParamSpec *pspec)
{
  GitCommitDialog *cdiag = (GitCommitDialog *) object;

  switch (property_id)
    {
    case PROP_COMMIT:
      git_commit_dialog_set_commit (cdiag, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
git_commit_dialog_get_property (GObject *object,
				guint property_id,
				GValue *value,
				GParamSpec *pspec)
{
  GitCommitDialog *cdiag = (GitCommitDialog *) object;

  switch (property_id)
    {
    case PROP_COMMIT:
      g_value_set_object (value, git_commit_dialog_get_commit (cdiag));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}
