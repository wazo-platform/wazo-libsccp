#include <asterisk.h>

ASTERISK_FILE_VERSION(__FILE__, "$Revision: $")

#include <asterisk/astdb.h>
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
	char *conf = NULL, *conf2 = NULL;
	struct sccp_line *line = NULL;
	struct sccp_device *device = NULL;

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
	AST_RWLIST_HEAD_INIT(&sccp_cfg->list_device);
	AST_RWLIST_HEAD_INIT(&sccp_cfg->list_line);

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
		"dialtimeout=3\n"
		"context=default\n"
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

	if (sccp_cfg->dialtimeout != 3) {
		ast_test_status_update(test, "dialtimeout %i != %i\n", sccp_cfg->dialtimeout, 3);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(sccp_cfg->context, "default")) {
		ast_test_status_update(test, "context %s != %s\n", sccp_cfg->context, "default");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	line = find_line_by_name("200", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s doesn't exist\n", "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (line->device == NULL) {
		ast_test_status_update(test, "line %s has no device\n", line->name);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	device = find_device_by_name("SEPACA016FDF235", &sccp_cfg->list_device);
	if (device == NULL) {
		ast_test_status_update(test, "device name %s doesn't exist\n", "SEPACA016FDF235");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	line = find_line_by_name("201", &sccp_cfg->list_line);
	if (line != NULL) {
		ast_test_status_update(test, "line 201 doesn't exist...\n");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

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
		"dialtimeout=3\n"
		"context=default\n"
		"\n"
		"[lines]\n"
		"[201]\n"
		"cid_num=201\n"
		"cid_name=Alice\n"
		"\n"
		"[devices]\n"
		"[SEPACA016FDF236]\n"
		"device=SEPACA016FDF236\n"
		"line=201";

	fwrite(conf, 1, strlen(conf), conf_file);
	fclose(conf_file);

	config_load("/tmp/sccp.conf", sccp_cfg);

	/* We removed line 200 and its associated device.
	 * We add line 201 with a new device.
	 *
	 * Expectation:
	 * Line 201 must be added to the list.
	 * Line 200 must still be in the list.
	 */

	line = find_line_by_name("200", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s doesn't exist\n", "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (line->device == NULL) {
		ast_test_status_update(test, "line %s has no device\n", line->name);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	device = find_device_by_name("SEPACA016FDF235", &sccp_cfg->list_device);
	if (device == NULL) {
		ast_test_status_update(test, "device name %s doesn't exist\n", "SEPACA016FDF235");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	line = find_line_by_name("201", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s doesn't exist\n", "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (line->device == NULL) {
		ast_test_status_update(test, "line %s has no device\n", line->name);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	device = find_device_by_name("SEPACA016FDF236", &sccp_cfg->list_device);
	if (device == NULL) {
		ast_test_status_update(test, "device name %s doesn't exist\n", "SEPACA016FDF236");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

cleanup:

	AST_RWLIST_HEAD_DESTROY(&sccp_cfg->list_device);
	AST_RWLIST_HEAD_DESTROY(&sccp_cfg->list_line);

	config_unload(sccp_cfg);
	ast_free(sccp_cfg);
	remove("/tmp/sccp.conf");

	return ret;
}

static void initialize_device(struct sccp_device *device, const char *name)
{
	ast_mutex_init(&device->lock);
	ast_copy_string(device->name, name, sizeof(device->name));
	TAILQ_INIT(&device->qline);
	device->voicemail[0] = '\0';
	device->mwi_event_sub = NULL;
	device->active_line = NULL;
	device->active_line_cnt = 0;
	device->lookup = 0;
	device->autoanswer = 0;
	device->registered = DEVICE_REGISTERED_FALSE;
	device->session = NULL;

	AST_RWLIST_HEAD_INIT(&device->lines);
}

static void initialize_line(struct sccp_line *line, uint32_t instance, struct sccp_device *device)
{
	ast_mutex_init(&line->lock);
	line->state = SCCP_ONHOOK;
	line->instance = instance;
	line->device = device;
	line->serial_callid = 1;
	line->count_subchan = 0;
	line->active_subchan = NULL;
	line->callfwd = SCCP_CFWD_UNACTIVE;
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
	int err = 0;
	int line_instance = 1;

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

							/* link the line to the device */
							AST_RWLIST_WRLOCK(&device->lines);
							AST_RWLIST_INSERT_HEAD(&device->lines, line_itr, list_per_device);
							AST_RWLIST_UNLOCK(&device->lines);
							device->line_count++;

							/* initialize the line instance */
							initialize_line(line_itr, line_instance++, device);
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
			}

			/* Add the device to the list only if no error occured and
			 * at least one line is present */
			if (!err && device->line_count > 0) {
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
	while (category != NULL && strcasecmp(category, "general") && strcasecmp(category, "devices")) {

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

			for (var = ast_variable_browse(cfg, category); var != NULL; var = var->next) {

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

	sccp_cfg->keepalive = SCCP_DEFAULT_KEEPALIVE;
	sccp_cfg->authtimeout = SCCP_DEFAULT_AUTH_TIMEOUT;
	sccp_cfg->dialtimeout = SCCP_DEFAULT_DIAL_TIMEOUT;

	/* Custom configuration and handle lower bound */
	for (var = ast_variable_browse(cfg, "general"); var != NULL; var = var->next) {

		if (!strcasecmp(var->name, "bindaddr")) {
			ast_copy_string(sccp_cfg->bindaddr, var->value, sizeof(sccp_cfg->bindaddr));
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

		} else if (!strcasecmp(var->name, "dialtimeout")) {
			sccp_cfg->dialtimeout = atoi(var->value);
			if (sccp_cfg->dialtimeout <= 0)
				sccp_cfg->dialtimeout = SCCP_DEFAULT_DIAL_TIMEOUT;
			continue;

		} else if (!strcasecmp(var->name, "context")) {
			ast_copy_string(sccp_cfg->context, var->value, sizeof(sccp_cfg->context));
			continue;
		}
	}

	return 0;
}

static void config_unload(struct sccp_configs *sccp_cfg)
{
	struct sccp_device *device_itr;
	struct sccp_line *line_itr;

	AST_RWLIST_WRLOCK(&sccp_cfg->list_device);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&sccp_cfg->list_device, device_itr, list) {

		ast_mutex_destroy(&device_itr->lock);
		AST_RWLIST_REMOVE_CURRENT(list);
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&sccp_cfg->list_device);

	AST_RWLIST_WRLOCK(&sccp_cfg->list_line);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&sccp_cfg->list_line, line_itr, list) {

		ast_mutex_destroy(&line_itr->lock);
		AST_RWLIST_REMOVE_CURRENT(list);
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&sccp_cfg->list_line);
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

	ast_config_destroy(cfg);

	return 0;
}

static char *sccp_update_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int ret = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp update config";
		e->usage = "Usage: sccp update config\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ret = config_load("sccp.conf", sccp_config);
	if (ret == -1) {
		return CLI_FAILURE;
	}

	return CLI_SUCCESS;
}

static char *sccp_show_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show config";
		e->usage = "Usage: sccp show config\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	struct sccp_line *line_itr;
	struct sccp_device *device_itr;

	ast_cli(a->fd, "bindaddr = %s\n", sccp_config->bindaddr);
	ast_cli(a->fd, "dateformat = %s\n", sccp_config->dateformat);
	ast_cli(a->fd, "keepalive = %d\n", sccp_config->keepalive);
	ast_cli(a->fd, "authtimeout = %d\n", sccp_config->authtimeout);
	ast_cli(a->fd, "dialtimeout = %d\n", sccp_config->dialtimeout);
	ast_cli(a->fd, "context = %s\n", sccp_config->context);
	ast_cli(a->fd, "\n");

	AST_RWLIST_RDLOCK(&sccp_config->list_device);
	AST_RWLIST_TRAVERSE(&sccp_config->list_device, device_itr, list) {
		ast_cli(a->fd, "Device: [%s]\n", device_itr->name);

		AST_RWLIST_RDLOCK(&device_itr->lines);
		AST_RWLIST_TRAVERSE(&device_itr->lines, line_itr, list_per_device) {
			ast_cli(a->fd, "Line extension: (%d) <%s> <%s>\n", line_itr->instance, line_itr->name, line_itr->cid_name);
		}
		AST_RWLIST_UNLOCK(&device_itr->lines);
		ast_cli(a->fd, "\n");
	}
	AST_RWLIST_UNLOCK(&sccp_config->list_device);

	return CLI_SUCCESS;
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
	AST_CLI_DEFINE(sccp_show_config, "Show the configured devices"),
	AST_CLI_DEFINE(sccp_update_config, "Update the configuration"),
};

static void garbage_ast_database()
{
	struct ast_db_entry *db_tree;
	struct ast_db_entry *entry;
	char *line_name;

	db_tree = ast_db_gettree("sccp/cfwdall", NULL);

	/* Remove orphan entries */
	for (entry = db_tree; entry; entry = entry->next) {

		line_name = entry->key + strlen("/sccp/cfwdall/");

		if (find_line_by_name(line_name, &sccp_config->list_line) == NULL) {
			ast_db_del("sccp/cfwdall", line_name);
			ast_log(LOG_DEBUG, "/sccp/cfwdall/%s... removed\n", line_name);
		}
	}
}

static int load_module(void)
{
	int ret = 0;
	ast_verbose("sccp channel loading...\n");

	sccp_config = ast_calloc(1, sizeof(struct sccp_configs));
	if (sccp_config == NULL) {
		return AST_MODULE_LOAD_DECLINE;
	}

	AST_RWLIST_HEAD_INIT(&sccp_config->list_device);
	AST_RWLIST_HEAD_INIT(&sccp_config->list_line);

	ret = config_load("sccp.conf", sccp_config);
	if (ret == -1) {
		ast_free(sccp_config);
		return AST_MODULE_LOAD_DECLINE;
	}

	garbage_ast_database();

	ret = sccp_server_init(sccp_config);
	if (ret == -1) {
		ast_cli_unregister_multiple(cli_sccp, ARRAY_LEN(cli_sccp));
		ast_free(sccp_config);
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

	AST_RWLIST_HEAD_DESTROY(&sccp_config->list_device);
	AST_RWLIST_HEAD_DESTROY(&sccp_config->list_line);

	ast_free(sccp_config);
	sccp_config = NULL;

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
