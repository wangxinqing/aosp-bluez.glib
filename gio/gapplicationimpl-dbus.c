/*
 * Copyright © 2010 Codethink Limited
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors: Ryan Lortie <desrt@desrt.ca>
 */

#include "gapplicationimpl.h"

#include "gapplication.h"
#include "gfile.h"
#include "gdbusconnection.h"
#include "gdbusintrospection.h"
#include "gdbuserror.h"

#include <string.h>
#include <stdio.h>

#include "gapplicationimpl-dbus-interface.c"
#include "gapplicationcommandline.h"
#include "gdbusmethodinvocation.h"

struct _GApplicationImpl
{
  GDBusConnection *session_bus;
  const gchar     *bus_name;
  gchar           *object_path;
  guint            object_id;
  gpointer         app;
};


static GApplicationCommandLine *
g_dbus_command_line_new (GDBusMethodInvocation *invocation);


static void
g_application_impl_method_call (GDBusConnection       *connection,
                                const gchar           *sender,
                                const gchar           *object_path,
                                const gchar           *interface_name,
                                const gchar           *method_name,
                                GVariant              *parameters,
                                GDBusMethodInvocation *invocation,
                                gpointer               user_data)
{
  GApplicationImpl *impl = user_data;
  GApplicationClass *class;

  class = G_APPLICATION_GET_CLASS (impl->app);

  if (strcmp (method_name, "Activate") == 0)
    {
      GVariant *platform_data;

      g_variant_get (parameters, "(@a{sv})", &platform_data);
      class->before_emit (impl->app, platform_data);
      g_signal_emit_by_name (impl->app, "activate");
      class->after_emit (impl->app, platform_data);
      g_variant_unref (platform_data);
    }

  else if (strcmp (method_name, "Open") == 0)
    {
      GVariant *platform_data;
      const gchar *hint;
      GVariant *array;
      GFile **files;
      gint n, i;

      g_variant_get (parameters, "(@ass@a{sv})",
                     &array, &hint, &platform_data);

      n = g_variant_n_children (array);
      files = g_new (GFile *, n + 1);

      for (i = 0; i < n; i++)
        {
          const gchar *uri;

          g_variant_get_child (array, i, "&s", &uri);
          files[i] = g_file_new_for_uri (uri);
        }
      g_variant_unref (array);
      files[n] = NULL;

      class->before_emit (impl->app, platform_data);
      g_signal_emit_by_name (impl->app, "open", files, n, hint);
      class->after_emit (impl->app, platform_data);

      g_variant_unref (platform_data);

      for (i = 0; i < n; i++)
        g_object_unref (files[i]);
      g_free (files);
    }

  else if (strcmp (method_name, "CommandLine") == 0)
    {
      GApplicationCommandLine *cmdline;
      GVariant *platform_data;
      int status;

      cmdline = g_dbus_command_line_new (invocation);
      platform_data = g_variant_get_child_value (parameters, 2);
      class->before_emit (impl->app, platform_data);
      g_signal_emit_by_name (impl->app, "command-line", cmdline, &status);
      g_application_command_line_set_exit_status (cmdline, status);
      class->after_emit (impl->app, platform_data);
      g_variant_unref (platform_data);
      g_object_unref (cmdline);
    }

  else
    g_assert_not_reached ();
}

static gchar *
application_path_from_appid (const gchar *appid)
{
  gchar *appid_path, *iter;

  appid_path = g_strconcat ("/", appid, NULL);
  for (iter = appid_path; *iter; iter++)
    {
      if (*iter == '.')
        *iter = '/';
    }

  return appid_path;
}

void
g_application_impl_destroy (GApplicationImpl *impl)
{
  if (impl->session_bus)
    {
      if (impl->object_id)
        g_dbus_connection_unregister_object (impl->session_bus,
                                             impl->object_id);

      g_object_unref (impl->session_bus);
      g_free (impl->object_path);
    }
  else
    {
      g_assert (impl->object_path == NULL);
      g_assert (impl->object_id == 0);
    }

  g_slice_free (GApplicationImpl, impl);
}

