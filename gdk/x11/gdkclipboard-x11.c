/* GDK - The GIMP Drawing Kit
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gdkclipboardprivate.h"
#include "gdkclipboard-x11.h"

#include "gdkintl.h"
#include "gdkprivate-x11.h"
#include "gdkselectioninputstream-x11.h"
#include "gdkselectionoutputstream-x11.h"
#include "gdktextlistconverter-x11.h"
#include "gdk/gdk-private.h"

#include <string.h>
#include <X11/Xatom.h>

#ifdef HAVE_XFIXES
#include <X11/extensions/Xfixes.h>
#endif

#define IDLE_ABORT_TIME 30 /* seconds */

typedef struct _GdkX11ClipboardClass GdkX11ClipboardClass;

typedef struct _RetrievalInfo RetrievalInfo;

struct _GdkX11Clipboard
{
  GdkClipboard parent;

  char       *selection;
  Atom        xselection;
  gulong      timestamp;
};

struct _GdkX11ClipboardClass
{
  GdkClipboardClass parent_class;
};

G_DEFINE_TYPE (GdkX11Clipboard, gdk_x11_clipboard, GDK_TYPE_CLIPBOARD)

static void
print_atoms (GdkX11Clipboard *cb,
             const char      *prefix,
             const Atom      *atoms,
             gsize            n_atoms)
{
  GDK_NOTE(CLIPBOARD,
           GdkDisplay *display = gdk_clipboard_get_display (GDK_CLIPBOARD (cb));
           gsize i;
            
           g_printerr ("%s: %s [ ", cb->selection, prefix);
           for (i = 0; i < n_atoms; i++)
             {
               g_printerr ("%s%s", i > 0 ? ", " : "", gdk_x11_get_xatom_name_for_display (display , atoms[i]));
             }
           g_printerr (" ]\n");
          ); 
}

static void
gdk_x11_clipboard_default_output_done (GObject      *clipboard,
                                       GAsyncResult *result,
                                       gpointer      user_data)
{
  GError *error = NULL;

  if (!gdk_clipboard_write_finish (GDK_CLIPBOARD (clipboard), result, &error))
    {
      GDK_NOTE(CLIPBOARD, g_printerr ("%s: failed to write stream: %s\n", GDK_X11_CLIPBOARD (clipboard)->selection, error->message));
      g_error_free (error);
    }
}

static void
gdk_x11_clipboard_default_output_handler (GdkX11Clipboard *cb,
                                          const char      *target,
                                          GOutputStream   *stream)
{
  gdk_clipboard_write_async (GDK_CLIPBOARD (cb),
                             g_intern_string (target),
                             stream,
                             G_PRIORITY_DEFAULT,
                             NULL,
                             gdk_x11_clipboard_default_output_done,
                             NULL);
  g_object_unref (stream);
}

static void
handle_targets_done (GObject      *stream,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GError *error = NULL;
  gsize bytes_written;

  if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (stream), result, &bytes_written, &error))
    {
      GDK_NOTE(CLIPBOARD, g_printerr ("---: failed to send targets after %zu bytes: %s\n",
                                      bytes_written, error->message));
      g_error_free (error);
    }

  g_free (user_data);
}

static Atom *
gdk_x11_clipboard_formats_to_atoms (GdkDisplay        *display,
                                    GdkContentFormats *formats,
                                    gsize             *n_atoms);

static void
handle_targets (GdkX11Clipboard *cb,
                const char      *target,
                const char      *encoding,
                int              format,
                GOutputStream   *stream)
{
  GdkClipboard *clipboard = GDK_CLIPBOARD (cb);
  Atom *atoms;
  gsize n_atoms;

  atoms = gdk_x11_clipboard_formats_to_atoms (gdk_clipboard_get_display (clipboard),
                                              gdk_clipboard_get_formats (clipboard),
                                              &n_atoms);
  print_atoms (cb, "sending targets", atoms, n_atoms);
  g_output_stream_write_all_async (stream,
                                     atoms,
                                     n_atoms * sizeof (Atom),
                                     G_PRIORITY_DEFAULT,
                                     NULL,
                                     handle_targets_done,
                                     atoms);
  g_object_unref (stream);
}

