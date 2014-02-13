#include <asterisk.h>
#include <asterisk/causes.h>
#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/devicestate.h>
#include <asterisk/module.h>
#include <asterisk/rtp_engine.h>
#include <asterisk/sched.h>

#include "sccp_debug.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_device_registry.h"
#include "sccp_msg.h"
#include "sccp_server.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

struct ast_sched_context *sccp_sched;

static struct sccp_device_registry *global_registry;
static struct sccp_server *global_server;

static struct ast_channel *sccp_request(const char *type, struct ast_format_cap *cap, const struct ast_channel *requestor, const char *addr, int *cause)
{
	struct sccp_line *line;
	struct ast_channel *channel;

	line = sccp_device_registry_find_line(global_registry, addr);
	if (!line) {
		/* XXX in fact, with the new system, it could be either because:
		 *
		 * the line (i.e. associated device) is not registered (AST_CAUSE_SUBSCRIBER_ABSENT)
		 * the line does not exist (AST_CAUSE_NO_ROUTE_DESTINATION)
		 */
		*cause = AST_CAUSE_NO_ROUTE_DESTINATION;
		return NULL;
	}

	/* TODO add support for autoanswer */
	channel = sccp_line_request(line, cap, requestor ? ast_channel_linkedid(requestor) : NULL, cause);
	ao2_ref(line, -1);

	return channel;
}

static int sccp_devicestate(const char *data)
{
	struct sccp_line *line;
	struct sccp_cfg *cfg;
	struct sccp_line_cfg *line_cfg;
	char *name = ast_strdupa(data);
	char *ptr;
	int state = AST_DEVICE_UNKNOWN;

	ptr = strchr(name, '/');
	if (ptr) {
		*ptr = '\0';
	}

	line = sccp_device_registry_find_line(global_registry, name);
	if (!line) {
		cfg = sccp_config_get();
		line_cfg = sccp_cfg_find_line(cfg, name);
		if (!line_cfg) {
			state = AST_DEVICE_INVALID;
		} else {
			state = AST_DEVICE_UNAVAILABLE;
			ao2_ref(line_cfg, -1);
		}

		ao2_ref(cfg, -1);
	} else {
		state = sccp_line_devstate(line);
		ao2_ref(line, -1);
	}

	return state;
}

static int sccp_call(struct ast_channel *channel, const char *dest, int timeout)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);

	return sccp_subchannel_call(subchan);
}

static int sccp_hangup(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);

	return sccp_subchannel_hangup(subchan);
}

static int sccp_answer(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);

	return sccp_subchannel_answer(subchan);
}

static struct ast_frame *sccp_read(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);

	return sccp_subchannel_read(subchan);
}

static int sccp_write(struct ast_channel *channel, struct ast_frame *frame)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);

	return sccp_subchannel_write(subchan, frame);
}

static int sccp_indicate(struct ast_channel *channel, int ind, const void *data, size_t datalen)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);

	return sccp_subchannel_indicate(subchan, ind, data, datalen);
}

static int sccp_fixup(struct ast_channel *oldchannel, struct ast_channel *newchannel)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(newchannel);

	return sccp_subchannel_fixup(subchan, newchannel);
}

static int sccp_senddigit_begin(struct ast_channel *channel, char digit)
{
	ast_log(LOG_DEBUG, "senddigit begin %c\n", digit);
	return 0;
}

static int sccp_senddigit_end(struct ast_channel *channel, char digit, unsigned int duration)
{
	ast_log(LOG_DEBUG, "senddigit end %c\n", digit);
	return 0;
}

struct ast_channel_tech sccp_tech = {
	.type = "sccp",
	.description = "Skinny Client Control Protocol",
	.properties = AST_CHAN_TP_WANTSJITTER | AST_CHAN_TP_CREATESJITTER,
	.requester = sccp_request,
	.devicestate = sccp_devicestate,
	.call = sccp_call,
	.hangup = sccp_hangup,
	.answer = sccp_answer,
	.read = sccp_read,
	.write = sccp_write,
	.indicate = sccp_indicate,
	.fixup = sccp_fixup,
	.send_digit_begin = sccp_senddigit_begin,
	.send_digit_end = sccp_senddigit_end,
	.bridge = ast_rtp_instance_bridge,
};

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

static char *sccp_set_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	const char *what;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp set debug {on|off|ip}";
		e->usage =
			"Usage: sccp set debug {on|off|ip addr}\n"
			"       Enable/disable dumping of SCCP packets.\n";
		return NULL;

	case CLI_GENERATE:
		return NULL;
	}

	what = a->argv[e->args - 1];

	if (!strcasecmp(what, "on")) {
		sccp_enable_debug();
		ast_cli(a->fd, "SCCP debugging enabled\n");
	} else if (!strcasecmp(what, "off")) {
		sccp_disable_debug();
		ast_cli(a->fd, "SCCP debugging disabled\n");
	}  else if (!strcasecmp(what, "ip") && a->argc == e->args + 1) {
		sccp_enable_debug_ip(a->argv[e->args]);
		ast_cli(a->fd, "SCCP debugging enabled for IP: %s\n", sccp_debug_addr);
	} else {
		return CLI_SHOWUSAGE;
	}

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
#define FORMAT_STRING  "%-16.16s %-16.16s %-6.6s %-6.6s %-18.18s\n"
#define FORMAT_STRING2 "%-16.16s %-16.16s %-6.6s %-6u %-18.18s\n"
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

	ast_cli(a->fd, FORMAT_STRING, "Device", "IP", "Type", "Proto", "Capabilities");
	for (i = 0; i < n; i++) {
		ast_cli(a->fd, FORMAT_STRING2, snapshots[i].name, snapshots[i].ipaddr, sccp_device_type_str(snapshots[i].type), snapshots[i].proto_version, snapshots[i].capabilities);
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
	AST_CLI_DEFINE(sccp_set_debug, "Enable/Disable SCCP debugging"),
	AST_CLI_DEFINE(cli_show_config, "Show the module configuration"),
	AST_CLI_DEFINE(cli_show_devices, "Show the connected devices"),
	AST_CLI_DEFINE(cli_show_sessions, "Show the active sessions"),
	AST_CLI_DEFINE(cli_show_version, "Show the module version"),
};

static int register_sccp_tech(void)
{
	sccp_tech.capabilities = ast_format_cap_alloc();
	if (!sccp_tech.capabilities) {
		return -1;
	}

	ast_format_cap_add_all_by_type(sccp_tech.capabilities, AST_FORMAT_TYPE_AUDIO);

	return ast_channel_register(&sccp_tech);
}

static void unregister_sccp_tech(void)
{
	ast_channel_unregister(&sccp_tech);
	ast_format_cap_destroy(sccp_tech.capabilities);
}

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

	sccp_sched = ast_sched_context_create();
	if (!sccp_sched) {
		sccp_device_registry_destroy(global_registry);
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	cfg = sccp_config_get();
	global_server = sccp_server_create(cfg, global_registry);
	if (!global_server) {
		ast_sched_context_destroy(sccp_sched);
		sccp_device_registry_destroy(global_registry);
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	if (sccp_server_start(global_server)) {
		sccp_server_destroy(global_server);
		ast_sched_context_destroy(sccp_sched);
		sccp_device_registry_destroy(global_registry);
		sccp_config_destroy();
		return AST_MODULE_LOAD_DECLINE;
	}

	if (register_sccp_tech()) {
		sccp_server_destroy(global_server);
		ast_sched_context_destroy(sccp_sched);
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

	unregister_sccp_tech();
	sccp_server_destroy(global_server);
	ast_sched_context_destroy(sccp_sched);
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
