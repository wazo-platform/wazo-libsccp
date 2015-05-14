#include <asterisk.h>
#include <asterisk/acl.h>
#include <asterisk/astobj2.h>
#include <asterisk/config_options.h>
#include <asterisk/linkedlists.h>
#include <asterisk/strings.h>

#include "sccp.h"
#include "sccp_config.h"

#define DEVICE_CFG_NAME_GUEST "guest"

static void sccp_device_cfg_free_internal(struct sccp_device_cfg *device_cfg);
static void sccp_device_cfg_free_speeddials(struct sccp_device_cfg *device_cfg);
static void sccp_line_cfg_free_internal(struct sccp_line_cfg *line_cfg);
static void sccp_general_cfg_free_internal(struct sccp_general_cfg *general_cfg);
static struct sccp_speeddial_cfg *sccp_cfg_find_speeddial(struct sccp_cfg *cfg, const char *name);
static int pre_apply_config(void);

struct sccp_general_cfg_internal {
	int guest;
};

struct sccp_device_cfg_internal {
	char line_name[SCCP_LINE_NAME_MAX];
	AST_LIST_HEAD_NOLOCK(, device_cfg_speeddial) speeddials;
};

struct device_cfg_speeddial {
	char name[SCCP_SPEEDDIAL_NAME_MAX];
	/* temporary storage to hold the speeddial_cfg reference */
	struct sccp_speeddial_cfg *speeddial_cfg;
	AST_LIST_ENTRY(device_cfg_speeddial) list;
};

struct sccp_line_cfg_internal {
	int associated;
};

static void *sccp_speeddial_cfg_alloc(const char *category)
{
	struct sccp_speeddial_cfg *speeddial_cfg;

	speeddial_cfg = ao2_alloc_options(sizeof(*speeddial_cfg), NULL, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!speeddial_cfg) {
		return NULL;
	}

	ast_copy_string(speeddial_cfg->name, category, sizeof(speeddial_cfg->name));

	return speeddial_cfg;
}

static int sccp_speeddial_cfg_hash(const void *obj, int flags)
{
	const char *name;

	if (flags & OBJ_KEY) {
		name = obj;
	} else {
		name = ((const struct sccp_speeddial_cfg *) obj)->name;
	}

	return ast_str_hash(name);
}

static int sccp_speeddial_cfg_cmp(void *obj, void *arg, int flags)
{
	struct sccp_speeddial_cfg *speeddial_cfg = obj;
	const char *name;

	if (flags & OBJ_KEY) {
		name = arg;
	} else {
		name = ((const struct sccp_speeddial_cfg *) arg)->name;
	}

	return strcmp(speeddial_cfg->name, name) ? 0 : (CMP_MATCH | CMP_STOP);
}

static void *sccp_speeddial_cfg_find(struct ao2_container *container, const char *category)
{
	return ao2_find(container, category, OBJ_KEY);
}

static void sccp_line_cfg_destructor(void *obj)
{
	struct sccp_line_cfg *line_cfg = obj;

	sccp_line_cfg_free_internal(line_cfg);
	ast_variables_destroy(line_cfg->chanvars);
	ast_unref_namedgroups(line_cfg->named_callgroups);
	ast_unref_namedgroups(line_cfg->named_pickupgroups);
	ao2_ref(line_cfg->caps, -1);
}

static void *sccp_line_cfg_alloc(const char *category)
{
	struct sccp_line_cfg *line_cfg;
	struct sccp_line_cfg_internal *internal;
	struct ast_format_cap *caps;

	internal = ast_calloc(1, sizeof(*internal));
	if (!internal) {
		return NULL;
	}

	caps = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!caps) {
		ast_free(internal);
		return NULL;
	}

	line_cfg = ao2_alloc_options(sizeof(*line_cfg), sccp_line_cfg_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!line_cfg) {
		ao2_ref(caps, -1);
		ast_free(internal);
		return NULL;
	}

	ast_copy_string(line_cfg->name, category, sizeof(line_cfg->name));
	line_cfg->caps = caps;
	line_cfg->chanvars = NULL;
	line_cfg->callgroups = 0;
	line_cfg->pickupgroups = 0;
	line_cfg->named_callgroups = NULL;
	line_cfg->named_pickupgroups = NULL;
	line_cfg->internal = internal;
	line_cfg->internal->associated = 0;

	return line_cfg;
}

