
#include <asterisk.h>

ASTERISK_FILE_VERSION(__FILE__, "$Revision: $")

#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/utils.h>

#include "../config.h"

#include "sccp.h"
#include "message.h"
#include "device.h"

#ifndef AST_MODULE
#define AST_MODULE "chan_sccp"
#endif

static const char sccp_config[] = "skinny.conf";
struct sccp_configs sccp_cfg = {0}; /* global settings */

static int parse_config_devices(struct ast_config *cfg)
{
	struct ast_variable *var;
	struct sccp_device *device, *device_itr;
	struct sccp_line *line_itr;
	char *category;
	int duplicate = 0;
	int found_line = 0;
	int line_instance = 1;

	/* get the default settings for the devices */
	for (var = ast_variable_browse(cfg, "devices"); var != NULL; var = var->next) {
		ast_log(LOG_NOTICE, "var name {%s} value {%s} \n", var->name, var->value);	
	}

	category = ast_category_browse(cfg, "devices");
	/* handle eache devices */
	while (category != NULL && strcasecmp(category, "general") && strcasecmp(category, "lines")) {
		/* no duplicates allowed */
		AST_LIST_TRAVERSE(&list_device, device_itr, list) {
			if (!strcasecmp(category, device_itr->name)) {
				ast_log(LOG_WARNING, "Device [%s] already exist, instance ignored\n", category);
				duplicate = 1;
				break;
			}
		}

		if (!duplicate) {
			/* configure a new device */
			device = ast_calloc(1, sizeof(struct sccp_device));
			ast_copy_string(device->name, category, 80);
			AST_LIST_HEAD_INIT(&device->lines);

			AST_LIST_INSERT_HEAD(&list_device, device, list);

			/* get every settings for a particular device */
			for (var = ast_variable_browse(cfg, category); var != NULL; var = var->next) {
				if (!strcasecmp(var->name, "line")) {
					/* attach lines to the device */
					AST_LIST_TRAVERSE(&list_line, line_itr, list) {
						if (!strcasecmp(var->value, line_itr->name)) {
							/* we found a line */
							if (line_itr->device == NULL) {

								/* initialize the line instance */
								AST_LIST_INSERT_HEAD(&device->lines, line_itr, list_per_device);
								line_itr->state = SCCP_ONHOOK;
								line_itr->instance = line_instance++;
								device->line_count++;
								line_itr->device = device;
								line_itr->callid = 0;
								
								/* set the device default line */
								if (line_itr->instance == 1)
									device->default_line = line_itr;

							} else {
								ast_log(LOG_WARNING, "Line [%s] is already attach to device [%s]\n",
									line_itr->name, line_itr->device->name);
							}
							found_line = 1;
						}
					}
					if (!found_line) {
						ast_log(LOG_WARNING, "Can't attach invalid line [%s] to device [%s]\n",
							var->value, category);
					}
				}


				found_line = 0;
			}
		}
		duplicate = 0;
		line_instance = 1;
		category = ast_category_browse(cfg, category);
	}

	return 0;
}
		
static int parse_config_lines(struct ast_config *cfg)
{
	struct ast_variable *var;
	struct sccp_line *line, *line_itr;
	char *category;
	int duplicate = 0;

	/* get the default settings for the lines */
	for (var = ast_variable_browse(cfg, "lines"); var != NULL; var = var->next) {
		ast_log(LOG_NOTICE, "var name {%s} value {%s} \n", var->name, var->value);	
	}

	category = ast_category_browse(cfg, "lines");
	/* handle each lines */
	while (category != NULL && strcasecmp(category, "general") && strcasecmp(category, "devices")) {
		/* no duplicates allowed */
		AST_LIST_TRAVERSE(&list_line, line_itr, list) {
			if (!strcasecmp(category, line_itr->name)) {
				ast_log(LOG_WARNING, "Line [%s] already exist, line ignored\n", category);
				duplicate = 1;
				break;
			}
		}

		if (!duplicate) {
			/* configure a new line */
			line = ast_calloc(1, sizeof(struct sccp_line));
			ast_copy_string(line->name, category, 80);
	
			AST_LIST_INSERT_HEAD(&list_line, line, list);

			for (var = ast_variable_browse(cfg, category); var != NULL; var = var->next) {
				ast_log(LOG_NOTICE, "var name {%s} value {%s} \n", var->name, var->value);

				if (!strcasecmp(var->name, "cid_num")) {
					ast_copy_string(line->cid_num, var->value, sizeof(line->cid_num));
					continue;

				} else if (!strcasecmp(var->name, "cid_name")) {
					ast_copy_string(line->cid_name, var->value, sizeof(line->cid_name));
					continue;
				}
			}
		}

		duplicate = 0;
		category = ast_category_browse(cfg, category);
	}
	
	return 0;
}

