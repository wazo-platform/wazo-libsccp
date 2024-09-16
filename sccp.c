#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/causes.h>
#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/devicestate.h>
#include <asterisk/module.h>
#include <asterisk/rtp_engine.h>
#include <asterisk/sched.h>

#include "device/sccp_channel_tech.h"
#include "device/sccp_rtp_glue.h"
#include "sccp_debug.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_device_registry.h"
#include "sccp_msg.h"
#include "sccp_server.h"
#include "sccp_utils.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

struct ast_sched_context *sccp_sched;
const struct ast_module_info *sccp_module_info;

static struct sccp_device_registry *global_registry;
static struct sccp_server *global_server;

enum find_line_result {
	LINE_FOUND,
	LINE_NOT_REGISTERED,
	LINE_NOT_FOUND,
};

/*!
 * \brief Find a line and if not found, tell why.
 *
 * \note If the line is found, then its reference count is incremented by one.
 *
 * \retval LINE_FOUND on success
 * \retval LINE_NOT_REGISTERED if the line exist in the config but is not currently registered
 * \retval LINE_NOT_FOUND is the line doesn't exist in the config
 */
static enum find_line_result find_line(const char *name, struct sccp_line **result)
{
	struct sccp_cfg *cfg;
	struct sccp_line_cfg *line_cfg;

	*result = sccp_device_registry_find_line(global_registry, name);
	if (*result) {
		return LINE_FOUND;
	}

	cfg = sccp_config_get();
	line_cfg = sccp_cfg_find_line(cfg, name);
	ao2_ref(cfg, -1);
	if (line_cfg) {
		ao2_ref(line_cfg, -1);
		return LINE_NOT_REGISTERED;
	}

	return LINE_NOT_FOUND;
}

static struct ast_channel *channel_tech_requester(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *addr, int *cause)
{
	struct sccp_line *line;
	struct ast_channel *channel = NULL;
	char *options;

	options = strchr(addr, '/');
	if (options) {
		*options++ = '\0';
	}

	switch (find_line(addr, &line)) {
	case LINE_FOUND:
		channel = sccp_channel_tech_requester(line, options, cap, assignedids, requestor, cause);
		ao2_ref(line, -1);
		break;
	case LINE_NOT_REGISTERED:
		*cause = AST_CAUSE_SUBSCRIBER_ABSENT;
		break;
	case LINE_NOT_FOUND:
		*cause = AST_CAUSE_NO_ROUTE_DESTINATION;
		break;
	}

	if (!line) {
		return NULL;
	}

	struct sccp_device *device = sccp_line_device(line);
	if (sccp_device_has_active_subchan(device)) {
		if (sccp_device_has_active_incoming_subchan(device)) {
			sccp_device_transmit_tone(device, SCCP_TONE_CALLWAIT);
			sccp_device_transmit_callstate(device, SCCP_CALLWAIT);
		} else {
			sccp_device_transmit_tone(device, SCCP_TONE_NONE);
		}
	}


	return channel;
}

static int channel_tech_devicestate(const char *data)
{
	struct sccp_line *line;
	char *name = ast_strdupa(data);
	char *ptr;
	int state;

	ptr = strchr(name, '/');
	if (ptr) {
		*ptr = '\0';
	}

	switch (find_line(name, &line)) {
	case LINE_FOUND:
		state = sccp_channel_tech_devicestate(line);
		ao2_ref(line, -1);
		break;
	case LINE_NOT_REGISTERED:
		state = AST_DEVICE_UNAVAILABLE;
		break;
	case LINE_NOT_FOUND:
		state = AST_DEVICE_INVALID;
		break;
	}

	return state;
}

struct ast_channel_tech sccp_tech = {
	.type = "sccp",
	.description = "Skinny Client Control Protocol",
	.properties = AST_CHAN_TP_WANTSJITTER | AST_CHAN_TP_CREATESJITTER,
	.requester = channel_tech_requester,
	.devicestate = channel_tech_devicestate,
	.call = sccp_channel_tech_call,
	.hangup = sccp_channel_tech_hangup,
	.answer = sccp_channel_tech_answer,
	.read = sccp_channel_tech_read,
	.write = sccp_channel_tech_write,
	.indicate = sccp_channel_tech_indicate,
	.fixup = sccp_channel_tech_fixup,
	.send_digit_end = sccp_channel_tech_send_digit_end,
	.func_channel_read = sccp_channel_tech_acf_channel_read,
};

static struct ast_rtp_glue sccp_rtp_glue = {
	.type = "sccp",
	.get_rtp_info = sccp_rtp_glue_get_rtp_info,
	.update_peer = sccp_rtp_glue_update_peer,
	.get_codec = sccp_rtp_glue_get_codec,
};