static void
handle_timestamp_done (GObject      *stream,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  GError *error = NULL;
  gsize bytes_written;

  if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (stream), result, &bytes_written, &error))
    {
      GDK_NOTE(CLIPBOARD, g_printerr ("---: failed to send timestamp after %zu bytes: %s\n",
                                      bytes_written, error->message));
      g_error_free (error);
    }

  g_slice_free (gulong, user_data);
}

static void
handle_timestamp (GdkX11Clipboard *cb,
                  const char      *target,
                  const char      *encoding,
                  int              format,
                  GOutputStream   *stream)
{
  gulong *timestamp;

  timestamp = g_slice_new (gulong);
  *timestamp = cb->timestamp;

  g_output_stream_write_all_async (stream,
                                   timestamp,
                                   sizeof (gulong),
                                   G_PRIORITY_DEFAULT,
                                   NULL,
                                   handle_timestamp_done,
                                   timestamp);
  g_object_unref (stream);
}

static GInputStream * 
text_list_convert (GdkX11Clipboard *cb,
                   GInputStream    *stream,
                   const char      *encoding,
                   int              format)
{
  GInputStream *converter_stream;
  GConverter *converter;

  converter = gdk_x11_text_list_converter_to_utf8_new (gdk_clipboard_get_display (GDK_CLIPBOARD (cb)),
                                                       encoding,
                                                       format);
  converter_stream = g_converter_input_stream_new (stream, converter);

  g_object_unref (converter);
  g_object_unref (stream);

  return converter_stream;
}

static void
handle_text_list (GdkX11Clipboard *cb,
                  const char      *target,
                  const char      *encoding,
                  int              format,
                  GOutputStream   *stream)
{
  GOutputStream *converter_stream;
  GConverter *converter;

  converter = gdk_x11_text_list_converter_to_utf8_new (gdk_clipboard_get_display (GDK_CLIPBOARD (cb)),
                                                       encoding,
                                                       format);
  converter_stream = g_converter_output_stream_new (stream, converter);

  g_object_unref (converter);
  g_object_unref (stream);

  gdk_x11_clipboard_default_output_handler (cb, "text/plain;charset=utf-8", converter_stream);
}

static void
handle_utf8 (GdkX11Clipboard *cb,
             const char      *target,
             const char      *encoding,
             int              format,
             GOutputStream   *stream)
{
  gdk_x11_clipboard_default_output_handler (cb, "text/plain;charset=utf-8", stream);
}

static GInputStream * 
no_convert (GdkX11Clipboard *cb,
            GInputStream    *stream,
            const char      *encoding,
            int              format)
{
  return stream;
}

typedef void (* MimeTypeHandleFunc) (GdkX11Clipboard *, const char *, const char *, int, GOutputStream *);

static const struct {
  const char *x_target;
  const char *mime_type;
  GInputStream * (* convert) (GdkX11Clipboard *, GInputStream *, const char *, int);
  const char *type;
  gint format;
  MimeTypeHandleFunc handler;
} special_targets[] = {
  { "UTF8_STRING",   "text/plain;charset=utf-8", no_convert,        "UTF8_STRING",   8,  handle_utf8 },
  { "COMPOUND_TEXT", "text/plain;charset=utf-8", text_list_convert, "COMPOUND_TEXT", 8,  handle_text_list },
  { "TEXT",          "text/plain;charset=utf-8", text_list_convert, "STRING",        8,  handle_text_list },
  { "STRING",        "text/plain;charset=utf-8", text_list_convert, "STRING",        8,  handle_text_list },
  { "TARGETS",       NULL,                       NULL,              "ATOM",          32, handle_targets },
  { "TIMESTAMP",     NULL,                       NULL,              "INTEGER",       32, handle_timestamp }
};

