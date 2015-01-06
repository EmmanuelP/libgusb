/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2011-2014 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <glib.h>
#include <gusb/gusb.h>
#include <gusb/gusb-cleanup.h>
#include <string.h>

/**
 * gusb_log_ignore_cb:
 **/
static void
gusb_log_ignore_cb (const gchar *log_domain, GLogLevelFlags log_level,
		   const gchar *message, gpointer user_data)
{
}

/**
 * gusb_log_handler_cb:
 **/
static void
gusb_log_handler_cb (const gchar *log_domain, GLogLevelFlags log_level,
		    const gchar *message, gpointer user_data)
{
	gchar str_time[255];
	time_t the_time;

	/* header always in green */
	time (&the_time);
	strftime (str_time, 254, "%H:%M:%S", localtime (&the_time));
	g_print ("%c[%dmTI:%s\t", 0x1B, 32, str_time);

	/* critical is also in red */
	if (log_level == G_LOG_LEVEL_CRITICAL ||
	    log_level == G_LOG_LEVEL_WARNING ||
	    log_level == G_LOG_LEVEL_ERROR) {
		g_print ("%c[%dm%s\n%c[%dm", 0x1B, 31, message, 0x1B, 0);
	} else {
		/* debug in blue */
		g_print ("%c[%dm%s\n%c[%dm", 0x1B, 34, message, 0x1B, 0);
	}
}

typedef struct {
	GOptionContext		*context;
	GUsbContext		*usb_ctx;
	GPtrArray		*cmd_array;
} GUsbCmdPrivate;

typedef gboolean (*GUsbCmdPrivateCb)	(GUsbCmdPrivate	*cmd,
					 gchar		**values,
					 GError		**error);

typedef struct {
	gchar			*name;
	gchar			*description;
	GUsbCmdPrivateCb	 callback;
} GUsbCmdItem;

/**
 * gusb_cmd_item_free:
 **/
static void
gusb_cmd_item_free (GUsbCmdItem *item)
{
	g_free (item->name);
	g_free (item->description);
	g_slice_free (GUsbCmdItem, item);
}

/*
 * gusb_sort_command_name_cb:
 */
static gint
gusb_sort_command_name_cb (GUsbCmdItem **item1, GUsbCmdItem **item2)
{
	return g_strcmp0 ((*item1)->name, (*item2)->name);
}

/**
 * gusb_cmd_add:
 **/
static void
gusb_cmd_add (GPtrArray *array, const gchar *name, const gchar *description, GUsbCmdPrivateCb callback)
{
	GUsbCmdItem *item;
	guint i;
	_cleanup_strv_free_ gchar **names = NULL;

	/* add each one */
	names = g_strsplit (name, ",", -1);
	for (i=0; names[i] != NULL; i++) {
		item = g_slice_new0 (GUsbCmdItem);
		item->name = g_strdup (names[i]);
		if (i == 0) {
			item->description = g_strdup (description);
		} else {
			/* TRANSLATORS: this is a command alias */
			item->description = g_strdup_printf ("Alias to %s",
							     names[0]);
		}
		item->callback = callback;
		g_ptr_array_add (array, item);
	}
}

/**
 * gusb_cmd_get_descriptions:
 **/
static gchar *
gusb_cmd_get_descriptions (GPtrArray *array)
{
	guint i;
	guint j;
	guint len;
	guint max_len = 19;
	GUsbCmdItem *item;
	GString *string;

	/* print each command */
	string = g_string_new ("");
	for (i = 0; i < array->len; i++) {
		item = g_ptr_array_index (array, i);
		g_string_append (string, "  ");
		g_string_append (string, item->name);
		g_string_append (string, " ");
		len = strlen (item->name);
		for (j = len; j < max_len+2; j++)
			g_string_append_c (string, ' ');
		g_string_append (string, item->description);
		g_string_append_c (string, '\n');
	}

	/* remove trailing newline */
	if (string->len > 0)
		g_string_set_size (string, string->len - 1);

	return g_string_free (string, FALSE);
}

/**
 * gusb_device_list_added_cb:
 **/
static void
gusb_device_list_added_cb (GUsbContext *context,
			   GUsbDevice *device,
			   gpointer user_data)
{
	g_print ("device %s added %x:%x\n",
		 g_usb_device_get_platform_id (device),
		 g_usb_device_get_bus (device),
		 g_usb_device_get_address (device));
}

/**
 * gusb_device_list_removed_cb:
 **/
static void
gusb_device_list_removed_cb (GUsbContext *context,
			     GUsbDevice *device,
			     gpointer user_data)
{
	g_print ("device %s removed %x:%x\n",
		 g_usb_device_get_platform_id (device),
		 g_usb_device_get_bus (device),
		 g_usb_device_get_address (device));
}

