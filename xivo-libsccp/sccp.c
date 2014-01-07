#include <asterisk.h>
#include <asterisk/cli.h>
#include <asterisk/module.h>

#include "sccp_config.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

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

	ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries));

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));

	sccp_config_destroy();

	return 0;
}

static int reload(void)
{
	if (sccp_config_reload()) {
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