static GSList *
gdk_x11_clipboard_formats_to_targets (GdkContentFormats *formats)
{
  GSList *targets;
  const char * const *mime_types;
  gsize i, j, n_mime_types;

  targets = NULL;
  mime_types = gdk_content_formats_get_mime_types (formats, &n_mime_types);

  for (i = 0; i < n_mime_types; i++)
    {
      for (j = 0; j < G_N_ELEMENTS (special_targets); j++)
        {
          if (special_targets[j].mime_type == NULL)
            continue;

          if (g_str_equal (mime_types[i], special_targets[j].mime_type))
            targets = g_slist_prepend (targets, (gpointer) g_intern_string (special_targets[j].x_target));
        }
      targets = g_slist_prepend (targets, (gpointer) mime_types[i]);
    }

  return g_slist_reverse (targets);
}

static Atom *
gdk_x11_clipboard_formats_to_atoms (GdkDisplay        *display,
                                    GdkContentFormats *formats,
                                    gsize             *n_atoms)
{
  GSList *l, *targets;
  Atom *atoms;
  gsize i;

  targets = gdk_x11_clipboard_formats_to_targets (formats);

  for (i = 0; i < G_N_ELEMENTS (special_targets); i++)
    {
      if (special_targets[i].mime_type != NULL)
        continue;

      if (special_targets[i].handler)
        targets = g_slist_prepend (targets, (gpointer) g_intern_string (special_targets[i].x_target));
    }

  *n_atoms = g_slist_length (targets);
  atoms = g_new (Atom, *n_atoms);
  i = 0;
  for (l = targets; l; l = l->next)
    atoms[i++] = gdk_x11_get_xatom_by_name_for_display (display, l->data);

  return atoms;
}

static GdkContentFormats *
gdk_x11_clipboard_formats_from_atoms (GdkDisplay *display,
                                      const Atom *atoms,
                                      gsize       n_atoms)
{
  GdkContentFormatsBuilder *builder;
  gsize i, j;

  builder = gdk_content_formats_builder_new ();
  for (i = 0; i < n_atoms; i++)
    {
      const char *name;

      name = gdk_x11_get_xatom_name_for_display (display , atoms[i]);
      if (strchr (name, '/'))
        {
          gdk_content_formats_builder_add_mime_type (builder, name);
          continue;
        }

      for (j = 0; j < G_N_ELEMENTS (special_targets); j++)
        {
          if (g_str_equal (name, special_targets[j].x_target))
            {
              if (special_targets[j].mime_type)
                gdk_content_formats_builder_add_mime_type (builder, special_targets[j].mime_type);
              break;
            }
        }
    }

  return gdk_content_formats_builder_free (builder);
}

static void
gdk_x11_clipboard_request_targets_finish (GObject      *source_object,
                                          GAsyncResult *res,
                                          gpointer      user_data)
{
  GInputStream *stream = G_INPUT_STREAM (source_object);
  GdkX11Clipboard *cb = user_data;
  GdkDisplay *display;
  GdkContentFormats *formats;
  GBytes *bytes;
  GError *error = NULL;

  bytes = g_input_stream_read_bytes_finish (stream, res, &error);
  if (bytes == NULL)
    {
      GDK_NOTE(CLIPBOARD, g_printerr ("%s: error reading TARGETS: %s\n", cb->selection, error->message));
      g_error_free (error);
      g_object_unref (stream);
      g_object_unref (cb);
      return;
    }
  else if (g_bytes_get_size (bytes) == 0)
    {
      g_bytes_unref (bytes);
      g_object_unref (stream);
      g_object_unref (cb);
      return;
    }

  print_atoms (cb,
               "received targets",
               g_bytes_get_data (bytes, NULL),
               g_bytes_get_size (bytes) / sizeof (Atom));

  display = gdk_clipboard_get_display (GDK_CLIPBOARD (cb));
  formats = gdk_x11_clipboard_formats_from_atoms (display,
                                                  g_bytes_get_data (bytes, NULL),
                                                  g_bytes_get_size (bytes) / sizeof (Atom));
  GDK_NOTE(CLIPBOARD, char *s = gdk_content_formats_to_string (formats); g_printerr ("%s: got formats: %s\n", cb->selection, s); g_free (s));

  /* union with previously loaded formats */
  gdk_clipboard_claim_remote (GDK_CLIPBOARD (cb), formats);
  gdk_content_formats_unref (formats);

  g_input_stream_read_bytes_async (stream,
                                   gdk_x11_display_get_max_request_size (display),
                                   G_PRIORITY_DEFAULT,
                                   NULL,
                                   gdk_x11_clipboard_request_targets_finish,
                                   cb);
}