/**
 * gusb_devices_sort_by_platform_id_cb:
 **/
static gint
gusb_devices_sort_by_platform_id_cb (gconstpointer a, gconstpointer b)
{
	GUsbDevice *device_a = *((GUsbDevice **) a);
	GUsbDevice *device_b = *((GUsbDevice **) b);
	return g_strcmp0 (g_usb_device_get_platform_id (device_a),
			  g_usb_device_get_platform_id (device_b));
}

static gboolean
moo_cb (GNode *node, gpointer data)
{
	GUsbDevice *device = G_USB_DEVICE (node->data);
	GNode *n;
	guint i;
	const gchar *tmp;
	_cleanup_free_ gchar *product = NULL;
	_cleanup_free_ gchar *vendor = NULL;
	_cleanup_string_free_ GString *str = NULL;

	if (device == NULL) {
		g_print ("Root Device\n");
		return FALSE;
	}

	/* indent */
	str = g_string_new ("");
	for (n = node; n->data != NULL; n = n->parent)
		g_string_append (str, " ");

	/* add bus:address */
	g_string_append_printf (str, "%02x:%02x [%04x:%04x]",
			        g_usb_device_get_bus (device),
			        g_usb_device_get_address (device),
			        g_usb_device_get_vid (device),
			        g_usb_device_get_pid (device));

	/* pad */
	for (i = str->len; i < 30; i++)
		g_string_append (str, " ");

	/* We don't error check these as not all devices have these
	   (and the device_open may have failed). */
	g_usb_device_open (device, NULL);
	vendor = g_usb_device_get_string_descriptor (device,
			g_usb_device_get_manufacturer_index (device),
			NULL);
	product = g_usb_device_get_string_descriptor (device,
			g_usb_device_get_product_index (device),
			NULL);

	/* lookup from usb.ids */
	if (vendor == NULL) {
		tmp = g_usb_device_get_vid_as_str (device);
		if (tmp != NULL)
			vendor = g_strdup (tmp);
	}
	if (product == NULL) {
		tmp = g_usb_device_get_pid_as_str (device);
		if (tmp != NULL)
			product = g_strdup (tmp);
	}

	/* a hub */
	if (g_usb_device_get_device_class (device) == 0x09) {
		if (product == NULL)
			product = g_strdup ("USB HUB");
	}

	/* fall back to the VID/PID */
	if (product == NULL)
		product = g_strdup ("Unknown");
	if (vendor == NULL)
		vendor = g_strdup ("Unknown");

	/* add bus:address */
	g_string_append_printf (str, "%s - %s", vendor, product);
	g_print ("%s\n", str->str);
	return FALSE;
}

/**
 * gusb_cmd_show:
 **/
static gboolean
gusb_cmd_show (GUsbCmdPrivate *priv, gchar **values, GError **error)
{
	GNode *n;
	GNode *node;
	guint i;
	GUsbDevice *device;
	GUsbDevice *parent;
	_cleanup_ptrarray_unref_ GPtrArray *devices = NULL;

	/* sort */
	devices = g_usb_context_get_devices (priv->usb_ctx);
	g_ptr_array_sort (devices, gusb_devices_sort_by_platform_id_cb);

	/* make a tree of the devices */
	node = g_node_new (NULL);
	for (i = 0; i < devices->len; i++) {
		device = g_ptr_array_index (devices, i);

		parent = g_usb_device_get_parent (device);
		if (parent == NULL) {
			g_node_append_data (node, device);
			continue;
		}
		n = g_node_find (node, G_PRE_ORDER, G_TRAVERSE_ALL, parent);
		if (n == NULL) {
			g_set_error (error, 1, 0,
				     "no parent node for %s",
				     g_usb_device_get_platform_id (device));
			return FALSE;
		}
		g_node_append_data (n, device);

	}
	g_node_traverse (node, G_PRE_ORDER, G_TRAVERSE_ALL, -1, moo_cb, priv);
	return TRUE;
}

/**
 * gusb_cmd_watch:
 **/
