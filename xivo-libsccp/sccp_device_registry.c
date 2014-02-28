#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/lock.h>
#include <asterisk/strings.h>

#include "sccp.h"
#include "sccp_device.h"
#include "sccp_device_registry.h"

struct sccp_device_registry {
	ast_mutex_t lock;
	struct ao2_container *devices;
	struct ao2_container *lines;
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

static int sccp_line_hash(const void *obj, int flags)
{
	const char *name;

	if (flags & OBJ_KEY) {
		name = (const char *) obj;
	} else {
		name = sccp_line_name((const struct sccp_line *) obj);
	}

	return ast_str_hash(name);
}

static int sccp_line_cmp(void *obj, void *arg, int flags)
{
	struct sccp_line *line = obj;
	const char *name;

	if (flags & OBJ_KEY) {
		name = (const char *) arg;
	} else {
		name = sccp_line_name((const struct sccp_line *) arg);
	}

	return strcmp(sccp_line_name(line), name) ? 0 : (CMP_MATCH | CMP_STOP);
}

struct sccp_device_registry *sccp_device_registry_create(void)
{
	struct sccp_device_registry *registry;

	registry = ast_calloc(1, sizeof(*registry));
	if (!registry) {
		return NULL;
	}

	registry->devices = ao2_container_alloc_options(AO2_ALLOC_OPT_LOCK_NOLOCK, SCCP_BUCKETS, sccp_device_hash, sccp_device_cmp);
	if (!registry->devices) {
		ast_free(registry);
		return NULL;
	}

	registry->lines = ao2_container_alloc_options(AO2_ALLOC_OPT_LOCK_NOLOCK, SCCP_BUCKETS, sccp_line_hash, sccp_line_cmp);
	if (!registry->lines) {
		ao2_ref(registry->devices, -1);
		ast_free(registry);
		return NULL;
	}

	ast_mutex_init(&registry->lock);

	return registry;
}

void sccp_device_registry_destroy(struct sccp_device_registry *registry)
{
	ao2_ref(registry->devices, -1);
	ao2_ref(registry->lines, -1);
	ast_mutex_destroy(&registry->lock);
	ast_free(registry);
}

static int add_device(struct sccp_device_registry *registry, struct sccp_device *device)
{
	return !ao2_link(registry->devices, device);
}

static void remove_device(struct sccp_device_registry *registry, struct sccp_device *device)
{
	ao2_unlink(registry->devices, device);
}

static int add_lines(struct sccp_device_registry *registry, struct sccp_device *device)
{
	unsigned int i;
	unsigned int n;

	n = sccp_device_line_count(device);
	for (i = 0; i < n; i++) {
		if (!ao2_link(registry->lines, sccp_device_line(device, i))) {
			goto error;
		}
	}

	return 0;

error:
	for (; i > 0; i--) {
		ao2_unlink(registry->lines, sccp_device_line(device, i - 1));
	}

	return -1;
}

static void remove_lines(struct sccp_device_registry *registry, struct sccp_device *device)
{
	unsigned int i;
	unsigned int n;

	n = sccp_device_line_count(device);
	for (i = 0; i < n; i++) {
		ao2_unlink(registry->lines, sccp_device_line(device, i));
	}
}

int sccp_device_registry_add(struct sccp_device_registry *registry, struct sccp_device *device)
{
	struct sccp_device *other_device;
	int ret = 0;

	if (!device) {
		ast_log(LOG_ERROR, "sccp device registry add failed: device is null\n");
		return -1;
	}

	ast_mutex_lock(&registry->lock);

	other_device = ao2_find(registry->devices, sccp_device_name(device), OBJ_KEY);
	if (other_device) {
		ao2_ref(other_device, -1);
		ret = SCCP_DEVICE_REGISTRY_ALREADY;
		goto unlock;
	}

	if (add_device(registry, device)) {
		ret = -1;
		goto unlock;
	}

	if (add_lines(registry, device)) {
		remove_device(registry, device);
		ret = -1;
		goto unlock;
	}

unlock:
	ast_mutex_unlock(&registry->lock);

	return ret;
}

void sccp_device_registry_remove(struct sccp_device_registry *registry, struct sccp_device *device)
{
	if (!device) {
		ast_log(LOG_ERROR, "sccp device registry remove failed: device is null\n");
		return;
	}

	ast_mutex_lock(&registry->lock);
	remove_lines(registry, device);
	remove_device(registry, device);
	ast_mutex_unlock(&registry->lock);
}

struct sccp_device *sccp_device_registry_find(struct sccp_device_registry *registry, const char *name)
{
	struct sccp_device *device;

	if (!name) {
		ast_log(LOG_ERROR, "registry find failed: name is null\n");
		return NULL;
	}

	ast_mutex_lock(&registry->lock);
	device = ao2_find(registry->devices, name, OBJ_KEY);
	ast_mutex_unlock(&registry->lock);

	return device;
}

struct sccp_line *sccp_device_registry_find_line(struct sccp_device_registry *registry, const char *name)
{
	struct sccp_line *line;

	if (!name) {
		ast_log(LOG_ERROR, "registry find line failed: name is null\n");
		return NULL;
	}

	ast_mutex_lock(&registry->lock);
	line = ao2_find(registry->lines, name, OBJ_KEY);
	ast_mutex_unlock(&registry->lock);

	return line;
}

/* XXX this make me thinking, with a large number of device, this must be
 *     like, really slow, since it will be called quite a few time
 */
char *sccp_device_registry_complete(struct sccp_device_registry *registry, const char *word, int state)
{
	struct ao2_iterator iter;
	struct sccp_device *device;
	char *result = NULL;
	int which = 0;
	int len;

	if (!word) {
		ast_log(LOG_ERROR, "registry complete failed: word is null\n");
		return NULL;
	}

	len = strlen(word);

	ast_mutex_lock(&registry->lock);

	iter = ao2_iterator_init(registry->devices, 0);
	while ((device = ao2_iterator_next(&iter))) {
		if (!strncasecmp(word, sccp_device_name(device), len) && ++which > state) {
			result = ast_strdup(sccp_device_name(device));
			ao2_ref(device, -1);
			break;
		}

		ao2_ref(device, -1);
	}
	ao2_iterator_destroy(&iter);

	ast_mutex_unlock(&registry->lock);

	return result;
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

	ast_mutex_lock(&registry->lock);

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
	iter = ao2_iterator_init(registry->devices, 0);
	while ((device = ao2_iterator_next(&iter))) {
		sccp_device_take_snapshot(device, &(*snapshots)[i++]);
		ao2_ref(device, -1);
	}
	ao2_iterator_destroy(&iter);

unlock:
	ast_mutex_unlock(&registry->lock);

	return ret;
}