static void
gdk_x11_clipboard_request_targets_got_stream (GObject      *source,
                                              GAsyncResult *result,
                                              gpointer      data)
{
  GdkX11Clipboard *cb = data;
  GInputStream *stream;
  GdkDisplay *display;
  GError *error = NULL;
  const char *type;
  int format;

  stream = gdk_x11_selection_input_stream_new_finish (result, &type, &format, &error);
  if (stream == NULL)
    {
      GDK_NOTE(CLIPBOARD, g_printerr ("%s: can't request TARGETS: %s\n", cb->selection, error->message));
      g_object_unref (cb);
      g_error_free (error);
      return;
    }
  else if (!g_str_equal (type, "ATOM") || format != 32)
    {
      GDK_NOTE(CLIPBOARD, g_printerr ("%s: Wrong reply type to TARGETS: type %s != ATOM or format %d != 32\n",
                                      cb->selection, type, format));
      g_input_stream_close (stream, NULL, NULL);
      g_object_unref (stream);
      g_object_unref (cb);
      return;
    }

  display = gdk_clipboard_get_display (GDK_CLIPBOARD (cb));

  g_input_stream_read_bytes_async (stream,
                                   gdk_x11_display_get_max_request_size (display),
                                   G_PRIORITY_DEFAULT,
                                   NULL,
                                   gdk_x11_clipboard_request_targets_finish,
                                   cb);
}

static void
gdk_x11_clipboard_request_targets (GdkX11Clipboard *cb)
{
  gdk_x11_selection_input_stream_new_async (gdk_clipboard_get_display (GDK_CLIPBOARD (cb)),
                                            cb->selection,
                                            "TARGETS",
                                            cb->timestamp,
                                            G_PRIORITY_DEFAULT,
                                            NULL,
                                            gdk_x11_clipboard_request_targets_got_stream,
                                            g_object_ref (cb));
}

static void
gdk_x11_clipboard_claim_remote (GdkX11Clipboard *cb,
                                guint32          timestamp)
{
  GdkContentFormats *empty;

  empty = gdk_content_formats_new (NULL, 0);
  gdk_clipboard_claim_remote (GDK_CLIPBOARD (cb), empty);
  gdk_content_formats_unref (empty);
  cb->timestamp = timestamp;
  gdk_x11_clipboard_request_targets (cb);
}

static void
gdk_x11_clipboard_request_selection (GdkX11Clipboard              *cb,
                                     GdkX11PendingSelectionNotify *notify,
                                     Window                        requestor,
                                     const char                   *target,
                                     const char                   *property,
                                     gulong                        timestamp)
{
  const char *mime_type;
  GdkDisplay *display;
  gsize i;

  display = gdk_clipboard_get_display (GDK_CLIPBOARD (cb));
  mime_type = gdk_intern_mime_type (target);

  if (mime_type)
    {
      if (gdk_content_formats_contain_mime_type (gdk_clipboard_get_formats (GDK_CLIPBOARD (cb)), mime_type))
        {
          GOutputStream *stream;

          stream = gdk_x11_selection_output_stream_new (display,
                                                        notify,
                                                        requestor,
                                                        cb->selection,
                                                        target,
                                                        property,
                                                        target,
                                                        8,
                                                        timestamp);
          gdk_x11_clipboard_default_output_handler (cb, target, stream);
        }
    }
  else
    {
      for (i = 0; i < G_N_ELEMENTS (special_targets); i++)
        {
          if (g_str_equal (target, special_targets[i].x_target) &&
              special_targets[i].handler)
            {
              GOutputStream *stream;

              if (special_targets[i].mime_type)
                mime_type = gdk_intern_mime_type (special_targets[i].mime_type);
              stream = gdk_x11_selection_output_stream_new (display,
                                                            notify,
                                                            requestor,
                                                            cb->selection,
                                                            target,
                                                            property,
                                                            special_targets[i].type,
                                                            special_targets[i].format,
                                                            timestamp);
              special_targets[i].handler (cb,
                                          target,
                                          special_targets[i].type,
                                          special_targets[i].format,
                                          stream);
              return;
            }
        }
    }

  gdk_x11_pending_selection_notify_send (notify, display, FALSE);
}