static gboolean
gusb_cmd_watch (GUsbCmdPrivate *priv, gchar **values, GError **error)
{
	gboolean ret = TRUE;
	guint i;
	GUsbDevice *device;
	GMainLoop *loop;
	_cleanup_ptrarray_unref_ GPtrArray *devices = NULL;

	devices = g_usb_context_get_devices (priv->usb_ctx);
	for (i = 0; i < devices->len; i++) {
		device = g_ptr_array_index (devices, i);
		g_print ("device %s already present %x:%x\n",
			 g_usb_device_get_platform_id (device),
			 g_usb_device_get_bus (device),
			 g_usb_device_get_address (device));
	}

	loop = g_main_loop_new (NULL, FALSE);
	g_signal_connect (priv->usb_ctx, "device-added",
			  G_CALLBACK (gusb_device_list_added_cb),
			  priv);
	g_signal_connect (priv->usb_ctx, "device-removed",
			  G_CALLBACK (gusb_device_list_removed_cb),
			  priv);
	g_main_loop_run (loop);

	g_main_loop_unref (loop);
	return ret;
}

/**
 * gusb_cmd_run:
 **/
static gboolean
gusb_cmd_run (GUsbCmdPrivate *priv, const gchar *command, gchar **values, GError **error)
{
	guint i;
	GUsbCmdItem *item;
	_cleanup_string_free_ GString *string = NULL;

	/* find command */
	for (i = 0; i < priv->cmd_array->len; i++) {
		item = g_ptr_array_index (priv->cmd_array, i);
		if (g_strcmp0 (item->name, command) == 0)
			return item->callback (priv, values, error);
	}

	/* not found */
	string = g_string_new ("");
	/* TRANSLATORS: error message */
	g_string_append_printf (string, "%s\n", "Command not found, valid commands are:");
	for (i = 0; i < priv->cmd_array->len; i++) {
		item = g_ptr_array_index (priv->cmd_array, i);
		g_string_append_printf (string, " * %s\n", item->name);
	}
	g_set_error_literal (error, 1, 0, string->str);
	return FALSE;
}

/**
 * main:
 **/
int
main (int argc, char *argv[])
{
	gboolean ret;
	gboolean verbose = FALSE;
	gint retval = 0;
	GUsbCmdPrivate *priv;
	_cleanup_free_ gchar *cmd_descriptions = NULL;
	_cleanup_free_ gchar *options_help = NULL;
	_cleanup_error_free_ GError *error = NULL;
	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			"Show extra debugging information", NULL },
		{ NULL}
	};

	/* create helper object */
	priv = g_slice_new0 (GUsbCmdPrivate);

	priv->context = g_option_context_new ("GUSB Console Program");
	g_option_context_add_main_entries (priv->context, options, NULL);
	g_option_context_parse (priv->context, &argc, &argv, NULL);

	/* verbose? */
	if (verbose) {
		g_log_set_fatal_mask (NULL, G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
		g_log_set_handler ("GUsb", G_LOG_LEVEL_ERROR |
					  G_LOG_LEVEL_CRITICAL |
					  G_LOG_LEVEL_DEBUG |
					  G_LOG_LEVEL_WARNING,
				   gusb_log_handler_cb, NULL);
	} else {
		/* hide all debugging */
		g_log_set_fatal_mask (NULL, G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
		g_log_set_handler ("GUsb", G_LOG_LEVEL_DEBUG,
				   gusb_log_ignore_cb, NULL);
	}

	/* GUsbContext */
	priv->usb_ctx = g_usb_context_new (NULL);

	/* add commands */
	priv->cmd_array = g_ptr_array_new_with_free_func ((GDestroyNotify) gusb_cmd_item_free);
	gusb_cmd_add (priv->cmd_array,
		     "show",
		     "Show currently connected devices",
		     gusb_cmd_show);
	gusb_cmd_add (priv->cmd_array,
		     "watch",
		     "Watch devices as they come and go",
		     gusb_cmd_watch);

	/* sort by command name */
	g_ptr_array_sort (priv->cmd_array,
			  (GCompareFunc) gusb_sort_command_name_cb);

	/* get a list of the commands */
	cmd_descriptions = gusb_cmd_get_descriptions (priv->cmd_array);
	g_option_context_set_summary (priv->context, cmd_descriptions);

	/* nothing specified */
	if (argc < 2) {
		options_help = g_option_context_get_help (priv->context, TRUE, NULL);
		g_print ("%s", options_help);
		goto out;
	}

	/* run the specified command */
	ret = gusb_cmd_run (priv, argv[1], (gchar**) &argv[2], &error);
	if (!ret) {
		g_print ("%s\n", error->message);
		retval = 1;
		goto out;
	}
out:
	if (priv != NULL) {
		if (priv->cmd_array != NULL)
			g_ptr_array_unref (priv->cmd_array);
		if (priv->usb_ctx != NULL)
			g_object_unref (priv->usb_ctx);
		g_option_context_free (priv->context);
		g_slice_free (GUsbCmdPrivate, priv);
	}
	return retval;
}
