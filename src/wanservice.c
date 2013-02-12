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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <glib.h>
#include <pbnjson.h>
#include <luna-service2/lunaservice.h>

#include "wandriver.h"
#include "wanservice.h"
#include "utils.h"
#include "luna_service_utils.h"

extern GMainLoop *event_loop;
static GSList *g_driver_list;

struct wan_service {
	struct wan_driver *driver;
	void *data;
	LSPalmService *palm_service;
	LSHandle *private_service;
};

bool _wan_service_getstatus_cb(LSHandle *handle, LSMessage *message, void *user_data);

static LSMethod _wan_service_methods[]  = {
	{ "getstatus", _wan_service_getstatus_cb },
	{ 0, 0 }
};

const char* wan_network_type_to_string(enum wan_network_type type)
{
	switch (type) {
	case WAN_NETWORK_TYPE_GPRS:
		return "gprs";
	case WAN_NETWORK_TYPE_EDGE:
		return "edge";
	case WAN_NETWORK_TYPE_UMTS:
		return "umts";
	case WAN_NETWORK_TYPE_HSDPA:
		return "hsdpa";
	case WAN_NETWORK_TYPE_1X:
		return "1x";
	case WAN_NETWORK_TYPE_EVDO:
		return "evdo";
	default:
		break;
	}

	return "none";
}

const char* wan_status_type_to_string(enum wan_status_type status)
{
	switch (status) {
	case WAN_STATUS_TYPE_DISABLE:
		return "disable";
	case WAN_STATUS_TYPE_DISABLING:
		return "disabling";
	case WAN_STATUS_TYPE_ENABLE:
		return "enable";
	}

	return NULL;
}

const char* wan_connection_status_to_string(enum wan_connection_status status)
{
	switch (status) {
	case WAN_CONNECTION_STATUS_ACTIVE:
		return "active";
	case WAN_CONNECTION_STATUS_CONNECTING:
		return "disconnecting";
	case WAN_CONNECTION_STATUS_DISCONNECTED:
		return "disconnected";
	case WAN_CONNECTION_STATUS_DISCONNECTING:
		return "disconnecting";
	case WAN_CONNECTION_STATUS_DORMANT:
		return "dormant";
	}

	return NULL;
}

const char* wan_service_type_to_string(enum wan_service_type type)
{
	switch (type) {
	case WAN_SERVICE_TYPE_INTERNET:
		return "internet";
	case WAN_SERVICE_TYPE_MMS:
		return "mms";
	case WAN_SERVICE_TYPE_SPRINT_PROVISIONING:
		return "sprintProvisioning";
	case WAN_SERVICE_TYPE_TETHERED:
		return "tethered";
	default:
		break;
	}

	return "unknown";
}

const char* wan_request_status_to_string(enum wan_request_status status)
{
	switch (status) {
	case WAN_REQUEST_STATUS_CONNECT_FAILED:
		return "connect failed";
	case WAN_REQUEST_STATUS_CONNECT_SUCCEEDED:
		return "connect succeeded";
	case WAN_REQUEST_STATUS_DISCONNECT_FAILED:
		return "disconnect failed";
	case WAN_REQUEST_STATUS_DISCONNECT_SUCCEEDED:
		return "disconnect succeeded";
	default:
		break;
	}

	return NULL;
}

struct wan_service* wan_service_create(void)
{
	struct wan_service *service;
	LSError error;

	if (g_driver_list == NULL) {
		g_message("Can't create WAN service as no suitable driver is available");
		return NULL;
	}

	service = g_try_new0(struct wan_service, 1);
	if (!service)
		return NULL;

	/* take first driver until we have some machanism to determine the best driver */
	service->driver = g_driver_list->data;

	if (service->driver->probe(service) < 0) {
		g_free(service);
		return NULL;
	}

	LSErrorInit(&error);

