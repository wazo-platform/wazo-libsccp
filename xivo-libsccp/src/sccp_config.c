#include <asterisk.h>

#include "sccp_device.h"
#include "sccp_config.h"

#include "../config.h"

#define SCCP_DEFAULT_KEEPALIVE 10
#define SCCP_DEFAULT_AUTH_TIMEOUT 5
#define SCCP_DEFAULT_DIAL_TIMEOUT 1

static int parse_config_general(struct ast_config *cfg, struct sccp_configs *sccp_cfg);
static int parse_config_devices(struct ast_config *cfg, struct sccp_configs *sccp_cfg);
static int parse_config_lines(struct ast_config *cfg, struct sccp_configs *sccp_cfg);
static int parse_config_speeddials(struct ast_config *cfg, struct sccp_configs *sccp_cfg);
static void initialize_device(struct sccp_device *device, const char *name);
static void initialize_line(struct sccp_line *line, uint32_t instance, struct sccp_device *device);
static void initialize_speeddial(struct sccp_speeddial *speeddial, uint32_t index, uint32_t instance, struct sccp_device *device);
static struct ast_variable *add_var(const char *buf, struct ast_variable *list);

int sccp_config_init(struct sccp_configs **config)
{
	struct sccp_configs *new_config = NULL;

	if (config == NULL) {
		ast_log(LOG_ERROR, "NULL address supplied to init SCCP configuration\n");
		return -1;
	}

	new_config = ast_calloc(1, sizeof(*new_config));
	if (new_config == NULL) {
		ast_log(LOG_ERROR, "SCCP configuration memory allocation failed\n");
		return -1;
	}

	AST_RWLIST_HEAD_INIT(&new_config->list_device);
	AST_RWLIST_HEAD_INIT(&new_config->list_line);

	*config = new_config;

	return 0;
}

int sccp_config_destroy(struct sccp_configs **config)
{
	struct sccp_configs *to_destroy = NULL;

	if (config == NULL) {
		ast_log(LOG_ERROR, "NULL address supplied to destroy SCCP configuration\n");
		return -1;
	}

	to_destroy = *config;
	if (to_destroy == NULL) {
		return 0;
	}

	AST_RWLIST_HEAD_DESTROY(&to_destroy->list_device);
	AST_RWLIST_HEAD_DESTROY(&to_destroy->list_line);
	ast_free(to_destroy);

	*config = NULL;

	return 0;
}

int sccp_config_load(struct sccp_configs *sccp_cfg, const char *config_file)
{
	struct ast_config *cfg = NULL;
	struct ast_flags config_flags = { 0 };

	ast_log(LOG_NOTICE, "Configuring sccp from %s...\n", config_file);

	cfg = ast_config_load(config_file, config_flags);
	if (!cfg) {
		ast_log(LOG_ERROR, "Unable to load configuration file '%s'\n", config_file);
		return -1;
	}

	parse_config_general(cfg, sccp_cfg);
	parse_config_lines(cfg, sccp_cfg);
	parse_config_speeddials(cfg, sccp_cfg);
	parse_config_devices(cfg, sccp_cfg);

	ast_config_destroy(cfg);

	return 0;
}

void sccp_config_unload(struct sccp_configs *sccp_cfg)
{
	struct sccp_device *device_itr = NULL;
	struct sccp_line *line_itr = NULL;

	AST_RWLIST_WRLOCK(&sccp_cfg->list_device);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&sccp_cfg->list_device, device_itr, list) {
		ast_mutex_destroy(&device_itr->lock);
		AST_RWLIST_HEAD_DESTROY(&device_itr->lines);
		AST_RWLIST_HEAD_DESTROY(&device_itr->speeddials);
		AST_RWLIST_REMOVE_CURRENT(list);
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&sccp_cfg->list_device);

	AST_RWLIST_WRLOCK(&sccp_cfg->list_line);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&sccp_cfg->list_line, line_itr, list) {
		AST_RWLIST_HEAD_DESTROY(&line_itr->subchans);
		AST_RWLIST_REMOVE_CURRENT(list);
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&sccp_cfg->list_line);
}

