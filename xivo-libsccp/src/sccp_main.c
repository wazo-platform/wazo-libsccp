#include <asterisk.h>

#include <asterisk/astdb.h>
#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/netsock2.h>
#include <asterisk/test.h>
#include <asterisk/utils.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "sccp.h"
#include "sccp_message.h"
#include "sccp_device.h"
#include "sccp_config.h"

#include "../config.h"

#include "test_config.c"

static struct sccp_configs *sccp_config;

static char *sccp_resync_device(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct sccp_device *device = NULL;
	int restart = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp resync";
		e->usage =
			"Usage: sccp resync <device>\n"
			"       Resynchronize an SCCP device with its updated configuration.\n";
		return NULL;

	case CLI_GENERATE:
		if (a->pos == 2) {
			return complete_sccp_devices(a->word, a->n, &sccp_config->list_device);
		}
		return NULL;
	}

	device = find_device_by_name(a->argv[2], &sccp_config->list_device);
	if (device == NULL)
		return CLI_FAILURE;

	if (device->registered != DEVICE_REGISTERED_TRUE) {
		return CLI_FAILURE;
	}

	/* Prevent the device from reconnecting while it is
	   getting destroyed */
	AST_LIST_REMOVE(&sccp_config->list_device, device, list);

	/* Tell the thread_session to destroy the device */
	device->destroy = 1;

	/* Ask the phone to reboot, as soon as possible */
	transmit_reset(device->session, 1);

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

	ast_cli(a->fd, "%s\n", PACKAGE_STRING);

	return CLI_SUCCESS;
}

char *sccp_show_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct sccp_line *line_itr = NULL;
	struct sccp_device *device_itr = NULL;
	struct sccp_speeddial *speeddial_itr = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show config";
		e->usage = "Usage: sccp show config\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ast_cli(a->fd, "bindaddr = %s\n", sccp_config->bindaddr);
	ast_cli(a->fd, "dateformat = %s\n", sccp_config->dateformat);
	ast_cli(a->fd, "keepalive = %d\n", sccp_config->keepalive);
	ast_cli(a->fd, "authtimeout = %d\n", sccp_config->authtimeout);
	ast_cli(a->fd, "dialtimeout = %d\n", sccp_config->dialtimeout);
	ast_cli(a->fd, "context = %s\n", sccp_config->context);
	ast_cli(a->fd, "language = %s\n", sccp_config->language);
	ast_cli(a->fd, "directmedia = %d\n", sccp_config->directmedia);
	ast_cli(a->fd, "\n");

	AST_RWLIST_RDLOCK(&sccp_config->list_device);
	AST_RWLIST_TRAVERSE(&sccp_config->list_device, device_itr, list) {
		ast_cli(a->fd, "Device: [%s]\n", device_itr->name);

		AST_RWLIST_RDLOCK(&device_itr->lines);
		AST_RWLIST_TRAVERSE(&device_itr->lines, line_itr, list_per_device) {
			ast_cli(a->fd, "Line extension: (%d) <%s> <%s> <%s> <%s>\n",
				line_itr->instance, line_itr->name, line_itr->cid_name, line_itr->context, line_itr->language);
		}
		AST_RWLIST_UNLOCK(&device_itr->lines);

		AST_RWLIST_RDLOCK(&device_itr->speeddials);
		AST_RWLIST_TRAVERSE(&device_itr->speeddials, speeddial_itr, list_per_device) {
			ast_cli(a->fd, "Speeddial: (%d) <%s>\n", speeddial_itr->index, speeddial_itr->extension);
		}
		AST_RWLIST_UNLOCK(&device_itr->speeddials);

		ast_cli(a->fd, "\n");
	}
	AST_RWLIST_UNLOCK(&sccp_config->list_device);

	return CLI_SUCCESS;
}

char *sccp_update_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
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

	ret = sccp_config_load(sccp_config, "sccp.conf");
	if (ret == -1) {
		return CLI_FAILURE;
	}

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_sccp[] = {
	AST_CLI_DEFINE(sccp_show_version, "Show the version of the sccp channel"),
	AST_CLI_DEFINE(sccp_show_config, "Show the configured devices"),
	AST_CLI_DEFINE(sccp_resync_device, "Resynchronize SCCP device"),
	AST_CLI_DEFINE(sccp_update_config, "Update the configuration"),
};

static void garbage_ast_database()
{
	struct ast_db_entry *db_tree = NULL;
	struct ast_db_entry *entry = NULL;
	char *line_name = NULL;

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
	ast_log(LOG_NOTICE, "sccp channel loading...\n");

	ret = sccp_config_init(&sccp_config);
	if (ret == -1) {
		return AST_MODULE_LOAD_DECLINE;
	}

	ret = sccp_config_load(sccp_config, "sccp.conf");
	if (ret == -1) {
		sccp_config_destroy(&sccp_config);

		return AST_MODULE_LOAD_DECLINE;
	}

	garbage_ast_database();

	ret = sccp_server_init(sccp_config);
	if (ret == -1) {
		sccp_config_unload(sccp_config);
		sccp_config_destroy(&sccp_config);

		return AST_MODULE_LOAD_DECLINE;
	}
	sccp_rtp_init(ast_module_info);

	ast_cli_register_multiple(cli_sccp, ARRAY_LEN(cli_sccp));
	AST_TEST_REGISTER(sccp_test_config);
	AST_TEST_REGISTER(sccp_test_resync);

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_log(LOG_DEBUG, "sccp channel unloading...\n");

	sccp_server_fini();
	sccp_rtp_fini();
	sccp_config_unload(sccp_config);

	ast_cli_unregister_multiple(cli_sccp, ARRAY_LEN(cli_sccp));

	sccp_config_destroy(&sccp_config);

	AST_TEST_UNREGISTER(sccp_test_resync);
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
	AST_MODFLAG_LOAD_ORDER,
	"Skinny Client Control Protocol",
	.load = load_module,
	.reload = reload_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
);