	if (!LSRegisterPalmService("com.palm.wan", &service->palm_service, &error)) {
		g_error("Failed to initialize the WAN service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSGmainAttachPalmService(service->palm_service, event_loop, &error)) {
		g_error("Failed to attach to glib mainloop for WAN service: %s", error.message);
		LSErrorFree(&error);
		goto error;
	}

	if (!LSPalmServiceRegisterCategory(service->palm_service, "/", NULL, _wan_service_methods,
			NULL, service, &error)) {
		g_warning("Could not register category for WAN service");
		LSErrorFree(&error);
		return NULL;
	}

	service->private_service = LSPalmServiceGetPrivateConnection(service->palm_service);

	return service;

error:
	if (service->palm_service &&
		LSUnregisterPalmService(service->palm_service, &error) < 0) {
		g_error("Could not unregister palm service: %s", error.message);
		LSErrorFree(&error);
	}

	g_free(service);

	return NULL;
}

void wan_service_free(struct wan_service *service)
{
	LSError error;

	LSErrorInit(&error);

	if (service->palm_service != NULL &&
		LSUnregisterPalmService(service->palm_service, &error) < 0) {
		g_error("Could not unregister palm service: %s", error.message);
		LSErrorFree(&error);
	}

	if (service->driver) {
		service->driver->remove(service);
		service->driver = NULL;
	}

	g_free(service);
}

void wan_service_set_data(struct wan_service *service, void *data)
{
	g_assert(service != NULL);
	service->data = data;
}

void* wan_service_get_data(struct wan_service *service)
{
	g_assert(service != NULL);
	return service->data;
}

int wan_driver_register(struct wan_driver *driver)
{
	if (driver->probe == NULL)
		return -EINVAL;

	g_driver_list = g_slist_prepend(g_driver_list, driver);

	return 0;
}

void wan_driver_unregister(struct wan_driver *driver)
{
	g_driver_list = g_slist_remove(g_driver_list, driver);
}

void wan_service_status_changed_notify(struct wan_service *service, struct wan_status *status)
{
	jvalue_ref reply_obj = NULL;
	jvalue_ref connected_services_obj = NULL;
	jvalue_ref service_obj = NULL;
	jvalue_ref services_obj = NULL;
	int n;
	struct wan_connected_service *wanservice;
	GSList *iter;

	reply_obj = jobject_create();

	jobject_put(reply_obj, J_CSTR_TO_JVAL("state"),
				jstring_create(status->state ? "enable" : "disable"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("roamGuard"),
				jstring_create(status->roam_guard ? "enable" : "disable"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("networktype"),
				jstring_create(wan_network_type_to_string(status->network_type)));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("dataaccess"),
				jstring_create(status->dataaccess_usable ? "usable" : "unusable"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("networkstatus"),
				jstring_create(status->network_attached ? "attached" : "notattached"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("wanstate"),
				jstring_create(wan_status_type_to_string(status->wan_status)));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("disablewan"),
				jstring_create(status->disablewan ? "on" : "off"));

	connected_services_obj = jarray_create(NULL);
	for (iter = status->connected_services; iter != NULL; iter = g_slist_next(iter)) {
		wanservice = iter->data;

		service_obj = jobject_create();
		services_obj = jarray_create(NULL);

		for (n = 0; n < WAN_SERVICE_TYPE_MAX; n++) {
			if (wanservice->services[n]) {
				jarray_append(services_obj,
					jstring_create(wan_service_type_to_string((enum wan_service_type) n)));
			}
		}

		jobject_put(service_obj, J_CSTR_TO_JVAL("service"), services_obj);
		jobject_put(service_obj, J_CSTR_TO_JVAL("cid"),
					jnumber_create_i32(wanservice->cid));
		jobject_put(service_obj, J_CSTR_TO_JVAL("connectstatus"),
					jstring_create(wan_connection_status_to_string(wanservice->connection_status)));
		jobject_put(service_obj, J_CSTR_TO_JVAL("ipaddress"),
					jstring_create(wanservice->ipaddress));
		jobject_put(service_obj, J_CSTR_TO_JVAL("requeststatus"),
					jstring_create(wan_request_status_to_string(wanservice->req_status)));
		jobject_put(service_obj, J_CSTR_TO_JVAL("errorCode"),
					jnumber_create_i32(wanservice->error_code));
		jobject_put(service_obj, J_CSTR_TO_JVAL("causeCode"),
					jnumber_create_i32(wanservice->cause_code));
		jobject_put(service_obj, J_CSTR_TO_JVAL("mipFailureCode"),
					jnumber_create_i32(wanservice->mip_failure_code));

		jarray_append(connected_services_obj, service_obj);
	}

	jobject_put(reply_obj, J_CSTR_TO_JVAL("connectedservices"), connected_services_obj);

	luna_service_post_subscription(service->private_service, "/", "getstatus", reply_obj);


	j_release(&reply_obj);
}

bool _wan_service_getstatus_cb(LSHandle *handle, LSMessage *message, void *user_data)
{
	jvalue_ref reply_obj = NULL;
	bool subscribed = false;

	reply_obj = jobject_create();

	subscribed = luna_service_check_for_subscription_and_process(handle, message);

	jobject_put(reply_obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorCode"), jnumber_create_i32(0));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("errorText"), jstring_create("success"));
	jobject_put(reply_obj, J_CSTR_TO_JVAL("subscribed"), jboolean_create(subscribed));

	if(!luna_service_message_validate_and_send(handle, message, reply_obj))
		luna_service_message_reply_error_internal(handle, message);

	j_release(&reply_obj);

	return true;
}

// vim:ts=4:sw=4:noexpandtab