static GdkFilterReturn
gdk_x11_clipboard_filter_event (GdkXEvent *xev,
                                GdkEvent  *gdkevent,
                                gpointer   data)
{
  GdkX11Clipboard *cb = GDK_X11_CLIPBOARD (data);
  GdkDisplay *display;
  XEvent *xevent = xev;
  Window xwindow;

  display = gdk_clipboard_get_display (GDK_CLIPBOARD (cb));
  xwindow = GDK_X11_DISPLAY (display)->leader_window;

  if (xevent->xany.window != xwindow)
    return GDK_FILTER_CONTINUE;

  switch (xevent->type)
  {
    case SelectionClear:
      if (xevent->xselectionclear.selection != cb->xselection)
        return GDK_FILTER_CONTINUE;

      if (xevent->xselectionclear.time < cb->timestamp)
        {
          GDK_NOTE(CLIPBOARD, g_printerr ("%s: ignoring SelectionClear with too old timestamp (%lu vs %lu)\n",
                                          cb->selection, xevent->xselectionclear.time, cb->timestamp));
          return GDK_FILTER_CONTINUE;
        }

      GDK_NOTE(CLIPBOARD, g_printerr ("%s: got SelectionClear\n", cb->selection));
      gdk_x11_clipboard_claim_remote (cb, xevent->xselectionclear.time);
      return GDK_FILTER_REMOVE;

    case SelectionRequest:
      {
        GdkX11PendingSelectionNotify *notify;
        const char *target, *property;

        if (xevent->xselectionrequest.selection != cb->xselection)
          return GDK_FILTER_CONTINUE;

        target = gdk_x11_get_xatom_name_for_display (display, xevent->xselectionrequest.target);
        if (xevent->xselectionrequest.property == None)
          property = target;
        else
          property = gdk_x11_get_xatom_name_for_display (display, xevent->xselectionrequest.property);

        if (!gdk_clipboard_is_local (GDK_CLIPBOARD (cb)))
          {
            GDK_NOTE(CLIPBOARD, g_printerr ("%s: got SelectionRequest for %s @ %s even though we don't own the selection, huh?\n",
                                            cb->selection, target, property));
            return GDK_FILTER_REMOVE;
          }
        if (xevent->xselectionrequest.requestor == None)
          {
            GDK_NOTE(CLIPBOARD, g_printerr ("%s: got SelectionRequest for %s @ %s with NULL window, ignoring\n",
                                            cb->selection, target, property));
            return GDK_FILTER_REMOVE;
          }
        
        GDK_NOTE(CLIPBOARD, g_printerr ("%s: got SelectionRequest for %s @ %s\n",
                                        cb->selection, target, property));

        notify = gdk_x11_pending_selection_notify_new (xevent->xselectionrequest.requestor,
                                                       xevent->xselectionrequest.selection,
                                                       xevent->xselectionrequest.target,
                                                       xevent->xselectionrequest.property ? xevent->xselectionrequest.property
                                                                                          : xevent->xselectionrequest.target,
                                                       xevent->xselectionrequest.time);

        gdk_x11_clipboard_request_selection (cb,
                                             notify,
                                             xevent->xselectionrequest.requestor,
                                             target,
                                             property,
                                             xevent->xselectionrequest.time);
        return GDK_FILTER_REMOVE;
      }

    default:
#ifdef HAVE_XFIXES
      if (xevent->type - GDK_X11_DISPLAY (display)->xfixes_event_base == XFixesSelectionNotify)
        {
          XFixesSelectionNotifyEvent *sn = (XFixesSelectionNotifyEvent *) xevent;

          if (sn->selection != cb->xselection)
            return GDK_FILTER_CONTINUE;

          if (sn->owner == GDK_X11_DISPLAY (display)->leader_window)
            {
              GDK_NOTE(CLIPBOARD, g_printerr ("%s: Ignoring XFixesSelectionNotify for ourselves\n",
                                              cb->selection));
              return GDK_FILTER_CONTINUE;
            }

          GDK_NOTE(CLIPBOARD, g_printerr ("%s: Received XFixesSelectionNotify, claiming selection\n",
                                          cb->selection));

          gdk_x11_clipboard_claim_remote (cb, sn->selection_timestamp);
        }
#endif
      return GDK_FILTER_CONTINUE;
  }
}

