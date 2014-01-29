#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/strings.h>

#include "sccp_device.h"
#include "sccp_device_registry.h"

#define DEV_REGISTRY_BUCKETS 7

struct sccp_device_registry {
	struct ao2_container *devices;
};

static int sccp_device_hash(const void *obj, int flags)
{
	const char *name;

	if (flags & OBJ_KEY) {
		name = (const char *) obj;
	} else {
		name = sccp_device_name((const struct sccp_device *) obj);
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
		name = sccp_device_name((const struct sccp_device *) arg);
	}

	return strcmp(sccp_device_name(device), name) ? 0 : (CMP_MATCH | CMP_STOP);
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
	other_device = ao2_find(registry->devices, sccp_device_name(device), OBJ_NOLOCK | OBJ_KEY);
	if (other_device) {
		ao2_unlock(registry->devices);
		ao2_ref(other_device, -1);
		return SCCP_DEVICE_REGISTRY_ALREADY;
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

	ao2_lock(registry->devices);
	ao2_unlink_flags(registry->devices, device, OBJ_NOLOCK);
	ao2_unlock(registry->devices);
}

struct sccp_device *sccp_device_registry_find(struct sccp_device_registry *registry, const char *name)
{
	if (!name) {
		ast_log(LOG_ERROR, "registry find failed: name is null\n");
		return NULL;
	}

	return ao2_find(registry->devices, name, OBJ_KEY);
}

int sccp_device_registry_take_snapshots(struct sccp_device_registry *registry, struct sccp_device_snapshot **snapshots, size_t *n)
{
	struct ao2_iterator iter;
	struct sccp_device *device;
	size_t i;
	int ret = 0;

	if (!snapshots) {
		ast_log(LOG_ERROR, "registry take snapshots failed: snapshots is null\n");
		return -1;
	}

	if (!n) {
		ast_log(LOG_ERROR, "registry take snapshots failed: n is null\n");
		return -1;
	}

	ao2_lock(registry->devices);
	*n = ao2_container_count(registry->devices);
	if (!*n) {
		*snapshots = NULL;
		goto unlock;
	}

	*snapshots = ast_calloc(*n, sizeof(**snapshots));
	if (!*snapshots) {
		ret = -1;
		goto unlock;
	}

	i = 0;
	iter = ao2_iterator_init(registry->devices, AO2_ITERATOR_DONTLOCK);
	while ((device = ao2_iterator_next(&iter))) {
		sccp_device_take_snapshot(device, &(*snapshots)[i++]);
		ao2_ref(device, -1);
	}
	ao2_iterator_destroy(&iter);

unlock:
	ao2_unlock(registry->devices);

	return ret;
}