static void sccp_line_cfg_free_internal(struct sccp_line_cfg *line_cfg)
{
	if (!line_cfg->internal) {
		return;
	}

	ast_free(line_cfg->internal);
	line_cfg->internal = NULL;
}

static int sccp_line_cfg_hash(const void *obj, int flags)
{
	const char *name;

	if (flags & OBJ_KEY) {
		name = obj;
	} else {
		name = ((const struct sccp_line_cfg *) obj)->name;
	}

	return ast_str_hash(name);
}

static int sccp_line_cfg_cmp(void *obj, void *arg, int flags)
{
	struct sccp_line_cfg *line_cfg = obj;
	const char *name;

	if (flags & OBJ_KEY) {
		name = arg;
	} else {
		name = ((const struct sccp_line_cfg *) arg)->name;
	}

	return strcmp(line_cfg->name, name) ? 0 : (CMP_MATCH | CMP_STOP);
}

static void *sccp_line_cfg_find(struct ao2_container *container, const char *category)
{
	return ao2_find(container, category, OBJ_KEY);
}

static void sccp_device_cfg_destructor(void *obj)
{
	struct sccp_device_cfg *device_cfg = obj;

	sccp_device_cfg_free_internal(device_cfg);
	sccp_device_cfg_free_speeddials(device_cfg);
	ao2_cleanup(device_cfg->line_cfg);
}

static void *sccp_device_cfg_alloc(const char *category)
{
	struct sccp_device_cfg *device_cfg;
	struct sccp_device_cfg_internal *internal;

	internal = ast_calloc(1, sizeof(*internal));
	if (!internal) {
		return NULL;
	}

	device_cfg = ao2_alloc_options(sizeof(*device_cfg), sccp_device_cfg_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!device_cfg) {
		ast_free(internal);
		return NULL;
	}

	ast_copy_string(device_cfg->name, category, sizeof(device_cfg->name));
	device_cfg->line_cfg = NULL;
	device_cfg->speeddial_count = 0;
	device_cfg->speeddials_cfg = NULL;
	device_cfg->internal = internal;
	device_cfg->internal->line_name[0] = '\0';
	AST_LIST_HEAD_INIT_NOLOCK(&device_cfg->internal->speeddials);

	return device_cfg;
}

static void sccp_device_cfg_free_speeddials(struct sccp_device_cfg *device_cfg)
{
	size_t i;

	if (!device_cfg->speeddials_cfg) {
		return;
	}

	for (i = 0; i < device_cfg->speeddial_count; i++) {
		ao2_ref(device_cfg->speeddials_cfg[i], -1);
	}

	ast_free(device_cfg->speeddials_cfg);
	device_cfg->speeddials_cfg = NULL;
}