static void
gdk_x11_clipboard_finalize (GObject *object)
{
  GdkX11Clipboard *cb = GDK_X11_CLIPBOARD (object);

  gdk_window_remove_filter (NULL, gdk_x11_clipboard_filter_event, cb);
  g_free (cb->selection);

  G_OBJECT_CLASS (gdk_x11_clipboard_parent_class)->finalize (object);
}

static gboolean
gdk_x11_clipboard_claim (GdkClipboard       *clipboard,
                         GdkContentFormats  *formats,
                         gboolean            local,
                         GdkContentProvider *content)
{
  if (local)
    {
      GdkX11Clipboard *cb = GDK_X11_CLIPBOARD (clipboard);
      GdkDisplay *display = gdk_clipboard_get_display (GDK_CLIPBOARD (cb));
      Display *xdisplay = GDK_DISPLAY_XDISPLAY (display);
      Window xwindow = GDK_X11_DISPLAY (display)->leader_window;
      guint32 time;

      time = gdk_display_get_last_seen_time (display);

      if (content)
        {
          XSetSelectionOwner (xdisplay, cb->xselection, xwindow, time);

          if (XGetSelectionOwner (xdisplay, cb->xselection) != xwindow)
            {
              GDK_NOTE(CLIPBOARD, g_printerr ("%s: failed XSetSelectionOwner()\n", cb->selection));
              return FALSE;
            }
        }
      else
        {
          XSetSelectionOwner (xdisplay, cb->xselection, None, time);
        }

      cb->timestamp = time;
      GDK_NOTE(CLIPBOARD, g_printerr ("%s: claimed via XSetSelectionOwner()\n", cb->selection));
    }

  return GDK_CLIPBOARD_CLASS (gdk_x11_clipboard_parent_class)->claim (clipboard, formats, local, content);
}

static void
gdk_x11_clipboard_read_got_stream (GObject      *source,
                                   GAsyncResult *res,
                                   gpointer      data)
{
  GTask *task = data;
  GError *error = NULL;
  GInputStream *stream;
  const char *type;
  int format;
  
  stream = gdk_x11_selection_input_stream_new_finish (res, &type, &format, &error);
  if (stream == NULL)
    {
      GSList *targets, *next;
      
      targets = g_task_get_task_data (task);
      next = targets->next;
      if (next)
        {
          GdkX11Clipboard *cb = GDK_X11_CLIPBOARD (g_task_get_source_object (task));

          GDK_NOTE(CLIPBOARD, g_printerr ("%s: reading %s failed, trying %s next\n",
                                          cb->selection, (char *) targets->data, (char *) next->data));
          targets->next = NULL;
          g_task_set_task_data (task, next, (GDestroyNotify) g_slist_free);
          gdk_x11_selection_input_stream_new_async (gdk_clipboard_get_display (GDK_CLIPBOARD (cb)),
                                                    cb->selection,
                                                    next->data,
                                                    cb->timestamp,
                                                    g_task_get_priority (task),
                                                    g_task_get_cancellable (task),
                                                    gdk_x11_clipboard_read_got_stream,
                                                    task);
          g_error_free (error);
          return;
        }

      g_task_return_error (task, error);
    }
  else
    {
      GdkX11Clipboard *cb = GDK_X11_CLIPBOARD (g_task_get_source_object (task));
      const char *mime_type = ((GSList *) g_task_get_task_data (task))->data;
      gsize i;

      for (i = 0; i < G_N_ELEMENTS (special_targets); i++)
        {
          if (g_str_equal (mime_type, special_targets[i].x_target))
            {
              g_assert (special_targets[i].mime_type != NULL);

              GDK_NOTE(CLIPBOARD, g_printerr ("%s: reading with converter from %s to %s\n",
                                              cb->selection, mime_type, special_targets[i].mime_type));
              mime_type = g_intern_string (special_targets[i].mime_type);
              g_task_set_task_data (task, g_slist_prepend (NULL, (gpointer) mime_type), (GDestroyNotify) g_slist_free);
              stream = special_targets[i].convert (cb, stream, type, format);
              break;
            }
        }

      GDK_NOTE(CLIPBOARD, g_printerr ("%s: reading clipboard as %s now\n",
                                      cb->selection, mime_type));
      g_task_return_pointer (task, stream, g_object_unref);
    }

  g_object_unref (task);
}

