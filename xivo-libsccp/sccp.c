#include <asterisk.h>
#include <asterisk/cli.h>
#include <asterisk/module.h>

#include "sccp_config.h"
#include "sccp_server.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

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
				S_OR(device_cfg->line_cfg->voicemail, "(None)"),
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
	AST_CLI_DEFINE(cli_show_config, "Show the module configuration"),
	AST_CLI_DEFINE(cli_show_version, "Show the module version"),
};

static int load_module(void)
{
	if (sccp_config_init()) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (sccp_config_load()) {
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	if (sccp_server_init()) {
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	if (sccp_server_start()) {
		sccp_server_destroy();
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries));

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));

	sccp_server_destroy();

	sccp_config_destroy();

	return 0;
}

static int reload(void)
{
	if (sccp_config_reload()) {
		return -1;
	}

	if (sccp_server_reload_config()) {
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