static void sccp_device_cfg_free_internal(struct sccp_device_cfg *device_cfg)
{
	struct device_cfg_speeddial *device_sd;

	if (!device_cfg->internal) {
		return;
	}

	AST_LIST_TRAVERSE_SAFE_BEGIN(&device_cfg->internal->speeddials, device_sd, list) {
		AST_LIST_REMOVE_CURRENT(list);
		ast_free(device_sd);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	ast_free(device_cfg->internal);
	device_cfg->internal = NULL;
}

/*
 * The "internal" member must not have been freed.
 */
static int sccp_device_cfg_add_speeddial_name(struct sccp_device_cfg *device_cfg, const char *name)
{
	struct device_cfg_speeddial *device_sd;

	device_sd = ast_calloc(1, sizeof(*device_sd));
	if (!device_sd) {
		return -1;
	}

	ast_copy_string(device_sd->name, name, sizeof(device_sd->name));
	device_sd->speeddial_cfg = NULL;

	AST_LIST_INSERT_TAIL(&device_cfg->internal->speeddials, device_sd, list);

	return 0;
}

/*
 * The "internal" member must not have been freed.
 * The function must not have been called successfully for this device config
 */
static int sccp_device_cfg_build_line(struct sccp_device_cfg *device_cfg, struct sccp_cfg *cfg)
{
	struct sccp_line_cfg *line_cfg;

	if (ast_strlen_zero(device_cfg->internal->line_name)) {
		ast_log(LOG_ERROR, "invalid device %s: no line associated\n", device_cfg->name);
		return -1;
	}

	line_cfg = sccp_cfg_find_line(cfg, device_cfg->internal->line_name);
	if (!line_cfg) {
		ast_log(LOG_ERROR, "invalid device %s: unknown line %s\n", device_cfg->name, device_cfg->internal->line_name);
		return -1;
	}

	if (line_cfg->internal->associated) {
		ast_log(LOG_ERROR, "invalid device %s: line %s is already associated\n", device_cfg->name, line_cfg->name);
		ao2_ref(line_cfg, -1);
		return -1;
	}

	device_cfg->line_cfg = line_cfg;
	line_cfg->internal->associated = 1;

	return 0;
}

/*
 * The "internal" member must not have been freed.
 * The function must not have been called successfully for this device config
 */
static int sccp_device_cfg_build_speeddials(struct sccp_device_cfg *device_cfg, struct sccp_cfg *cfg)
{
	struct device_cfg_speeddial *device_sd;
	size_t i;
	size_t count = 0;

	AST_LIST_TRAVERSE(&device_cfg->internal->speeddials, device_sd, list) {
		device_sd->speeddial_cfg = sccp_cfg_find_speeddial(cfg, device_sd->name);
		if (!device_sd->speeddial_cfg) {
			ast_log(LOG_WARNING, "invalid device %s: unknown speeddial %s\n", device_cfg->name, device_sd->name);
			continue;
		}

		count++;
	}

	if (!count) {
		return 0;
	}

	device_cfg->speeddials_cfg = ast_calloc(count, sizeof(*device_cfg->speeddials_cfg));
	if (!device_cfg->speeddials_cfg) {
		AST_LIST_TRAVERSE(&device_cfg->internal->speeddials, device_sd, list) {
			ao2_cleanup(device_sd->speeddial_cfg);
		}

		return -1;
	}

	i = 0;
	AST_LIST_TRAVERSE(&device_cfg->internal->speeddials, device_sd, list) {
		if (!device_sd->speeddial_cfg) {
			continue;
		}

		device_cfg->speeddials_cfg[i] = device_sd->speeddial_cfg;
		i++;
	}

	device_cfg->speeddial_count = count;

	return 0;
}

static int sccp_device_cfg_hash(const void *obj, int flags)
{
	const char *name;

	if (flags & OBJ_KEY) {
		name = obj;
	} else {
		name = ((const struct sccp_device_cfg *) obj)->name;
	}

	return ast_str_hash(name);
}

static int sccp_device_cfg_cmp(void *obj, void *arg, int flags)
{
	struct sccp_device_cfg *device_cfg = obj;
	const char *name;

	if (flags & OBJ_KEY) {
		name = arg;
	} else {
		name = ((const struct sccp_device_cfg *) arg)->name;
	}

	return strcmp(device_cfg->name, name) ? 0 : (CMP_MATCH | CMP_STOP);
}

static void *sccp_device_cfg_find(struct ao2_container *container, const char *category)
{
	return ao2_find(container, category, OBJ_KEY);
}

static void sccp_general_cfg_destructor(void *obj)
{
	struct sccp_general_cfg *general_cfg = obj;

	sccp_general_cfg_free_internal(general_cfg);
	ao2_cleanup(general_cfg->guest_device_cfg);
}

static struct sccp_general_cfg *sccp_general_cfg_alloc(void)
{
	struct sccp_general_cfg *general_cfg;
	struct sccp_general_cfg_internal *internal;

	internal = ast_calloc(1, sizeof(*internal));
	if (!internal) {
		return NULL;
	}

	general_cfg = ao2_alloc_options(sizeof(*general_cfg), sccp_general_cfg_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!general_cfg) {
		ast_free(internal);
		return NULL;
	}

	general_cfg->guest_device_cfg = NULL;
	general_cfg->internal = internal;
	general_cfg->internal->guest = 0;

	return general_cfg;
}

static void sccp_general_cfg_free_internal(struct sccp_general_cfg *general_cfg)
{
	if (!general_cfg->internal) {
		return;
	}

	ast_free(general_cfg->internal);
	general_cfg->internal = NULL;
}

static void sccp_cfg_destructor(void *obj)
{
	struct sccp_cfg *cfg = obj;

	ao2_cleanup(cfg->general_cfg);
	ao2_cleanup(cfg->devices_cfg);
	ao2_cleanup(cfg->lines_cfg);
	ao2_cleanup(cfg->speeddials_cfg);
}

static void *sccp_cfg_alloc(void)
{
	struct sccp_cfg *cfg;
	struct sccp_general_cfg *general_cfg = NULL;
	struct ao2_container *devices_cfg = NULL;
	struct ao2_container *lines_cfg = NULL;
	struct ao2_container *speeddials_cfg = NULL;

	general_cfg = sccp_general_cfg_alloc();
	if (!general_cfg) {
		goto error;
	}

	devices_cfg = ao2_container_alloc_options(AO2_ALLOC_OPT_LOCK_NOLOCK, SCCP_BUCKETS, sccp_device_cfg_hash, sccp_device_cfg_cmp);
	if (!devices_cfg) {
		goto error;
	}

	lines_cfg = ao2_container_alloc_options(AO2_ALLOC_OPT_LOCK_NOLOCK, SCCP_BUCKETS, sccp_line_cfg_hash, sccp_line_cfg_cmp);
	if (!lines_cfg) {
		goto error;
	}

	speeddials_cfg = ao2_container_alloc_options(AO2_ALLOC_OPT_LOCK_NOLOCK, SCCP_BUCKETS, sccp_speeddial_cfg_hash, sccp_speeddial_cfg_cmp);
	if (!speeddials_cfg) {
		goto error;
	}

	cfg = ao2_alloc_options(sizeof(*cfg), sccp_cfg_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!cfg) {
		goto error;
	}

	cfg->general_cfg = general_cfg;
	cfg->devices_cfg = devices_cfg;
	cfg->lines_cfg = lines_cfg;
	cfg->speeddials_cfg = speeddials_cfg;

	return cfg;

error:
	ao2_cleanup(general_cfg);
	ao2_cleanup(devices_cfg);
	ao2_cleanup(lines_cfg);
	ao2_cleanup(speeddials_cfg);

	return NULL;
}

static struct sccp_speeddial_cfg *sccp_cfg_find_speeddial(struct sccp_cfg *cfg, const char *name)
{
	return ao2_find(cfg->speeddials_cfg, name, OBJ_KEY);
}

static struct aco_type speeddial_type = {
	.type = ACO_ITEM,
	.name = "speeddial",
	.category_match = ACO_BLACKLIST,
	.category = "^general$",
	.matchfield = "type",
	.matchvalue = "speeddial",
	.item_alloc = sccp_speeddial_cfg_alloc,
	.item_find = sccp_speeddial_cfg_find,
	.item_offset = offsetof(struct sccp_cfg, speeddials_cfg),
};

static struct aco_type *speeddial_types[] = ACO_TYPES(&speeddial_type);

static struct aco_type line_type = {
	.type = ACO_ITEM,
	.name = "line",
	.category_match = ACO_BLACKLIST,
	.category = "^general$",
	.matchfield = "type",
	.matchvalue = "line",
	.item_alloc = sccp_line_cfg_alloc,
	.item_find = sccp_line_cfg_find,
	.item_offset = offsetof(struct sccp_cfg, lines_cfg),
};

static struct aco_type *line_types[] = ACO_TYPES(&line_type);

static struct aco_type device_type = {
	.type = ACO_ITEM,
	.name = "device",
	.category_match = ACO_BLACKLIST,
	.category = "^general$",
	.matchfield = "type",
	.matchvalue = "device",
	.item_alloc = sccp_device_cfg_alloc,
	.item_find = sccp_device_cfg_find,
	.item_offset = offsetof(struct sccp_cfg, devices_cfg),
};

static struct aco_type *device_types[] = ACO_TYPES(&device_type);

static struct aco_type general_type = {
	.type = ACO_GLOBAL,
	.name = "general",
	.category_match = ACO_WHITELIST,
	.category = "^general$",
	.item_offset = offsetof(struct sccp_cfg, general_cfg),
};

static struct aco_type *general_types[] = ACO_TYPES(&general_type);

static struct aco_file sccp_conf = {
	.filename = "sccp.conf",
	.types = ACO_TYPES(&speeddial_type, &line_type, &device_type, &general_type),
};

static AO2_GLOBAL_OBJ_STATIC(global_cfg);

CONFIG_INFO_STANDARD(cfg_info, global_cfg, sccp_cfg_alloc,
	.files = ACO_FILES(&sccp_conf),
	.pre_apply_config = pre_apply_config,
);

static int cb_pre_apply_device_cfg(void *obj, void *arg, int flags)
{
	struct sccp_device_cfg *device_cfg = obj;
	struct sccp_cfg *cfg = arg;

	if (sccp_device_cfg_build_line(device_cfg, cfg)) {
		return CMP_MATCH;
	}

	if (sccp_device_cfg_build_speeddials(device_cfg, cfg)) {
		return CMP_MATCH;
	}

	sccp_device_cfg_free_internal(device_cfg);

	return 0;
}

static void pre_apply_devices_cfg(struct sccp_cfg *cfg)
{
	ao2_callback(cfg->devices_cfg, OBJ_NODATA | OBJ_MULTIPLE | OBJ_UNLINK, cb_pre_apply_device_cfg, cfg);
}

static int cb_pre_apply_line_cfg(void *obj, void *arg, int flags)
{
	struct sccp_line_cfg *line_cfg = obj;

	if (!line_cfg->internal->associated) {
		ast_log(LOG_ERROR, "invalid line %s: not associated to any device\n", line_cfg->name);
		return CMP_MATCH;
	}

	sccp_line_cfg_free_internal(line_cfg);

	return 0;
}

static void pre_apply_lines_cfg(struct sccp_cfg *cfg)
{
	ao2_callback(cfg->lines_cfg, OBJ_NODATA | OBJ_MULTIPLE | OBJ_UNLINK, cb_pre_apply_line_cfg, NULL);
}

static void pre_apply_general_cfg(struct sccp_cfg *cfg)
{
	struct sccp_general_cfg *general_cfg = cfg->general_cfg;
	struct sccp_device_cfg *device_cfg;

	device_cfg = sccp_cfg_find_device(cfg, DEVICE_CFG_NAME_GUEST);
	if (device_cfg) {
		/* unlink the guest device and its line */
		ao2_unlink(cfg->devices_cfg, device_cfg);
		ao2_unlink(cfg->lines_cfg, device_cfg->line_cfg);

		if (general_cfg->internal->guest) {
			general_cfg->guest_device_cfg = device_cfg;
		} else {
			ao2_ref(device_cfg, -1);
		}
	} else {
		if (general_cfg->internal->guest) {
			ast_log(LOG_WARNING, "invalid config: guest is enabled but no device \"%s\" is defined\n", DEVICE_CFG_NAME_GUEST);
		}
	}

	sccp_general_cfg_free_internal(general_cfg);
}

static int pre_apply_config(void)
{
	struct sccp_cfg *cfg = aco_pending_config(&cfg_info);

	pre_apply_devices_cfg(cfg);
	pre_apply_lines_cfg(cfg);
	pre_apply_general_cfg(cfg);

	return 0;
}

static int general_cfg_guest_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_general_cfg *general_cfg = obj;

	general_cfg->internal->guest = ast_true(var->value);

	return 0;
}