static int parse_config_general(struct ast_config *cfg)
{
	struct ast_variable *var;	

	for (var = ast_variable_browse(cfg, "general"); var != NULL; var = var->next) {

		if (!strcasecmp(var->name, "bindaddr")) {
			sccp_cfg.bindaddr = ast_strdup(var->value);
			ast_log(LOG_NOTICE, "var name {%s} value {%s} \n", var->name, var->value);
			continue;

		} else if (!strcasecmp(var->name, "dateformat")) {
			ast_copy_string(sccp_cfg.dateformat, var->value, sizeof(sccp_cfg.dateformat));
			continue;

		} else if (!strcasecmp(var->name, "keepalive")) {
			sccp_cfg.keepalive = atoi(var->value);
			continue;
		
		} else if (!strcasecmp(var->name, "authtimeout")) {
			sccp_cfg.authtimeout = atoi(var->value);
			if (sccp_cfg.authtimeout < 10)
				sccp_cfg.authtimeout = SCCP_DEFAULT_AUTH_TIMEOUT;
			continue;
		}
	}

	return 0;
}

static int config_unload(void)
{
	ast_free(sccp_cfg.bindaddr);

	struct sccp_device *device_itr;
	struct sccp_line *line_itr;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&list_device, device_itr, list) {
		AST_LIST_REMOVE_CURRENT(&list_device, list);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&list_line, line_itr, list) {
		AST_LIST_REMOVE_CURRENT(&list_line, list);
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

static int config_load(void)
{
	struct ast_config *cfg;

	ast_log(LOG_NOTICE, "Configuring sccp from %s...\n", sccp_config);

	cfg = ast_config_load(sccp_config);
	if (!cfg) {
		ast_log(LOG_ERROR, "Unable to load configuration file '%s'\n", sccp_config);
		return -1;
	}

	parse_config_general(cfg);
	parse_config_lines(cfg);
	parse_config_devices(cfg);

	struct sccp_line *line_itr;
	struct sccp_device *device_itr;

	AST_LIST_TRAVERSE(&list_line, line_itr, list) {
		ast_log(LOG_NOTICE, "Line [%s] \n", line_itr->name);
	}

	AST_LIST_TRAVERSE(&list_device, device_itr, list) {
		ast_log(LOG_NOTICE, "Device [%s] : \n", device_itr->name);

		AST_LIST_TRAVERSE(&device_itr->lines, line_itr, list_per_device) {
			ast_log(LOG_NOTICE, "[%s] [%d]\n", line_itr->name, line_itr->instance);
		}
	}

	ast_config_destroy(cfg);

	return 0;
}

static int sccp_show_version(int fd, int argc, char *argv[])
{
	ast_cli(fd, "%s <%s>\n", PACKAGE_STRING, PACKAGE_BUGREPORT);

	return RESULT_SUCCESS;
}

static char show_version_usage[] = "Usage: sccp show version\n";

static struct ast_cli_entry cli_sccp[] = {
	{ { "sccp", "show", "version", NULL },
	sccp_show_version, "Show the version of the skinny channel",
	show_version_usage },
};

static int load_module(void)
{
	int ret = 0;
	ast_verbose("sccp channel loading...\n");

	/* Make sure we can register our sccp channel type
	if (ast_channel_register(&sccp_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel type 'sccp'\n");
		return AST_MODULE_LOAD_FAILURE;
	}*/

	ret = config_load();
	if (ret == -1) {
		/*ast_channel_unregister(&sccp_tech);*/
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_cli_register_multiple(cli_sccp, ARRAY_LEN(cli_sccp));

	ret = sccp_server_init();
	if (ret == -1) {
		/*ast_channel_unregister(&sccp_tech);*/
		ast_cli_unregister_multiple(cli_sccp, ARRAY_LEN(cli_sccp));
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_verbose("sccp channel unloading...\n");

	/*ast_channel_unregister(&sccp_tech);*/

	sccp_server_fini();
	config_unload();

	ast_cli_unregister_multiple(cli_sccp, ARRAY_LEN(cli_sccp));

	return 0;
}

static int reload_module(void)
{
	unload_module();
	return load_module();
}

AST_MODULE_INFO(
	
	ASTERISK_GPL_KEY,
	AST_MODFLAG_DEFAULT,
	"Skinny Client Control Protocol",
	.load = load_module,
	.reload = reload_module,
	.unload = unload_module,
);