GApplicationImpl *
g_application_impl_register (GApplication       *application,
                             const gchar        *appid,
                             GApplicationFlags   flags,
                             gboolean           *is_remote,
                             GCancellable       *cancellable,
                             GError            **error)
{
  const static GDBusInterfaceVTable vtable = {
    g_application_impl_method_call
  };
  GApplicationImpl *impl;
  GVariant *reply;
  guint32 rval;

  impl = g_slice_new (GApplicationImpl);

  impl->app = application;
  impl->bus_name = appid;

  impl->session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION,
                                      cancellable, error);

  if (impl->session_bus == NULL)
    {
      g_slice_free (GApplicationImpl, impl);
      return NULL;
    }

  impl->object_path = application_path_from_appid (appid);

  if (flags & G_APPLICATION_IS_LAUNCHER)
    {
      impl->object_id = 0;
      *is_remote = TRUE;

      return impl;
    }

  impl->object_id = g_dbus_connection_register_object (impl->session_bus,
                                                       impl->object_path,
                                                       (GDBusInterfaceInfo *)
                                                         &org_gtk_Application,
                                                       &vtable,
                                                       impl, NULL,
                                                       error);

  if (impl->object_id == 0)
    {
      g_object_unref (impl->session_bus);
      g_free (impl->object_path);
      impl->session_bus = NULL;
      impl->object_path = NULL;

      g_slice_free (GApplicationImpl, impl);
      return NULL;
    }

  reply = g_dbus_connection_call_sync (impl->session_bus,
                                       "org.freedesktop.DBus",
                                       "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus",
                                       "RequestName",
                                       g_variant_new ("(su)",
                                       /* DBUS_NAME_FLAG_DO_NOT_QUEUE: 0x4 */
                                                      impl->bus_name, 0x4),
                                       G_VARIANT_TYPE ("(u)"),
                                       0, -1, cancellable, error);

  if (reply == NULL)
    {
      g_dbus_connection_unregister_object (impl->session_bus,
                                           impl->object_id);
      impl->object_id = 0;

      g_object_unref (impl->session_bus);
      g_free (impl->object_path);
      impl->session_bus = NULL;
      impl->object_path = NULL;

      g_slice_free (GApplicationImpl, impl);
      return NULL;
    }

  g_variant_get (reply, "(u)", &rval);
  g_variant_unref (reply);

  /* DBUS_REQUEST_NAME_REPLY_EXISTS: 3 */
  if ((*is_remote = (rval == 3)))
    {
      g_dbus_connection_unregister_object (impl->session_bus,
                                           impl->object_id);
      impl->object_id = 0;

      if (flags & G_APPLICATION_IS_SERVICE)
        {
          g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                       "Unable to acquire bus name `%s'", appid);
          g_object_unref (impl->session_bus);
          g_free (impl->object_path);

          g_slice_free (GApplicationImpl, impl);
          impl = NULL;
        }
    }

  return impl;
}

void
g_application_impl_activate (GApplicationImpl *impl,
                             GVariant         *platform_data)
{
  g_dbus_connection_call (impl->session_bus,
                          impl->bus_name,
                          impl->object_path,
                          "org.gtk.Application",
                          "Activate",
                          g_variant_new ("(@a{sv})", platform_data),
                          NULL, 0, -1, NULL, NULL, NULL);
}

void
g_application_impl_open (GApplicationImpl  *impl,
                         GFile            **files,
                         gint               n_files,
                         const gchar       *hint,
                         GVariant          *platform_data)
{
  GVariantBuilder builder;
  gint i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("(assa{sv})"));
  g_variant_builder_open (&builder, G_VARIANT_TYPE_STRING_ARRAY);
  for (i = 0; i < n_files; i++)
    {
      gchar *uri = g_file_get_uri (files[i]);
      g_variant_builder_add (&builder, "s", uri);
      g_free (uri);
    }
  g_variant_builder_close (&builder);
  g_variant_builder_add (&builder, "s", hint);
  g_variant_builder_add_value (&builder, platform_data);

  g_dbus_connection_call (impl->session_bus,
                          impl->bus_name,
                          impl->object_path,
                          "org.gtk.Application",
                          "Open",
                          g_variant_builder_end (&builder),
                          NULL, 0, -1, NULL, NULL, NULL);
}

static void
g_application_impl_cmdline_method_call (GDBusConnection       *connection,
                                        const gchar           *sender,
                                        const gchar           *object_path,
                                        const gchar           *interface_name,
                                        const gchar           *method_name,
                                        GVariant              *parameters,
                                        GDBusMethodInvocation *invocation,
                                        gpointer               user_data)
{
  const gchar *message;

  g_variant_get_child (parameters, 0, "&s", &message);

  if (strcmp (method_name, "Print") == 0)
    g_print ("%s", message);
  else if (strcmp (method_name, "PrintError") == 0)
    g_printerr ("%s", message);
  else
    g_assert_not_reached ();

  g_dbus_method_invocation_return_value (invocation, NULL);
}

typedef struct
{
  GMainLoop *loop;
  int status;
} CommandLineData;

static void
g_application_impl_cmdline_done (GObject      *source,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  CommandLineData *data = user_data;
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source),
                                         result, &error);

  if (reply != NULL)
    {
      g_variant_get (reply, "(i)", &data->status);
      g_variant_unref (reply);
    }

  else
    {
      g_printerr ("%s\n", error->message);
      g_error_free (error);
      data->status = 1;
    }

  g_main_loop_quit (data->loop);
}