void destroy_device_config(struct sccp_configs *sccp_cfg, struct sccp_device *device)
{
	struct sccp_line *line_itr = NULL;
	struct sccp_subchannel *subchan_itr = NULL;
	struct sccp_speeddial *speeddial_itr = NULL;

	AST_RWLIST_WRLOCK(&device->speeddials);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&device->speeddials, speeddial_itr, list_per_device) {

		AST_RWLIST_REMOVE_CURRENT(list_per_device);
		AST_LIST_REMOVE(&sccp_cfg->list_speeddial, speeddial_itr, list);
		free(speeddial_itr);
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&device->speeddials);

	AST_RWLIST_WRLOCK(&device->lines);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&device->lines, line_itr, list_per_device) {

		AST_RWLIST_WRLOCK(&line_itr->subchans);
		AST_RWLIST_TRAVERSE_SAFE_BEGIN(&line_itr->subchans, subchan_itr, list) {
			AST_LIST_REMOVE_CURRENT(list);
		}
		AST_RWLIST_TRAVERSE_SAFE_END;
		AST_RWLIST_UNLOCK(&line_itr->subchans);

		AST_LIST_REMOVE_CURRENT(list_per_device);
		AST_LIST_REMOVE(&sccp_cfg->list_line, line_itr, list);
		free(line_itr);
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&device->lines);

	ast_mutex_destroy(&device->lock);
	ast_cond_destroy(&device->lookup_cond);
	ast_format_cap_destroy(device->capabilities);
	free(device);
}

static void initialize_speeddial(struct sccp_speeddial *speeddial, uint32_t index, uint32_t instance, struct sccp_device *device)
{
	if (speeddial == NULL) {
		ast_log(LOG_WARNING, "speeddial is NULL\n");
		return;
	}

	if (device == NULL) {
		ast_log(LOG_WARNING, "device is NULL\n");
		return;
	}

	speeddial->index = index;
	speeddial->instance = instance;
	speeddial->device = device;
}

static void initialize_device(struct sccp_device *device, const char *name)
{
	if (device == NULL) {
		ast_log(LOG_WARNING, "device is NULL\n");
		return;
	}

	if (name == NULL) {
		ast_log(LOG_WARNING, "name is NULL\n");
		return;
	}

	ast_mutex_init(&device->lock);
	ast_cond_init(&device->lookup_cond, NULL);
	ast_copy_string(device->name, name, sizeof(device->name));

	device->voicemail[0] = '\0';
	device->exten[0] = '\0';
	device->mwi_event_sub = NULL;
	device->lookup = 0;
	device->autoanswer = 0;
	device->registered = DEVICE_REGISTERED_FALSE;
	device->session = NULL;
	device->line_count = 0;
	device->speeddial_count = 0;
	device->capabilities = ast_format_cap_alloc_nolock();

	AST_RWLIST_HEAD_INIT(&device->lines);
	AST_RWLIST_HEAD_INIT(&device->speeddials);
}

static void initialize_line(struct sccp_line *line, uint32_t instance, struct sccp_device *device)
{
	if (line == NULL) {
		ast_log(LOG_WARNING, "line is NULL\n");
		return;
	}

	if (device == NULL) {
		ast_log(LOG_WARNING, "device is NULL\n");
		return;
	}

	line->state = SCCP_ONHOOK;
	line->instance = instance;
	line->device = device;
	line->serial_callid = 1;
	line->active_subchan = NULL;
	line->callfwd = SCCP_CFWD_INACTIVE;
	AST_RWLIST_HEAD_INIT(&line->subchans);

	/* set the device default line */
	if (device->default_line == NULL) {
		device->default_line = line;
	}
}

