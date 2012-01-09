#include <asterisk.h>

ASTERISK_FILE_VERSION(__FILE__, "$Revision: $")

#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/test.h>
#include <asterisk/utils.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "sccp.h"
#include "message.h"
#include "device.h"

#include "../config.h"

#ifndef AST_MODULE
#define AST_MODULE "chan_sccp"
#endif

struct sccp_configs *sccp_config; /* global settings */
static int config_load(char *config_file, struct sccp_configs *sccp_cfg);
static void config_unload(struct sccp_configs *sccp_cfg);

AST_TEST_DEFINE(sccp_test_config)
{
	enum ast_test_result_state ret = AST_TEST_PASS;
	struct sccp_configs *sccp_cfg = NULL;
	FILE *conf_file = NULL;
	char *conf = NULL;
	struct sccp_line *line = NULL;

	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_config";
		info->category = "/channel/sccp/";
		info->summary = "test sccp config";
		info->description = "Test wether the sccp configuration is parsed properly.";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	ast_test_status_update(test, "Executing sccp test config...\n");

	sccp_cfg = ast_calloc(1, sizeof(struct sccp_configs));
	if (sccp_config == NULL) {
		ast_test_status_update(test, "ast_calloc failed\n");
		return AST_TEST_FAIL;
	}
	AST_LIST_HEAD_INIT(&sccp_cfg->list_device);
	AST_LIST_HEAD_INIT(&sccp_cfg->list_line);

	conf_file = fopen("/tmp/sccp.conf", "w");
	if (conf_file == NULL) {
		ast_test_status_update(test, "fopen failed %s\n", strerror(errno));
		return AST_TEST_FAIL;
	}

	conf =	"[general]\n"
		"bindaddr=0.0.0.0\n"
		"dateformat=D.M.Y\n"
		"keepalive=10\n"
		"authtimeout=10\n"
		"\n"
		"[lines]\n"
		"[200]\n"
		"cid_num=200\n"
		"cid_name=Bob\n"
		"\n"
		"[devices]\n"
		"[SEPACA016FDF235]\n"
		"device=SEPACA016FDF235\n"
		"line=200";

	fwrite(conf, 1, strlen(conf), conf_file);
	fclose(conf_file);

	config_load("/tmp/sccp.conf", sccp_cfg);

	if (strcmp(sccp_cfg->bindaddr, "0.0.0.0")) {
		ast_test_status_update(test, "bindaddr %s != %s\n", sccp_cfg->bindaddr, "0.0.0.0");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(sccp_cfg->dateformat, "D.M.Y")) {
		ast_test_status_update(test, "dateformat %s != %s\n", sccp_cfg->dateformat, "D.M.Y");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (sccp_cfg->keepalive != 10) {
		ast_test_status_update(test, "keepalive %i != %i\n", sccp_cfg->keepalive, 10);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (sccp_cfg->authtimeout != 10) {
		ast_test_status_update(test, "authtimeout %i != %i\n", sccp_cfg->authtimeout, 10);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	line = find_line_by_name("200", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s != %s\n", line->name, "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (line->device == NULL) {
		ast_test_status_update(test, "line %s has no device\n", line->name);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

cleanup:

	AST_LIST_HEAD_DESTROY(&sccp_cfg->list_device);
	AST_LIST_HEAD_DESTROY(&sccp_cfg->list_line);

	config_unload(sccp_cfg);
	ast_free(sccp_cfg);
	remove("/tmp/sccp.conf");

	return ret;
}

static void initialize_device(struct sccp_device *device, const char *name)
{
	ast_mutex_init(&device->lock);
	ast_copy_string(device->name, name, 80);
	TAILQ_INIT(&device->qline);
	device->active_line = NULL;
	device->active_line_cnt = 0;
	device->lookup = 0;
	device->registered = DEVICE_REGISTERED_FALSE;
	device->session = NULL;

	AST_LIST_HEAD_INIT(&device->lines);
}

static void initialize_line(struct sccp_line *line, uint32_t instance, struct sccp_device *device)
{
	ast_mutex_init(&line->lock);
	line->state = SCCP_ONHOOK;
	line->instance = instance;
	line->device = device;
	line->serial_callid = 0;
	line->count_subchan = 0;
	line->active_subchan = NULL;
	AST_LIST_HEAD_INIT(&line->subchans);

	/* set the device default line */
	if (line->instance == 1)
		device->default_line = line;
}


static int parse_config_devices(struct ast_config *cfg, struct sccp_configs *sccp_cfg)
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
		AST_LIST_TRAVERSE(&sccp_cfg->list_device, device_itr, list) {
			if (!strcasecmp(category, device_itr->name)) {
				ast_log(LOG_WARNING, "Device [%s] already exist, instance ignored\n", category);
				duplicate = 1;
				break;
			}
		}

		if (!duplicate) {

			/* create the new device */
			device = ast_calloc(1, sizeof(struct sccp_device));
			initialize_device(device, category);

			/* add it to the device list */
			AST_LIST_INSERT_HEAD(&sccp_cfg->list_device, device, list);

			/* get every settings for a particular device */
			for (var = ast_variable_browse(cfg, category); var != NULL; var = var->next) {
				if (!strcasecmp(var->name, "line")) {

					/* we are looking for the line that match with 'var->name' */
					AST_LIST_TRAVERSE(&sccp_cfg->list_line, line_itr, list) {
						if (!strcasecmp(var->value, line_itr->name)) {

							/* We found a line */
							found_line = 1;
							if (line_itr->device == NULL) {

								/* link the line to the device */
								AST_LIST_INSERT_HEAD(&device->lines, line_itr, list_per_device);
								device->line_count++;

								/* initialize the line instance */
								initialize_line(line_itr, line_instance++, device);
							} else {
								ast_log(LOG_WARNING, "Line [%s] is already attach to device [%s]\n",
									line_itr->name, line_itr->device->name);
							}
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
		
static int parse_config_lines(struct ast_config *cfg, struct sccp_configs *sccp_cfg)
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
		AST_LIST_TRAVERSE(&sccp_cfg->list_line, line_itr, list) {
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
	
			AST_LIST_INSERT_HEAD(&sccp_cfg->list_line, line, list);

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

static int parse_config_general(struct ast_config *cfg, struct sccp_configs *sccp_cfg)
{
	struct ast_variable *var;	

	for (var = ast_variable_browse(cfg, "general"); var != NULL; var = var->next) {

		if (!strcasecmp(var->name, "bindaddr")) {
			sccp_cfg->bindaddr = ast_strdup(var->value);
			ast_log(LOG_NOTICE, "var name {%s} value {%s} \n", var->name, var->value);
			continue;

		} else if (!strcasecmp(var->name, "dateformat")) {
			ast_copy_string(sccp_cfg->dateformat, var->value, sizeof(sccp_cfg->dateformat));
			continue;

		} else if (!strcasecmp(var->name, "keepalive")) {
			sccp_cfg->keepalive = atoi(var->value);
			continue;
		
		} else if (!strcasecmp(var->name, "authtimeout")) {
			sccp_cfg->authtimeout = atoi(var->value);
			if (sccp_cfg->authtimeout < 10)
				sccp_cfg->authtimeout = SCCP_DEFAULT_AUTH_TIMEOUT;
			continue;
		}
	}

	return 0;
}

static void config_unload(struct sccp_configs *sccp_cfg)
{
	ast_free(sccp_config->bindaddr);

	struct sccp_device *device_itr;
	struct sccp_line *line_itr;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&sccp_cfg->list_device, device_itr, list) {

		ast_mutex_destroy(&device_itr->lock);
		AST_LIST_REMOVE_CURRENT(list);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&sccp_cfg->list_line, line_itr, list) {

		ast_mutex_destroy(&line_itr->lock);
		AST_LIST_REMOVE_CURRENT(list);
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

static int config_load(char *config_file, struct sccp_configs *sccp_cfg)
{
	struct ast_config *cfg;
	struct ast_flags config_flags = { 0 };

	ast_log(LOG_NOTICE, "Configuring sccp from %s...\n", config_file);

	cfg = ast_config_load(config_file, config_flags);
	if (!cfg) {
		ast_log(LOG_ERROR, "Unable to load configuration file '%s'\n", config_file);
		return -1;
	}

	parse_config_general(cfg, sccp_cfg);
	parse_config_lines(cfg, sccp_cfg);
	parse_config_devices(cfg, sccp_cfg);

	struct sccp_line *line_itr;
	struct sccp_device *device_itr;

	AST_LIST_TRAVERSE(&sccp_cfg->list_line, line_itr, list) {
		ast_log(LOG_NOTICE, "Line [%s] \n", line_itr->name);
	}

	AST_LIST_TRAVERSE(&sccp_cfg->list_device, device_itr, list) {
		ast_log(LOG_NOTICE, "Device [%s] : \n", device_itr->name);

		AST_LIST_TRAVERSE(&device_itr->lines, line_itr, list_per_device) {
			ast_log(LOG_NOTICE, "[%s] [%d]\n", line_itr->name, line_itr->instance);
		}
	}

	ast_config_destroy(cfg);

	return 0;
}

static char *sccp_show_version(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show version";
		e->usage = "Usage: sccp show version\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ast_cli(a->fd, "%s <%s>\n", PACKAGE_STRING, PACKAGE_BUGREPORT);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_sccp[] = {
	AST_CLI_DEFINE(sccp_show_version, "Show the version of the sccp channel"),
};

static int load_module(void)
{
	int ret = 0;
	ast_verbose("sccp channel loading...\n");

	sccp_config = ast_calloc(1, sizeof(struct sccp_configs));
	if (sccp_config == NULL) {
		AST_MODULE_LOAD_DECLINE;
	}

	AST_LIST_HEAD_INIT(&sccp_config->list_device);
	AST_LIST_HEAD_INIT(&sccp_config->list_line);

	ret = config_load("sccp.conf", sccp_config);
	if (ret == -1) {
		return AST_MODULE_LOAD_DECLINE;
	}

	ret = sccp_server_init(sccp_config);
	if (ret == -1) {
		ast_cli_unregister_multiple(cli_sccp, ARRAY_LEN(cli_sccp));
		return AST_MODULE_LOAD_DECLINE;
	}
	sccp_rtp_init(ast_module_info);

	ast_cli_register_multiple(cli_sccp, ARRAY_LEN(cli_sccp));
	AST_TEST_REGISTER(sccp_test_config);

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_verbose("sccp channel unloading...\n");

	sccp_server_fini();
	sccp_rtp_fini();
	config_unload(sccp_config);

	ast_cli_unregister_multiple(cli_sccp, ARRAY_LEN(cli_sccp));

	AST_LIST_HEAD_DESTROY(&sccp_config->list_device);
	AST_LIST_HEAD_DESTROY(&sccp_config->list_line);
	ast_free(sccp_config);

	AST_TEST_UNREGISTER(sccp_test_config);

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
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
);
