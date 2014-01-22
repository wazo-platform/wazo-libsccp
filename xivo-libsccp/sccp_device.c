#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/lock.h>

#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_session.h"

#define DEV_REGISTRY_BUCKETS 7

struct sccp_line {
	/* (static) */
	struct sccp_device *device;
	/* (dynamic) */
	struct sccp_line_cfg *line_cfg;

	/* special case of duplicated information from the config (static) */
	char name[SCCP_LINE_NAME_MAX];
};

struct sccp_device {
	/* (static) */
	ast_mutex_t lock;

	/* (dynamic, set to NULL when device is destroyed) */
	struct sccp_line *line;
	/* (static) */
	struct sccp_session *session;
	/* (dynamic) */
	struct sccp_device_cfg *device_cfg;

	/* if the device is a guest, then the name will be different then the
	 * device config name (static)
	 */
	char name[SCCP_DEVICE_NAME_MAX];
};

static void sccp_line_destructor(void *data)
{
	struct sccp_line *line = data;

	ast_log(LOG_DEBUG, "in destructor for line %s\n", line->name);

	ao2_cleanup(line->device);
	ao2_cleanup(line->line_cfg);
}

static struct sccp_line *sccp_line_alloc(const char *name)
{
	struct sccp_line *line;

	line = ao2_alloc_options(sizeof(*line), sccp_line_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!line) {
		return NULL;
	}

	line->device = NULL;
	line->line_cfg = NULL;
	ast_copy_string(line->name, name, sizeof(line->name));

	return line;
}

static void sccp_line_destroy(struct sccp_line *line)
{
	/* nothing to do for now */
	ast_log(LOG_DEBUG, "destroying line %s\n", line->name);
}

static void sccp_device_destructor(void *data)
{
	struct sccp_device *device = data;

	ast_log(LOG_DEBUG, "in destructor for device %s\n", device->name);

	if (device->line) {
		/*
		 * This should not happen since if device->line, then
		 * device->line->device == device, which means there should be at least
		 * one ao2 reference remaining, yet we are in the destructor
		 */
		ast_log(LOG_ERROR, "device->line is not null in destructor, something is wrong\n");
	}

	ast_mutex_destroy(&device->lock);
	ao2_cleanup(device->session);
	ao2_cleanup(device->device_cfg);
}

static struct sccp_device *sccp_device_alloc(const char *name)
{
	struct sccp_device *device;

	device = ao2_alloc_options(sizeof(*device), sccp_device_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!device) {
		return NULL;
	}

	ast_mutex_init(&device->lock);
	device->line = NULL;
	device->session = NULL;
	device->device_cfg = NULL;
	ast_copy_string(device->name, name, sizeof(device->name));

	return device;
}

struct sccp_device *sccp_device_create(struct sccp_device_cfg *device_cfg, const char *name, struct sccp_session *session)
{
	struct sccp_device *device;
	struct sccp_line *line;

	if (!device_cfg) {
		ast_log(LOG_ERROR, "sccp device create failed: device_cfg is null\n");
		return NULL;
	}

	if (!name) {
		ast_log(LOG_ERROR, "sccp device create failed: name is null\n");
		return NULL;
	}

	if (!session) {
		ast_log(LOG_ERROR, "sccp device create failed: session is null\n");
		return NULL;
	}

	device = sccp_device_alloc(name);
	if (!device) {
		return NULL;
	}

	line = sccp_line_alloc(device_cfg->line_cfg->name);
	if (!line) {
		ao2_ref(device, -1);
		return NULL;
	}

	device->device_cfg = device_cfg;
	ao2_ref(device_cfg, +1);

	device->session = session;
	ao2_ref(session, +1);

	/* steal the reference ownership */
	device->line = line;

	line->line_cfg = device_cfg->line_cfg;
	ao2_ref(device_cfg->line_cfg, +1);

	line->device = device;
	ao2_ref(device, +1);

	return device;
}

void sccp_device_destroy(struct sccp_device *device)
{
	struct sccp_line *line = device->line;

	ast_log(LOG_DEBUG, "destroying device %s\n", device->name);

	sccp_line_destroy(line);

	device->line = NULL;
	ao2_ref(line, -1);
}

void sccp_device_handle_msg(struct sccp_device *device, struct sccp_msg *msg)
{
}

void sccp_device_reload_config(struct sccp_device *device, struct sccp_device_cfg *dev_cfg)
{
}

int sccp_device_want_disconnect(struct sccp_device *device)
{
	return 0;
}

int sccp_device_want_unlink(struct sccp_device *device)
{
	return 0;
}

struct sccp_device_registry {
	struct ao2_container *devices;
};

static int sccp_device_hash(const void *obj, int flags)
{
	const char *name;

	if (flags & OBJ_KEY) {
		name = (const char *) obj;
	} else {
		name = ((const struct sccp_device *) obj)->name;
	}

	return ast_str_hash(name);
}

static int sccp_device_cmp(void *obj, void *arg, int flags)
{
	struct sccp_device *device = obj;
	const char *name;

	if (flags & OBJ_KEY) {
		name = (const char *) arg;
	} else {
		name = ((const struct sccp_device *) arg)->name;
	}

	return strcmp(device->name, name) ? 0 : (CMP_MATCH | CMP_STOP);
}

struct sccp_device_registry *sccp_device_registry_create(void)
{
	struct sccp_device_registry *registry;

	registry = ast_calloc(1, sizeof(*registry));
	if (!registry) {
		return NULL;
	}

	registry->devices = ao2_container_alloc(DEV_REGISTRY_BUCKETS, sccp_device_hash, sccp_device_cmp);
	if (!registry->devices) {
		ast_free(registry);
		return NULL;
	}

	return registry;
}

void sccp_device_registry_destroy(struct sccp_device_registry *registry)
{
	ao2_ref(registry->devices, -1);
	ast_free(registry);
}

int sccp_device_registry_add(struct sccp_device_registry *registry, struct sccp_device *device)
{
	struct sccp_device *other_device;

	if (!device) {
		ast_log(LOG_ERROR, "sccp device registry add failed: device is null\n");
		return -1;
	}

	ao2_lock(registry->devices);
	other_device = ao2_find(registry->devices, device->name, OBJ_NOLOCK | OBJ_KEY);
	if (other_device) {
		ao2_unlock(registry->devices);
		ao2_ref(other_device, -1);
		ast_log(LOG_DEBUG, "sccp device registry add failed: device already present\n");
		return -1;
	}

	if (!ao2_link_flags(registry->devices, device, OBJ_NOLOCK)) {
		ao2_unlock(registry->devices);
		return -1;
	}

	ao2_unlock(registry->devices);

	return 0;
}

void sccp_device_registry_remove(struct sccp_device_registry *registry, struct sccp_device *device)
{
	if (!device) {
		ast_log(LOG_ERROR, "sccp device registry remove failed: device is null\n");
		return;
	}

	ao2_unlink(registry->devices, device);
}