static int parse_config_devices(struct ast_config *cfg, struct sccp_configs *sccp_cfg)
{
	struct ast_variable *var = NULL;
	struct sccp_device *device, *device_itr = NULL;
	struct sccp_line *line_itr = NULL;
	struct sccp_speeddial *speeddial_itr = NULL;
	char *category = NULL;
	int duplicate = 0;
	int found_line = 0;
	int found_speeddial = 0;
	int err = 0;
	int line_instance = 1;
	int sd_index = 1;

	category = ast_category_browse(cfg, "devices");
	/* handle each device */
	while (category != NULL && strcasecmp(category, "general") && strcasecmp(category, "lines")) {

		/* no duplicates allowed */
		AST_RWLIST_RDLOCK(&sccp_cfg->list_device);
		AST_RWLIST_TRAVERSE(&sccp_cfg->list_device, device_itr, list) {
			if (!strcasecmp(category, device_itr->name)) {
				ast_log(LOG_DEBUG, "Device [%s] already exist, instance ignored\n", category);
				duplicate = 1;
				break;
			}
		}
		AST_RWLIST_UNLOCK(&sccp_cfg->list_device);

		if (!duplicate) {

			/* create the new device */
			device = ast_calloc(1, sizeof(struct sccp_device));
			initialize_device(device, category);

			/* get every settings for a particular device */
			for (var = ast_variable_browse(cfg, category); var != NULL; var = var->next) {

				if (!strcasecmp(var->name, "voicemail")) {
					ast_copy_string(device->voicemail, var->value, sizeof(device->voicemail));
				}

				if (!strcasecmp(var->name, "line")) {

					/* we are looking for the line that match with 'var->name' */
					AST_RWLIST_RDLOCK(&sccp_cfg->list_line);
					AST_RWLIST_TRAVERSE(&sccp_cfg->list_line, line_itr, list) {
						if (!strcasecmp(var->value, line_itr->name)) {
							/* We found a line */
							found_line = 1;

							if (line_itr->device != NULL) {
								ast_log(LOG_ERROR, "Line [%s] is already attached to device [%s]\n",
									line_itr->name, line_itr->device->name);
							} else {
								/* link the line to the device */
								AST_RWLIST_WRLOCK(&device->lines);
								AST_RWLIST_INSERT_HEAD(&device->lines, line_itr, list_per_device);
								AST_RWLIST_UNLOCK(&device->lines);
								device->line_count++;
								initialize_line(line_itr, line_instance++, device);
							}
						}
					}
					AST_RWLIST_UNLOCK(&sccp_cfg->list_line);

					if (!found_line) {
						err = 1;
						ast_log(LOG_WARNING, "Can't attach invalid line [%s] to device [%s]\n",
							var->value, category);
					}
				}
				found_line = 0;

				if (!strcasecmp(var->name, "speeddial")) {

					/* we are looking for the speeddial that match with 'var->name' */
					AST_RWLIST_RDLOCK(&sccp_cfg->list_speeddial);
					AST_RWLIST_TRAVERSE(&sccp_cfg->list_speeddial, speeddial_itr, list) {
						if (!strcasecmp(var->value, speeddial_itr->name)) {
							/* We found a speeddial */
							found_speeddial = 1;

							if (speeddial_itr->device != NULL) {
								ast_log(LOG_ERROR, "Speeddial [%s] is already attached to device [%s]\n",
									speeddial_itr->name, speeddial_itr->device->name);
							} else {
								/* link the speeddial to the device */
								AST_RWLIST_WRLOCK(&device->speeddials);
								AST_RWLIST_INSERT_HEAD(&device->speeddials, speeddial_itr, list_per_device);
								AST_RWLIST_UNLOCK(&device->speeddials);
								device->speeddial_count++;
								initialize_speeddial(speeddial_itr, sd_index++, line_instance++, device);
							}
						}
					}
					AST_RWLIST_UNLOCK(&sccp_cfg->list_speeddial);

					if (!found_speeddial) {
						ast_log(LOG_WARNING, "Can't attache invalid speeddial [%s] to device [%s]\n",
							var->value, category);
					}

				}
				found_speeddial = 0;
			}

			/* Add the device to the list only if no error occured and
			 * at least one line is present */
			if (!err && (device->line_count > 0 || device->speeddial_count > 0)) {
				AST_RWLIST_WRLOCK(&sccp_cfg->list_device);
				AST_RWLIST_INSERT_HEAD(&sccp_cfg->list_device, device, list);
				AST_RWLIST_UNLOCK(&sccp_cfg->list_device);
			}
			else {
				ast_free(device);
			}
			err = 0;
		}

		duplicate = 0;
		line_instance = 1;
		sd_index = 1;
		category = ast_category_browse(cfg, category);
	}

	return 0;
}