static int reset_one_device(const char *name, enum sccp_reset_type type)
{
	struct sccp_device *device;

	device = sccp_device_registry_find(global_registry, name);
	if (!device) {
		return -1;
	}

	sccp_device_reset(device, type);
	ao2_ref(device, -1);

	return 0;
}

static void reset_registry_callback(struct sccp_device *device, void *data)
{
	enum sccp_reset_type type = *((enum sccp_reset_type *) data);

	sccp_device_reset(device, type);
}

static int reset_all_devices(enum sccp_reset_type type)
{
	sccp_device_registry_do(global_registry, reset_registry_callback, &type);

	return 0;
}

static char *cli_reset_device(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	static const char * const choices[] = { "restart", NULL };
	const char *name;
	enum sccp_reset_type type;
	int ret;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp reset";
		e->usage =
			"Usage: sccp reset <device|all> [restart]\n"
			"       Reset one or all SCCP device, optionally with a full restart.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 2) {
			return sccp_device_registry_complete(global_registry, a->word, a->n);
		} else if (a->pos == 3) {
			return ast_cli_complete(a->word, choices, a->n);
		}

		return NULL;
	}

	if (a->argc < 3) {
		return CLI_SHOWUSAGE;
	}

	name = a->argv[2];

	if (a->argc == 4 && !strcasecmp(a->argv[3], "restart")) {
		type = SCCP_RESET_HARD_RESTART;
	} else {
		type = SCCP_RESET_SOFT;
	}

	if (!strcasecmp(name, "all")) {
		ret = reset_all_devices(type);
	} else {
		ret = reset_one_device(name, type);
	}

	return ret ? CLI_FAILURE : CLI_SUCCESS;
}

static char *cli_set_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	const char *what;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp set debug {off|on|ip|device}";
		e->usage =
			"Usage: sccp set debug {off|on|ip addr|device name}\n"
			"       Globally disables dumping of SCCP packets,\n"
			"       or enables it either globally or for a (single)\n"
			"       IP address or device name.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 4 && !strcasecmp(a->argv[3], "device")) {
			return sccp_device_registry_complete(global_registry, a->word, a->n);
		}

		return NULL;
	}

	what = a->argv[e->args - 1];

	if (!strcasecmp(what, "on")) {
		sccp_debug_enable();
		ast_cli(a->fd, "SCCP debugging enabled\n");
	} else if (!strcasecmp(what, "off")) {
		sccp_debug_disable();
		ast_cli(a->fd, "SCCP debugging disabled\n");
	} else if (!strcasecmp(what, "device") && a->argc == e->args + 1) {
		sccp_debug_enable_device_name(a->argv[e->args]);
		ast_cli(a->fd, "SCCP debugging enabled for device: %s\n", a->argv[e->args]);
	} else if (!strcasecmp(what, "ip") && a->argc == e->args + 1) {
		sccp_debug_enable_ip(a->argv[e->args]);
		ast_cli(a->fd, "SCCP debugging enabled for IP: %s\n", a->argv[e->args]);
	} else {
		return CLI_SHOWUSAGE;
	}

	sccp_server_reload_debug(global_server);

	return CLI_SUCCESS;
}

static char *cli_show_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
#define FORMAT_STRING  "%-18.18s %-12.12s %-24.24s %-4d\n"
#define FORMAT_STRING2 "%-18.18s %-12.12s %-24.24s %-4s\n"
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

	ast_cli(a->fd, "authtimeout = %d\n", cfg->general_cfg->authtimeout);
	ast_cli(a->fd, "guest = %s\n", AST_CLI_YESNO(cfg->general_cfg->guest_device_cfg));
	ast_cli(a->fd, "max_guests = %u\n\n", cfg->general_cfg->max_guests);

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
#define FORMAT_STRING  "%-16.16s %-16.16s %-6.6s %-6.6s %-6.6s %-25.25s\n"
#define FORMAT_STRING2 "%-16.16s %-16.16s %-6.6s %-6.6s %-6u %-25.25s\n"
	struct sccp_device_snapshot *snapshots;
	size_t n;
	size_t n_guests = 0;
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

	ast_cli(a->fd, FORMAT_STRING, "Device", "IP", "Guest", "Type", "Proto", "Capabilities");
	for (i = 0; i < n; i++) {
		ast_cli(a->fd, FORMAT_STRING2, snapshots[i].name, snapshots[i].ipaddr, AST_CLI_YESNO(snapshots[i].guest),
				sccp_device_type_str(snapshots[i].type), snapshots[i].proto_version, snapshots[i].capabilities);
		if (snapshots[i].guest) {
			n_guests++;
		}
	}

	ast_cli(a->fd, "Total: %zu connected device(s) (%zu guests)\n", n, n_guests);

	ast_free(snapshots);

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

	ast_cli(a->fd, "wazo-libsccp %s\n", VERSION);

	return CLI_SUCCESS;
}