static void
gdk_x11_clipboard_read_async (GdkClipboard        *clipboard,
                              GdkContentFormats   *formats,
                              int                  io_priority,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GdkX11Clipboard *cb = GDK_X11_CLIPBOARD (clipboard);
  GSList *targets;
  GTask *task;

  task = g_task_new (clipboard, cancellable, callback, user_data);
  g_task_set_priority (task, io_priority);
  g_task_set_source_tag (task, gdk_clipboard_read_async);

  targets = gdk_x11_clipboard_formats_to_targets (formats);
  g_task_set_task_data (task, targets, (GDestroyNotify) g_slist_free);
  if (targets == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               _("No compatible transfer format found"));
      return;
    }

  GDK_NOTE(CLIPBOARD, g_printerr ("%s: new read for %s (%u other options)\n",
                                  cb->selection, (char *) targets->data, g_slist_length (targets->next)));
  gdk_x11_selection_input_stream_new_async (gdk_clipboard_get_display (GDK_CLIPBOARD (cb)),
                                            cb->selection,
                                            targets->data,
                                            cb->timestamp,
                                            io_priority,
                                            cancellable,
                                            gdk_x11_clipboard_read_got_stream,
                                            task);
}

static GInputStream *
gdk_x11_clipboard_read_finish (GdkClipboard  *clipboard,
                               const char   **out_mime_type,
                               GAsyncResult  *result,
                               GError       **error)
{
  GInputStream *stream;
  GTask *task;

  g_return_val_if_fail (g_task_is_valid (result, G_OBJECT (clipboard)), NULL);
  task = G_TASK (result);
  g_return_val_if_fail (g_task_get_source_tag (task) == gdk_clipboard_read_async, NULL);

  stream = g_task_propagate_pointer (task, error);

  if (stream)
    {
      if (out_mime_type)
        {
          GSList *targets;

          targets = g_task_get_task_data (task);
          *out_mime_type = targets->data;
        }
      g_object_ref (stream);
    }
  else
    {
      if (out_mime_type)
        *out_mime_type = NULL;
    }

  return stream;
}

static void
gdk_x11_clipboard_class_init (GdkX11ClipboardClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GdkClipboardClass *clipboard_class = GDK_CLIPBOARD_CLASS (class);

  object_class->finalize = gdk_x11_clipboard_finalize;

  clipboard_class->claim = gdk_x11_clipboard_claim;
  clipboard_class->read_async = gdk_x11_clipboard_read_async;
  clipboard_class->read_finish = gdk_x11_clipboard_read_finish;
}

static void
gdk_x11_clipboard_init (GdkX11Clipboard *cb)
{
  cb->timestamp = CurrentTime;
}

GdkClipboard *
gdk_x11_clipboard_new (GdkDisplay  *display,
                       const gchar *selection)
{
  GdkX11Clipboard *cb;

  cb = g_object_new (GDK_TYPE_X11_CLIPBOARD,
                     "display", display,
                     NULL);

  cb->selection = g_strdup (selection);
  cb->xselection = gdk_x11_get_xatom_by_name_for_display (display, selection);

  gdk_display_request_selection_notification (display, gdk_atom_intern (selection, FALSE));
  gdk_window_add_filter (NULL, gdk_x11_clipboard_filter_event, cb);
  gdk_x11_clipboard_claim_remote (cb, CurrentTime);

  return GDK_CLIPBOARD (cb);
}