static int parse_config_speeddials(struct ast_config *cfg, struct sccp_configs *sccp_cfg)
{
	struct ast_variable *var;
	struct sccp_speeddial *speeddial, *speeddial_itr;
	char *category;
	int duplicate = 0;

	category = ast_category_browse(cfg, "speeddials");
	/* handle each speeddial */
	while (category != NULL && strcasecmp(category, "general")
				&& strcasecmp(category, "devices")
				&& strcasecmp(category, "lines")) {

		/* no duplicates allowed */
		AST_RWLIST_RDLOCK(&sccp_cfg->list_speeddial);
		AST_RWLIST_TRAVERSE(&sccp_cfg->list_speeddial, speeddial_itr, list) {
			if (!strcasecmp(category, speeddial_itr->name)) {
				ast_log(LOG_WARNING, "Speeddial [%s] already exist, speeddial ignored\n", category);
				duplicate = 1;
				break;
			}
		}
		AST_RWLIST_UNLOCK(&sccp_cfg->list_speeddial);
		if (!duplicate) {
			speeddial = calloc(1, sizeof(struct sccp_speeddial));
			ast_copy_string(speeddial->name, category, sizeof(speeddial->name));

			AST_RWLIST_WRLOCK(&sccp_cfg->list_speeddial);
			AST_RWLIST_INSERT_HEAD(&sccp_cfg->list_speeddial, speeddial, list);
			AST_RWLIST_UNLOCK(&sccp_cfg->list_speeddial);

			for (var = ast_variable_browse(cfg, category); var != NULL; var = var->next) {

				if (!strcasecmp(var->name, "extension")) {
					ast_copy_string(speeddial->extension, var->value, sizeof(speeddial->extension));
				} else if (!strcasecmp(var->name, "label")) {
					ast_copy_string(speeddial->label, var->value, sizeof(speeddial->label));
				} else if (!strcasecmp(var->name, "blf")) {
					if (ast_true(var->value))
						speeddial->blf = 1;
				}
			}
		}
		duplicate = 0;
		category = ast_category_browse(cfg, category);
	}

	return 0;
}

static int parse_config_lines(struct ast_config *cfg, struct sccp_configs *sccp_cfg)
{
	struct ast_variable *var;
	struct sccp_line *line, *line_itr;
	char *category;
	int duplicate = 0;

	category = ast_category_browse(cfg, "lines");
	/* handle each lines */
	while (category != NULL && strcasecmp(category, "general")
				&& strcasecmp(category, "devices")
				&& strcasecmp(category, "speeddials")) {

		/* no duplicates allowed */
		AST_RWLIST_RDLOCK(&sccp_cfg->list_line);
		AST_RWLIST_TRAVERSE(&sccp_cfg->list_line, line_itr, list) {
			if (!strcasecmp(category, line_itr->name)) {
				ast_log(LOG_WARNING, "Line [%s] already exist, line ignored\n", category);
				duplicate = 1;
				break;
			}
		}
		AST_RWLIST_UNLOCK(&sccp_cfg->list_line);

		if (!duplicate) {
			/* configure a new line */
			line = ast_calloc(1, sizeof(struct sccp_line));
			ast_copy_string(line->name, category, sizeof(line->name));

			AST_RWLIST_WRLOCK(&sccp_cfg->list_line);
			AST_RWLIST_INSERT_HEAD(&sccp_cfg->list_line, line, list);
			AST_RWLIST_UNLOCK(&sccp_cfg->list_line);

			/* Default configuration */
			ast_copy_string(line->context, "default", sizeof(line->context));
			ast_copy_string(line->language, sccp_cfg->language, sizeof(line->language));

			for (var = ast_variable_browse(cfg, category); var != NULL; var = var->next) {

				if (!strcasecmp(var->name, "cid_num")) {
					ast_copy_string(line->cid_num, var->value, sizeof(line->cid_num));
				} else if (!strcasecmp(var->name, "cid_name")) {
					ast_copy_string(line->cid_name, var->value, sizeof(line->cid_name));
				} else if (!strcasecmp(var->name, "setvar")) {
					line->chanvars = add_var(var->value, line->chanvars);
				} else if (!strcasecmp(var->name, "language")) {
					ast_copy_string(line->language, var->value, sizeof(line->language));
				} else if (!strcasecmp(var->name, "context")) {
					ast_copy_string(line->context, var->value, sizeof(line->language));
				}
			}
		}
		duplicate = 0;
		category = ast_category_browse(cfg, category);
	}

	return 0;
}