int
g_application_impl_command_line (GApplicationImpl *impl,
                                 GVariant         *arguments,
                                 GVariant         *platform_data)
{
  const static GDBusInterfaceVTable vtable = {
    g_application_impl_cmdline_method_call
  };
  const gchar *object_path = "/org/gtk/Application/CommandLine";
  GMainContext *context;
  CommandLineData data;
  guint object_id;

  context = g_main_context_new ();
  data.loop = g_main_loop_new (context, FALSE);
  g_main_context_push_thread_default (context);

  object_id = g_dbus_connection_register_object (impl->session_bus,
                                                 object_path,
                                                 (GDBusInterfaceInfo *)
                                                   &org_gtk_private_Cmdline,
                                                 &vtable, &data, NULL, NULL);
  /* In theory we should try other paths... */
  g_assert (object_id != 0);

  g_dbus_connection_call (impl->session_bus,
                          impl->bus_name,
                          impl->object_path,
                          "org.gtk.Application",
                          "CommandLine",
                          g_variant_new ("(o@aay@a{sv})", object_path,
                                         arguments, platform_data),
                          G_VARIANT_TYPE ("(i)"), 0, -1, NULL,
                          g_application_impl_cmdline_done, &data);

  g_main_loop_run (data.loop);

  g_main_context_pop_thread_default (context);
  g_main_context_unref (context);
  g_main_loop_unref (data.loop);

  return data.status;
}

void
g_application_impl_flush (GApplicationImpl *impl)
{
  g_dbus_connection_flush_sync (impl->session_bus, NULL, NULL);
}





typedef GApplicationCommandLineClass GDBusCommandLineClass;
static GType g_dbus_command_line_get_type (void);
typedef struct
{
  GApplicationCommandLine  parent_instance;
  GDBusMethodInvocation   *invocation;

  GDBusConnection *connection;
  const gchar     *bus_name;
  const gchar     *object_path;
} GDBusCommandLine;


G_DEFINE_TYPE (GDBusCommandLine,
               g_dbus_command_line,
               G_TYPE_APPLICATION_COMMAND_LINE)

static void
g_dbus_command_line_print_literal (GApplicationCommandLine *cmdline,
                                   const gchar             *message)
{
  GDBusCommandLine *gdbcl = (GDBusCommandLine *) cmdline;

  g_dbus_connection_call (gdbcl->connection,
                          gdbcl->bus_name,
                          gdbcl->object_path,
                          "org.gtk.private.CommandLine", "Print",
                          g_variant_new ("(s)", message),
                          NULL, 0, -1, NULL, NULL, NULL);
}

static void
g_dbus_command_line_printerr_literal (GApplicationCommandLine *cmdline,
                                      const gchar             *message)
{
  GDBusCommandLine *gdbcl = (GDBusCommandLine *) cmdline;

  g_dbus_connection_call (gdbcl->connection,
                          gdbcl->bus_name,
                          gdbcl->object_path,
                          "org.gtk.private.CommandLine", "PrintError",
                          g_variant_new ("(s)", message),
                          NULL, 0, -1, NULL, NULL, NULL);
}

static void
g_dbus_command_line_finalize (GObject *object)
{
  GApplicationCommandLine *cmdline = G_APPLICATION_COMMAND_LINE (object);
  GDBusCommandLine *gdbcl = (GDBusCommandLine *) object;
  gint status;

  status = g_application_command_line_get_exit_status (cmdline);

  g_dbus_method_invocation_return_value (gdbcl->invocation,
                                         g_variant_new ("(i)", status));
  g_object_unref (gdbcl->invocation);

  G_OBJECT_CLASS (g_dbus_command_line_parent_class)
    ->finalize (object);
}

static void
g_dbus_command_line_init (GDBusCommandLine *gdbcl)
{
}

static void
g_dbus_command_line_class_init (GApplicationCommandLineClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = g_dbus_command_line_finalize;
  class->printerr_literal = g_dbus_command_line_printerr_literal;
  class->print_literal = g_dbus_command_line_print_literal;
}

static GApplicationCommandLine *
g_dbus_command_line_new (GDBusMethodInvocation *invocation)
{
  GDBusCommandLine *gdbcl;
  GVariant *args;

  args = g_dbus_method_invocation_get_parameters (invocation);

  gdbcl = g_object_new (g_dbus_command_line_get_type (),
                        "arguments", g_variant_get_child_value (args, 1),
                        "platform-data", g_variant_get_child_value (args, 2),
                        NULL);
  gdbcl->connection = g_dbus_method_invocation_get_connection (invocation);
  gdbcl->bus_name = g_dbus_method_invocation_get_sender (invocation);
  g_variant_get_child (args, 0, "&o", &gdbcl->object_path);
  gdbcl->invocation = g_object_ref (invocation);

  return G_APPLICATION_COMMAND_LINE (gdbcl);
}