static int device_cfg_line_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_device_cfg *device_cfg = obj;

	ast_copy_string(device_cfg->internal->line_name, var->value, sizeof(device_cfg->internal->line_name));

	return 0;
}

static int device_cfg_speeddial_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_device_cfg *device_cfg = obj;

	return sccp_device_cfg_add_speeddial_name(device_cfg, var->value);
}

static int line_cfg_setvar_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_line_cfg *line_cfg = obj;
	struct ast_variable *new_var;
	char *name = ast_strdupa(var->value);
	char *val = strchr(name, '=');

	if (!val) {
		return -1;
	}

	*val++ = '\0';
	new_var = ast_variable_new(name, val, "");
	if (!new_var) {
		return -1;
	}

	new_var->next = line_cfg->chanvars;
	line_cfg->chanvars = new_var;

	return 0;
}

static int line_cfg_callgroup_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_line_cfg *line_cfg = obj;

	line_cfg->callgroups = ast_get_group(var->value);

	return 0;
}

static int line_cfg_pickupgroup_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_line_cfg *line_cfg = obj;

	line_cfg->pickupgroups = ast_get_group(var->value);

	return 0;
}
static int line_cfg_namedcallgroup_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_line_cfg *line_cfg = obj;

	line_cfg->named_callgroups = ast_get_namedgroups(var->value);

	return 0;
}