static int parse_config_general(struct ast_config *cfg, struct sccp_configs *sccp_cfg)
{
	struct ast_variable *var = NULL;

	/* Do not parse it twice */
	if (sccp_cfg->set == 1) {
		return 0;
	}
	else {
		sccp_cfg->set = 1;
	}

	/* Default configuration */
	ast_copy_string(sccp_cfg->bindaddr, "0.0.0.0", sizeof(sccp_cfg->bindaddr));
	ast_copy_string(sccp_cfg->dateformat, "D.M.Y", sizeof(sccp_cfg->dateformat));
	ast_copy_string(sccp_cfg->context, "default", sizeof(sccp_cfg->context));
	ast_copy_string(sccp_cfg->language, "en_US", sizeof(sccp_cfg->language));
	ast_copy_string(sccp_cfg->vmexten, "*98", sizeof(sccp_cfg->vmexten));

	sccp_cfg->keepalive = SCCP_DEFAULT_KEEPALIVE;
	sccp_cfg->authtimeout = SCCP_DEFAULT_AUTH_TIMEOUT;
	sccp_cfg->dialtimeout = SCCP_DEFAULT_DIAL_TIMEOUT;
	sccp_cfg->directmedia = 0; /* off by default */

	/* Custom configuration and handle lower bound */
	for (var = ast_variable_browse(cfg, "general"); var != NULL; var = var->next) {

		if (!strcasecmp(var->name, "bindaddr")) {
			ast_copy_string(sccp_cfg->bindaddr, var->value, sizeof(sccp_cfg->bindaddr));
		} else if (!strcasecmp(var->name, "dateformat")) {
			ast_copy_string(sccp_cfg->dateformat, var->value, sizeof(sccp_cfg->dateformat));
		} else if (!strcasecmp(var->name, "keepalive")) {
			sccp_cfg->keepalive = atoi(var->value);
		} else if (!strcasecmp(var->name, "authtimeout")) {
			sccp_cfg->authtimeout = atoi(var->value);
			if (sccp_cfg->authtimeout < 10)
				sccp_cfg->authtimeout = SCCP_DEFAULT_AUTH_TIMEOUT;
		} else if (!strcasecmp(var->name, "dialtimeout")) {
			sccp_cfg->dialtimeout = atoi(var->value);
			if (sccp_cfg->dialtimeout <= 0)
				sccp_cfg->dialtimeout = SCCP_DEFAULT_DIAL_TIMEOUT;
		} else if (!strcasecmp(var->name, "context")) {
			ast_copy_string(sccp_cfg->context, var->value, sizeof(sccp_cfg->context));
		} else if (!strcasecmp(var->name, "language")) {
			ast_copy_string(sccp_cfg->language, var->value, sizeof(sccp_cfg->language));
		} else if (!strcasecmp(var->name, "vmexten")) {
			ast_copy_string(sccp_cfg->vmexten, var->value, sizeof(sccp_cfg->vmexten));
		} else if (!strcasecmp(var->name, "directmedia")) {
			sccp_cfg->directmedia = atoi(var->value);
		}
	}

	return 0;
}

static struct ast_variable *add_var(const char *buf, struct ast_variable *list)
{
	struct ast_variable *tmpvar = NULL;
	char *varname = ast_strdupa(buf), *varval = NULL;

	if ((varval = strchr(varname, '='))) {
		*varval++ = '\0';
		if ((tmpvar = ast_variable_new(varname, varval, ""))) {
			tmpvar->next = list;
			list = tmpvar;
		}
	}
	return list;
}
