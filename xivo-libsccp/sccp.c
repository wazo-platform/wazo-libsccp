#include <asterisk.h>
#include <asterisk/cli.h>
#include <asterisk/module.h>

#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_device_registry.h"
#include "sccp_msg.h"
#include "sccp_server.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

static struct sccp_device_registry *global_registry;
static struct sccp_server *global_server;

static char *sccp_reset_device(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	static const char * const choices[] = { "restart", NULL };
	struct sccp_device *device;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp reset";
		e->usage =
			"Usage: sccp reset <device> [restart]\n"
			"       Resets an SCCP device, optionally with a full restart.\n";
		return NULL;

	case CLI_GENERATE:
		if (a->pos == 2) {
			return sccp_device_registry_complete(global_registry, a->word, a->n);
		} else if (a->pos == 3) {
			return ast_cli_complete(a->word, choices, a->n);
		}

		return NULL;
	}

	device = sccp_device_registry_find(global_registry, a->argv[2]);
	if (!device) {
		return CLI_FAILURE;
	}

	if (a->argc == 4 && !strcasecmp(a->argv[3], "restart")) {
		sccp_device_reset(device, SCCP_RESET_HARD_RESTART);
	} else {
		sccp_device_reset(device, SCCP_RESET_SOFT);
	}

	ao2_ref(device, -1);

	return CLI_SUCCESS;
}

static char *cli_show_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
#define FORMAT_STRING  "%-18.18s %-12.12s %-12.12s %-4d\n"
#define FORMAT_STRING2 "%-18.18s %-12.12s %-12.12s %-4s\n"
	struct sccp_cfg *cfg;
	struct sccp_device_cfg *device_cfg;
	struct ao2_iterator iter;
	int count = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show config";
		e->usage = "Usage: sccp show config\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	cfg = sccp_config_get();

	ast_cli(a->fd, "bindaddr = %s\n", cfg->general_cfg->bindaddr);
	ast_cli(a->fd, "authtimeout = %d\n", cfg->general_cfg->authtimeout);
	ast_cli(a->fd, "guest = %s\n\n", AST_CLI_YESNO(cfg->general_cfg->guest_device_cfg));

	ast_cli(a->fd, FORMAT_STRING2, "Device", "Line", "Voicemail", "Speeddials");
	iter = ao2_iterator_init(cfg->devices_cfg, 0);
	while ((device_cfg = ao2_iterator_next(&iter))) {
		ast_cli(a->fd, FORMAT_STRING,
				device_cfg->name,
				device_cfg->line_cfg->name,
				S_OR(device_cfg->voicemail, "(None)"),
				(int) device_cfg->speeddial_count);
		ao2_ref(device_cfg, -1);
		count++;
	}

	ao2_iterator_destroy(&iter);
	ast_cli(a->fd, "%d devices\n", count);

	ao2_ref(cfg, -1);

	return CLI_SUCCESS;

#undef FORMAT_STRING
#undef FORMAT_STRING2
}

static char *cli_show_devices(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
#define FORMAT_STRING  "%-16.16s %-6.6s %-6.6s %-18.18s\n"
#define FORMAT_STRING2 "%-16.16s %-6.6s %-6u %-18.18s\n"
	struct sccp_device_snapshot *snapshots;
	size_t n;
	size_t i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show devices";
		e->usage =
				"Usage: sccp show devices\n"
				"       Show the connected devices.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (sccp_device_registry_take_snapshots(global_registry, &snapshots, &n)) {
		return CLI_FAILURE;
	}

	ast_cli(a->fd, FORMAT_STRING, "Device", "Type", "Proto", "Capabilities");
	for (i = 0; i < n; i++) {
		ast_cli(a->fd, FORMAT_STRING2, snapshots[i].name, sccp_device_type_str(snapshots[i].type), snapshots[i].proto_version, snapshots[i].capabilities);
	}

	ast_cli(a->fd, "Total: %zu connected device(s)\n", n);

	ast_free(snapshots);

	return CLI_SUCCESS;

#undef FORMAT_STRING
#undef FORMAT_STRING2
}

static char *cli_show_sessions(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show sessions";
		e->usage = "Usage: sccp show sessions\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ast_cli(a->fd, "%d active sessions\n", sccp_server_session_count(global_server));

	return CLI_SUCCESS;
}

static char *cli_show_version(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show version";
		e->usage = "Usage: sccp show version\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ast_cli(a->fd, "xivo-libsccp %s\n", VERSION);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(sccp_reset_device, "Reset SCCP device"),
	AST_CLI_DEFINE(cli_show_config, "Show the module configuration"),
	AST_CLI_DEFINE(cli_show_devices, "Show the connected devices"),
	AST_CLI_DEFINE(cli_show_sessions, "Show the active sessions"),
	AST_CLI_DEFINE(cli_show_version, "Show the module version"),
};

static int load_module(void)
{
	RAII_VAR(struct sccp_cfg *, cfg, NULL, ao2_cleanup);

	if (sccp_config_init()) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (sccp_config_load()) {
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	global_registry = sccp_device_registry_create();
	if (!global_registry) {
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	cfg = sccp_config_get();
	global_server = sccp_server_create(cfg, global_registry);
	if (!global_server) {
		sccp_device_registry_destroy(global_registry);
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	if (sccp_server_start(global_server)) {
		sccp_server_destroy(global_server);
		sccp_device_registry_destroy(global_registry);
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries));

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));

	sccp_server_destroy(global_server);
	sccp_device_registry_destroy(global_registry);
	sccp_config_destroy();

	return 0;
}

static int reload(void)
{
	RAII_VAR(struct sccp_cfg *, cfg, NULL, ao2_cleanup);

	if (sccp_config_reload()) {
		return -1;
	}

	cfg = sccp_config_get();
	if (sccp_server_reload_config(global_server, cfg)) {
		return -1;
	}

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "SCCP Channel Driver",
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
);