static char *cli_show_stats(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct sccp_stat stat;
	struct timeval tmp_tv = {.tv_usec = 0};
	struct ast_tm tm;
	char device_fault_last[64] = "-";
	char device_panic_last[64] = "-";

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show stats";
		e->usage = "Usage: sccp show stats\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	sccp_stat_take_snapshot(&stat);

	if (stat.device_fault_count) {
		tmp_tv.tv_sec = stat.device_fault_last;
		ast_localtime(&tmp_tv, &tm, NULL);
		ast_strftime(device_fault_last, sizeof(device_fault_last), "%Y-%m-%d %H:%M:%S", &tm);
	}

	if (stat.device_panic_count) {
		tmp_tv.tv_sec = stat.device_panic_last;
		ast_localtime(&tmp_tv, &tm, NULL);
		ast_strftime(device_panic_last, sizeof(device_panic_last), "%Y-%m-%d %H:%M:%S", &tm);
	}

	ast_cli(a->fd,
			"Device fault:          %d\n"
			"Last device fault:     %s\n"
			"Device panic:          %d\n"
			"Last device panic:     %s\n",
			stat.device_fault_count, device_fault_last, stat.device_panic_count, device_panic_last);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_reset_device, "Reset SCCP device"),
	AST_CLI_DEFINE(cli_set_debug, "Enable/Disable SCCP debugging"),
	AST_CLI_DEFINE(cli_show_config, "Show the module configuration"),
	AST_CLI_DEFINE(cli_show_devices, "Show the connected devices"),
	AST_CLI_DEFINE(cli_show_stats, "Show the module stats"),
	AST_CLI_DEFINE(cli_show_version, "Show the module version"),
};

static int register_sccp_tech(void)
{
	sccp_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!sccp_tech.capabilities) {
		return -1;
	}

	ast_format_cap_append_by_type(sccp_tech.capabilities, AST_MEDIA_TYPE_AUDIO);
	if (ast_channel_register(&sccp_tech) == -1) {
		ao2_ref(sccp_tech.capabilities, -1);
		return -1;
	}

	return 0;
}

static void unregister_sccp_tech(void)
{
	ast_channel_unregister(&sccp_tech);
	ao2_ref(sccp_tech.capabilities, -1);
}

static int load_module(void)
{
	struct sccp_cfg *cfg = NULL;

	sccp_module_info = ast_module_info;

	if (sccp_config_init()) {
		goto fail1;
	}

	if (sccp_config_load()) {
		goto fail2;
	}

	cfg = sccp_config_get();
	global_registry = sccp_device_registry_create(cfg);
	if (!global_registry) {
		goto fail2;
	}

	sccp_sched = ast_sched_context_create();
	if (!sccp_sched) {
		goto fail3;
	}

	global_server = sccp_server_create(cfg, global_registry);
	if (!global_server) {
		goto fail4;
	}

	if (register_sccp_tech()) {
		goto fail5;
	}

	if (ast_rtp_glue_register(&sccp_rtp_glue)) {
		goto fail6;
	}

	if (sccp_server_start(global_server)) {
		goto fail7;
	}

	ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries));
	ao2_ref(cfg, -1);

	return AST_MODULE_LOAD_SUCCESS;

fail7:
	ast_rtp_glue_unregister(&sccp_rtp_glue);
fail6:
	unregister_sccp_tech();
fail5:
	sccp_server_destroy(global_server);
fail4:
	ast_sched_context_destroy(sccp_sched);
fail3:
	sccp_device_registry_destroy(global_registry);
fail2:
	ao2_cleanup(cfg);
	sccp_config_destroy();
fail1:

	return AST_MODULE_LOAD_DECLINE;
}

static int unload_module(void)
{
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));

	ast_rtp_glue_unregister(&sccp_rtp_glue);
	unregister_sccp_tech();
	sccp_server_destroy(global_server);
	ast_sched_context_destroy(sccp_sched);
	sccp_device_registry_destroy(global_registry);
	sccp_config_destroy();

	return 0;
}

static int reload(void)
{
	struct sccp_cfg *cfg;
	int ret = 0;

	if (sccp_config_reload()) {
		return -1;
	}

	cfg = sccp_config_get();
	ret |= sccp_server_reload_config(global_server, cfg);
	ret |= sccp_device_registry_reload_config(global_registry, cfg);
	ao2_ref(cfg, -1);

	return ret ? -1 : 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "SCCP Channel Driver",
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
);