static int line_cfg_namedpickupgroup_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_line_cfg *line_cfg = obj;

	line_cfg->named_pickupgroups = ast_get_namedgroups(var->value);

	return 0;
}

static int line_cfg_tos_audio_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct sccp_line_cfg *line_cfg = obj;

	return ast_str2tos(var->value, &line_cfg->tos_audio);
}

int sccp_config_init(void)
{
	if (aco_info_init(&cfg_info)) {
		return -1;
	}

	/* general options */
	aco_option_register(&cfg_info, "authtimeout", ACO_EXACT, general_types, "5", OPT_INT_T, PARSE_IN_RANGE, FLDSET(struct sccp_general_cfg, authtimeout), 1, 60);
	aco_option_register_custom(&cfg_info, "guest", ACO_EXACT, general_types, "no", general_cfg_guest_handler, 0);

	/* device options */
	aco_option_register(&cfg_info, "type", ACO_EXACT, device_types, NULL, OPT_NOOP_T, 0, 0);
	aco_option_register(&cfg_info, "dateformat", ACO_EXACT, device_types, "D/M/Y", OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_device_cfg, dateformat));
	aco_option_register(&cfg_info, "voicemail", ACO_EXACT, device_types, NULL, OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_device_cfg, voicemail));
	aco_option_register(&cfg_info, "vmexten", ACO_EXACT, device_types, "*98", OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_device_cfg, vmexten));
	aco_option_register(&cfg_info, "keepalive", ACO_EXACT, device_types, "10", OPT_INT_T, PARSE_IN_RANGE, FLDSET(struct sccp_device_cfg, keepalive), 1, 600);
	aco_option_register(&cfg_info, "dialtimeout", ACO_EXACT, device_types, "2", OPT_INT_T, PARSE_IN_RANGE, FLDSET(struct sccp_device_cfg, dialtimeout), 1, 60);
	aco_option_register(&cfg_info, "timezone", ACO_EXACT, device_types, NULL, OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_device_cfg, timezone));
	aco_option_register_custom(&cfg_info, "line", ACO_EXACT, device_types, NULL, device_cfg_line_handler, 0);
	aco_option_register_custom(&cfg_info, "speeddial", ACO_EXACT, device_types, NULL, device_cfg_speeddial_handler, 0);

	/* line options */
	aco_option_register(&cfg_info, "type", ACO_EXACT, line_types, NULL, OPT_NOOP_T, 0, 0);
	aco_option_register(&cfg_info, "cid_name", ACO_EXACT, line_types, NULL, OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_line_cfg, cid_name));
	aco_option_register(&cfg_info, "cid_num", ACO_EXACT, line_types, NULL, OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_line_cfg, cid_num));
	aco_option_register_custom(&cfg_info, "setvar", ACO_EXACT, line_types, NULL, line_cfg_setvar_handler, 0);
	aco_option_register(&cfg_info, "context", ACO_EXACT, line_types, "default", OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_line_cfg, context));
	aco_option_register(&cfg_info, "language", ACO_EXACT, line_types, "en_US", OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_line_cfg, language));
	aco_option_register(&cfg_info, "directmedia", ACO_EXACT, line_types, "no", OPT_BOOL_T, 1, FLDSET(struct sccp_line_cfg, directmedia));
	aco_option_register_custom(&cfg_info, "tos_audio", ACO_EXACT, line_types, "EF", line_cfg_tos_audio_handler, 0);
	aco_option_register(&cfg_info, "disallow", ACO_EXACT, line_types, NULL, OPT_CODEC_T, 0, FLDSET(struct sccp_line_cfg, codec_pref, caps));
	aco_option_register(&cfg_info, "allow", ACO_EXACT, line_types, "ulaw,alaw", OPT_CODEC_T, 1, FLDSET(struct sccp_line_cfg, codec_pref, caps));
	aco_option_register_custom(&cfg_info, "callgroup", ACO_EXACT, line_types, NULL, line_cfg_callgroup_handler, 0);
	aco_option_register_custom(&cfg_info, "pickupgroup", ACO_EXACT, line_types, NULL, line_cfg_pickupgroup_handler, 0);
	aco_option_register_custom(&cfg_info, "namedcallgroup", ACO_EXACT, line_types, NULL, line_cfg_namedcallgroup_handler, 0);
	aco_option_register_custom(&cfg_info, "namedpickupgroup", ACO_EXACT, line_types, NULL, line_cfg_namedpickupgroup_handler, 0);

	/* speeddial options */
	aco_option_register(&cfg_info, "type", ACO_EXACT, speeddial_types, NULL, OPT_NOOP_T, 0, 0);
	aco_option_register(&cfg_info, "label", ACO_EXACT, speeddial_types, NULL, OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_speeddial_cfg, label));
	aco_option_register(&cfg_info, "extension", ACO_EXACT, speeddial_types, NULL, OPT_CHAR_ARRAY_T, 0, CHARFLDSET(struct sccp_speeddial_cfg, extension));
	aco_option_register(&cfg_info, "blf", ACO_EXACT, speeddial_types, "no", OPT_BOOL_T, 1, FLDSET(struct sccp_speeddial_cfg, blf));

	return 0;
}

