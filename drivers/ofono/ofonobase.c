/* @@@LICENSE
*
* Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <string.h>
#include <errno.h>

#include <glib-object.h>
#include <gio/gio.h>

#include "utils.h"
#include "ofonobase.h"
#include "ofono-interface.h"

struct ofono_base {
	void *remote;
	void *user_data;
	struct ofono_base_funcs *funcs;
};

static void set_property_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_base_result_cb cb = cbd->cb;
	struct ofono_base *base = cbd->user;
	gboolean success = FALSE;
	GError *error = NULL;
	struct ofono_error oerr;

	success = base->funcs->set_property_finish(base->remote, res, &error);
	if (!success) {
		oerr.message = error->message;
		cb(&oerr, cbd->data);
		g_error_free(error);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
}

void ofono_base_set_property(struct ofono_base *base, const gchar *name, GVariant *value,
						 ofono_base_result_cb cb, gpointer user_data)
{
	struct cb_data *cbd = cb_data_new(cb, user_data);
	cbd->user = base;

	base->funcs->set_property(base->remote, name, value, NULL, set_property_cb, cbd);
}

static void handle_get_properties_result(struct ofono_base *base, GVariant *properties)
{
	gchar *property_name = NULL;
	GVariant *property_value = NULL;
	GVariantIter iter;

	g_variant_iter_init(&iter, properties);
	while (g_variant_iter_loop(&iter, "{sv}", &property_name, &property_value)) {
		base->funcs->update_property(property_name, property_value, base->user_data);
	}
}

static void get_properties_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct ofono_base *base = user_data;
	GError *error = NULL;
	gboolean success = FALSE;
	GVariant *properties = NULL;

	success = base->funcs->get_properties_finish(base->remote, &properties, res, &error);
	if (!success) {
		g_warning("Failed to retrieve properties from base: %s", error->message);
		g_error_free(error);
		return;
	}

	handle_get_properties_result(base, properties);
}

static void property_changed_cb(void *object, const gchar *name, GVariant *value, gpointer user_data)
{
	struct ofono_base *base = user_data;

	base->funcs->update_property(name, g_variant_get_variant(value), base->user_data);
}

struct ofono_base* ofono_base_create(struct ofono_base_funcs *funcs, void *remote, void *user_data)
{
	struct ofono_base *base;
	GError *error = NULL;
	GVariant *properties;

	base = g_try_new0(struct ofono_base, 1);
	if (!base)
		return NULL;

	base->remote = remote;
	base->user_data = user_data;
	base->funcs = funcs;

	g_signal_connect(G_OBJECT(base->remote), "property-changed",
		G_CALLBACK(property_changed_cb), base);

	if (base->funcs->get_properties) {
		base->funcs->get_properties(base->remote, NULL, get_properties_cb, base);
	}
	else {
		base->funcs->get_properties_sync(base->remote, &properties, NULL, &error);
		if (error) {
			g_warning("Failed to retrieve properties from base: %s", error->message);
			g_error_free(error);
		}
		else {
			handle_get_properties_result(base, properties);
		}
	}

	return base;
}

void ofono_base_free(struct ofono_base *base)
{
	if (!base)
		return;

	if (base->remote)
		g_object_unref(base->remote);

	g_free(base);
}

// vim:ts=4:sw=4:noexpandtab