static int sccp_config_load_internal(int reload)
{
	if (aco_process_config(&cfg_info, reload) == ACO_PROCESS_ERROR) {
		return -1;
	}

	return 0;
}

int sccp_config_load(void)
{
	return sccp_config_load_internal(0);
}

int sccp_config_reload(void)
{
	return sccp_config_load_internal(1);
}

void sccp_config_destroy(void)
{
	aco_info_destroy(&cfg_info);
	ao2_global_obj_release(global_cfg);
}

struct sccp_cfg *sccp_config_get(void)
{
	return ao2_global_obj_ref(global_cfg);
}

struct sccp_device_cfg *sccp_cfg_find_device(struct sccp_cfg *cfg, const char *name)
{
	return ao2_find(cfg->devices_cfg, name, OBJ_KEY);
}

struct sccp_device_cfg *sccp_cfg_find_device_or_guest(struct sccp_cfg *cfg, const char *name)
{
	struct sccp_device_cfg *device_cfg;

	device_cfg = sccp_cfg_find_device(cfg, name);
	if (device_cfg) {
		return device_cfg;
	}

	device_cfg = cfg->general_cfg->guest_device_cfg;
	if (device_cfg) {
		ao2_ref(device_cfg, +1);
		return device_cfg;
	}

	return NULL;
}

struct sccp_line_cfg *sccp_cfg_find_line(struct sccp_cfg *cfg, const char *name)
{
	return ao2_find(cfg->lines_cfg, name, OBJ_KEY);
}
