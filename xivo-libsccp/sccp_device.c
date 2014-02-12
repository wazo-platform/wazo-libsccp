#include <asterisk.h>
#include <asterisk/app.h>
#include <asterisk/astobj2.h>
#include <asterisk/causes.h>
#include <asterisk/channelstate.h>
#include <asterisk/event.h>
#include <asterisk/format.h>
#include <asterisk/lock.h>
#include <asterisk/module.h>
#include <asterisk/musiconhold.h>
#include <asterisk/network.h>
#include <asterisk/pbx.h>
#include <asterisk/rtp_engine.h>

#include "sccp.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_session.h"
#include "sccp_msg.h"
#include "sccp_queue.h"
#include "sccp_serializer.h"
#include "sccp_utils.h"

#define LINE_INSTANCE_START 1

struct sccp_speeddial {
	/* (static) */
	struct sccp_device *device;
	/* (dynamic) */
	struct sccp_speeddial_cfg *cfg;

	/* (static) */
	uint32_t instance;
	uint32_t index;

	/* (dynamic, modified in thread session only) */
	int state_id;
	int state;	/* enum ast_extension_states */
};

struct speeddial_group {
	struct sccp_speeddial **speeddials;
	size_t count;
};

struct sccp_subchannel {
	/* (dynamic) */
	struct ast_sockaddr direct_media_addr;
	struct ast_format fmt;

	/* (static) */
	struct sccp_line *line;
	/* (dynamic) */
	struct ast_channel *channel;
	/* (dynamic) */
	struct ast_rtp_instance *rtp;

	/* (static) */
	uint32_t id;
	/* (dynamic) */
	enum sccp_state state;
	/* (static) */
	enum sccp_direction direction;

	uint8_t on_hold;
	uint8_t resuming;
	uint8_t transferring;

	AST_LIST_ENTRY(sccp_subchannel) list;
};

struct sccp_line {
	/* (dynamic) */
	AST_LIST_HEAD_NOLOCK(, sccp_subchannel) subchans;

	/* (static) */
	struct sccp_device *device;
	/* (dynamic) */
	struct sccp_line_cfg *cfg;

	/* (static) */
	uint32_t instance;
	/* (dynamic) */
	enum sccp_state state;

	/* special case of duplicated information from the config (static) */
	char name[SCCP_LINE_NAME_MAX];
};

/* limited to exactly 1 line for now, but is the way on to the support of multiple lines,
 * and more important, it offers symmetry with speeddial_group, so there's only one system
 * to understand
 */
struct line_group {
	struct sccp_line *line;
	size_t count;
};

enum sccp_device_state_id {
	STATE_NEW,
	STATE_REGISTERING,
	STATE_CONNLOST,
};

struct sccp_device_state {
	enum sccp_device_state_id id;
	void (*handle_msg)(struct sccp_device *device, struct sccp_msg *msg, uint32_t msg_id);
};

struct sccp_device {
	/* (static) */
	ast_mutex_t lock;

	struct speeddial_group sd_group;
	struct line_group line_group;

	/* (static) */
	struct sccp_msg_builder msg_builder;
	/* (dynamic) */
	struct sccp_queue nolock_tasks;
	/* (dynamic) */
	struct sockaddr_in remote;

	/* (static) */
	struct sccp_session *session;
	/* (dynamic) */
	struct sccp_device_cfg *cfg;
	/* (dynamic) */
	struct sccp_device_state *state;
	/* (dynamic) */
	struct ast_format_cap *caps;	/* Supported capabilities */
	/* (dynamic, modified in thread session only) */
	struct ast_event_sub *mwi_event_sub;
	/* (dynamic) */
	struct sccp_subchannel *active_subchan;

	uint32_t serial_callid;
	int open_receive_channel_pending;
	enum sccp_device_type type;
	uint8_t proto_version;

	/* if the device is a guest, then the name will be different then the
	 * device config name (static)
	 */
	char name[SCCP_DEVICE_NAME_MAX];
	char exten[AST_MAX_EXTENSION];
	char last_exten[AST_MAX_EXTENSION];
};

struct nolock_task_ast_queue_control {
	struct ast_channel *channel;
	enum ast_control_frame_type control;
};

struct nolock_task_ast_queue_hangup {
	struct ast_channel *channel;
};

struct nolock_task_init_channel {
	struct ast_channel *channel;
	struct sccp_line_cfg *line_cfg;
};

struct nolock_task_start_channel {
	struct ast_channel *channel;
	struct sccp_line_cfg *line_cfg;
};

union nolock_task_data {
	struct nolock_task_ast_queue_control queue_control;
	struct nolock_task_ast_queue_hangup queue_hangup;
	struct nolock_task_init_channel init_channel;
	struct nolock_task_start_channel start_channel;
};

struct nolock_task {
	union nolock_task_data data;
	void (*exec)(union nolock_task_data *data);
};

static void sccp_device_lock(struct sccp_device *device);
static void sccp_device_unlock(struct sccp_device *device);
static void sccp_device_panic(struct sccp_device *device);
static void subscribe_mwi(struct sccp_device *device);
static void unsubscribe_mwi(struct sccp_device *device);
static void subscribe_hints(struct sccp_device *device);
static void unsubscribe_hints(struct sccp_device *device);
static struct sccp_line *sccp_device_get_default_line(struct sccp_device *device);
static void handle_msg_state_common(struct sccp_device *device, struct sccp_msg *msg, uint32_t msg_id);
static void transmit_reset(struct sccp_device *device, enum sccp_reset_type type);
static void transmit_subchan_start_media_transmission(struct sccp_device *device, struct sccp_subchannel *subchan, struct sockaddr_in *endpoint);
static int add_dialtimeout_task(struct sccp_device *device, struct sccp_subchannel *subchan);
static void remove_dialtimeout_task(struct sccp_device *device, struct sccp_subchannel *subchan);

static struct sccp_device_state state_new = {
	.id = STATE_NEW,
	.handle_msg = NULL,
};

static struct sccp_device_state state_registering = {
	.id = STATE_REGISTERING,
	.handle_msg = handle_msg_state_common,
};

static struct sccp_device_state state_connlost = {
	.id = STATE_CONNLOST,
	.handle_msg = NULL,
};

static unsigned int chan_idx = 0;

static void sccp_speeddial_destructor(void *data)
{
	struct sccp_speeddial *sd = data;

	ast_log(LOG_DEBUG, "in destructor for speeddial %p\n", sd);

	ao2_ref(sd->device, -1);
	ao2_ref(sd->cfg, -1);
}

static struct sccp_speeddial *sccp_speeddial_alloc(struct sccp_speeddial_cfg *cfg, struct sccp_device *device, uint32_t instance, uint32_t index)
{
	struct sccp_speeddial *sd;

	sd = ao2_alloc_options(sizeof(*sd), sccp_speeddial_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!sd) {
		return NULL;
	}

	sd->device = device;
	ao2_ref(device, +1);
	sd->cfg = cfg;
	ao2_ref(cfg, +1);
	sd->instance = instance;
	sd->index = index;
	sd->state_id = -1;

	return sd;
}

static void sccp_speeddial_destroy(struct sccp_speeddial *sd)
{
	/* nothing to do for now */
	ast_log(LOG_DEBUG, "destroying speeddial %p\n", sd);
}

static int speeddial_group_init(struct speeddial_group *sd_group, struct sccp_device *device, uint32_t instance)
{
	struct sccp_speeddial **speeddials;
	struct sccp_speeddial_cfg **speeddials_cfg = device->cfg->speeddials_cfg;
	size_t i;
	size_t n = device->cfg->speeddial_count;
	uint32_t index = 1;

	if (!n) {
		sd_group->count = 0;
		return 0;
	}

	speeddials = ast_calloc(n, sizeof(*speeddials));
	if (!speeddials) {
		return -1;
	}

	for (i = 0; i < n; i++) {
		speeddials[i] = sccp_speeddial_alloc(speeddials_cfg[i], device, instance++, index++);
		if (!speeddials[i]) {
			goto error;
		}
	}

	sd_group->speeddials = speeddials;
	sd_group->count = n;

	return 0;

error:
	for (; i > 0; i--) {
		ao2_ref(speeddials[i - 1], -1);
	}

	ast_free(speeddials);

	return -1;
}

static void speeddial_group_deinit(struct speeddial_group *sd_group)
{
	size_t i;

	if (!sd_group->count) {
		return;
	}

	for (i = 0; i < sd_group->count; i++) {
		ao2_ref(sd_group->speeddials[i], -1);
	}

	ast_free(sd_group->speeddials);
}

static void speeddial_group_destroy(struct speeddial_group *sd_group)
{
	size_t i;

	for (i = 0; i < sd_group->count; i++) {
		sccp_speeddial_destroy(sd_group->speeddials[i]);
	}
}

static void sccp_subchannel_destructor(void *data)
{
	struct sccp_subchannel *subchan = data;

	ast_log(LOG_DEBUG, "in destructor for subchannel %u\n", subchan->id);

	if (subchan->channel) {
		/*
		 * This should not happen.
		 */
		ast_log(LOG_ERROR, "subchannel->channel is not null in destructor\n");
	}

	ao2_ref(subchan->line, -1);
}

static struct sccp_subchannel *sccp_subchannel_alloc(struct sccp_line *line, uint32_t id, enum sccp_direction direction)
{
	struct sccp_subchannel *subchan;

	subchan = ao2_alloc_options(sizeof(*subchan), sccp_subchannel_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!subchan) {
		return NULL;
	}

	subchan->line = line;
	ao2_ref(line, +1);
	subchan->channel = NULL;
	subchan->rtp = NULL;
	subchan->id = id;
	subchan->state = SCCP_OFFHOOK;
	subchan->direction = direction;
	subchan->resuming = 0;
	subchan->transferring = 0;

	return subchan;
}

static void sccp_subchannel_destroy(struct sccp_subchannel *subchan)
{
	ast_log(LOG_DEBUG, "destroying subchannel %u\n", subchan->id);

	/* TODO hangup the channel if there's one, but not here (because we might deadlock) */
}

/*
 * the device MUST be locked
 */
static int add_nolock_task(struct sccp_device *device, struct nolock_task *task)
{
	if (sccp_queue_put(&device->nolock_tasks, task)) {
		sccp_device_panic(device);
		return -1;
	}

	return 0;
}

/*
 * the device MUST NOT be locked
 */
static void exec_nolock_tasks(struct sccp_queue *tasks)
{
	struct nolock_task task;

	while (!sccp_queue_get(tasks, &task)) {
		task.exec(&task.data);
	}
}

static void exec_ast_queue_control(union nolock_task_data *data)
{
	struct ast_channel *channel = data->queue_control.channel;
	enum ast_control_frame_type control = data->queue_control.control;

	ast_queue_control(channel, control);

	ast_channel_unref(channel);
}

static int add_ast_queue_control_task(struct sccp_device *device, struct ast_channel *channel, enum ast_control_frame_type control)
{
	struct nolock_task task;

	task.exec = exec_ast_queue_control;
	task.data.queue_control.channel = channel;
	task.data.queue_control.control = control;

	if (add_nolock_task(device, &task)) {
		return -1;
	}

	ast_channel_ref(channel);

	return 0;
}

static void exec_ast_queue_hangup(union nolock_task_data *data)
{
	struct ast_channel *channel = data->queue_hangup.channel;

	ast_queue_hangup(channel);

	ast_channel_unref(channel);
}

static int add_ast_queue_hangup_task(struct sccp_device *device, struct ast_channel *channel)
{
	struct nolock_task task;

	task.exec = exec_ast_queue_hangup;
	task.data.queue_hangup.channel = channel;

	if (add_nolock_task(device, &task)) {
		return -1;
	}

	ast_channel_ref(channel);

	return 0;
}

static void exec_init_channel(union nolock_task_data *data)
{
	struct ast_channel *channel = data->init_channel.channel;
	struct sccp_line_cfg *line_cfg = data->init_channel.line_cfg;
	struct ast_variable *var_itr;
	char valuebuf[1024];

	for (var_itr = line_cfg->chanvars; var_itr; var_itr = var_itr->next) {
		ast_get_encoded_str(var_itr->value, valuebuf, sizeof(valuebuf));
		pbx_builtin_setvar_helper(channel, var_itr->name, valuebuf);
	}

	ast_channel_unref(channel);
	ao2_ref(line_cfg, -1);
}

static int add_init_channel_task(struct sccp_device *device, struct ast_channel *channel, struct sccp_line_cfg *line_cfg)
{
	struct nolock_task task;

	task.exec = exec_init_channel;
	task.data.init_channel.channel = channel;
	task.data.init_channel.line_cfg = line_cfg;

	if (add_nolock_task(device, &task)) {
		return -1;
	}

	ast_channel_ref(channel);
	ao2_ref(line_cfg, +1);

	return 0;
}

static void exec_start_channel(union nolock_task_data *data)
{
	struct ast_channel *channel = data->start_channel.channel;
	struct sccp_line_cfg *line_cfg = data->start_channel.line_cfg;

	ast_set_callerid(channel, line_cfg->cid_num, line_cfg->cid_name, NULL);

	ast_pbx_start(channel);

	ast_channel_unref(channel);
	ao2_ref(line_cfg, -1);
}

static int add_start_channel_task(struct sccp_device *device, struct ast_channel *channel, struct sccp_line_cfg *line_cfg)
{
	struct nolock_task task;

	task.exec = exec_start_channel;
	task.data.start_channel.channel = channel;
	task.data.start_channel.line_cfg = line_cfg;

	if (add_nolock_task(device, &task)) {
		return -1;
	}

	ast_channel_ref(channel);
	ao2_ref(line_cfg, +1);

	return 0;
}

static int sccp_subchannel_new_channel(struct sccp_subchannel *subchan, const char *linkedid, struct ast_format_cap *cap)
{
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	struct ast_channel *channel;
	RAII_VAR(struct ast_format_cap *, joint, ast_format_cap_alloc(), ast_format_cap_destroy);
	struct ast_format_cap *tmpcaps = NULL;
	int has_joint;
	char buf[256];

	if (subchan->channel) {
		ast_log(LOG_ERROR, "subchan already has a channel\n");
		return -1;
	}

	has_joint = ast_format_cap_joint_copy(line->cfg->caps, line->device->caps, joint);
	if (!has_joint) {
		ast_log(LOG_WARNING, "no compatible codecs\n");
		return -1;
	}

	if (cap && ast_format_cap_has_joint(joint, cap)) {
		tmpcaps = ast_format_cap_dup(joint);
		ast_format_cap_joint_copy(tmpcaps, cap, joint);
		ast_format_cap_destroy(tmpcaps);
	}

	ast_debug(1, "joint capabilities %s\n", ast_getformatname_multiple(buf, sizeof(buf), joint));

	channel = ast_channel_alloc(	1,				/* needqueue */
					AST_STATE_DOWN,			/* state */
					line->cfg->cid_num,		/* cid_num */
					line->cfg->cid_name,	/* cid_name */
					"code",				/* acctcode */
					device->exten,	/* exten */
					line->cfg->context,		/* context */
					linkedid,			/* linked ID */
					0,				/* amaflag */
					SCCP_LINE_PREFIX "/%s-%08x",
					line->name,
					ast_atomic_fetchadd_int((int *)&chan_idx, +1));

	if (!channel) {
		ast_log(LOG_ERROR, "channel allocation failed\n");
		return -1;
	}

	if (add_init_channel_task(device, channel, line->cfg)) {
		ast_channel_unref(channel);
		return -1;
	}

	ast_channel_tech_set(channel, &sccp_tech);
	ast_channel_tech_pvt_set(channel, subchan);
	ao2_ref(subchan, +1);
	subchan->channel = channel;

	if (!ast_strlen_zero(line->cfg->language)) {
		ast_channel_language_set(channel, line->cfg->language);
	}

	ast_codec_choose(&line->cfg->codec_pref, joint, 1, &subchan->fmt);
	ast_debug(1, "best codec %s\n", ast_getformatname(&subchan->fmt));

	ast_format_cap_set(ast_channel_nativeformats(channel), &subchan->fmt);
	ast_format_copy(ast_channel_writeformat(channel), &subchan->fmt);
	ast_format_copy(ast_channel_rawwriteformat(channel), &subchan->fmt);
	ast_format_copy(ast_channel_readformat(channel), &subchan->fmt);
	ast_format_copy(ast_channel_rawreadformat(channel), &subchan->fmt);

	return 0;
}

void sccp_subchannel_set_rtp_remote_address(struct sccp_subchannel *subchan)
{
	struct ast_sockaddr remote_tmp;

	ast_sockaddr_from_sin(&remote_tmp, &subchan->line->device->remote);
	ast_rtp_instance_set_remote_address(subchan->rtp, &remote_tmp);
}

void sccp_subchannel_get_rtp_local_address(struct sccp_subchannel *subchan, struct sockaddr_in *local)
{
	struct ast_sockaddr local_tmp;

	ast_rtp_instance_get_local_address(subchan->rtp, &local_tmp);
	ast_sockaddr_to_sin(&local_tmp, local);

	if (local->sin_addr.s_addr == 0) {
		local->sin_addr.s_addr = sccp_session_local_addr(subchan->line->device->session)->sin_addr.s_addr;
	}
}

static void sccp_subchannel_start_media_transmission(struct sccp_subchannel *subchan)
{
	struct sockaddr_in local;

	sccp_subchannel_set_rtp_remote_address(subchan);
	sccp_subchannel_get_rtp_local_address(subchan, &local);
	transmit_subchan_start_media_transmission(subchan->line->device, subchan, &local);
}

static void sccp_line_destructor(void *data)
{
	struct sccp_line *line = data;

	ast_log(LOG_DEBUG, "in destructor for line %s\n", line->name);

	ao2_ref(line->device, -1);
	ao2_ref(line->cfg, -1);
}

static struct sccp_line *sccp_line_alloc(struct sccp_line_cfg *cfg, struct sccp_device *device, uint32_t instance)
{
	struct sccp_line *line;

	line = ao2_alloc_options(sizeof(*line), sccp_line_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!line) {
		return NULL;
	}

	AST_LIST_HEAD_INIT_NOLOCK(&line->subchans);
	line->device = device;
	ao2_ref(device, +1);
	line->cfg = cfg;
	ao2_ref(cfg, +1);
	line->instance = instance;
	line->state = SCCP_ONHOOK;
	ast_copy_string(line->name, cfg->name, sizeof(line->name));

	return line;
}

static void sccp_line_destroy(struct sccp_line *line)
{
	struct sccp_subchannel *subchan;

	ast_log(LOG_DEBUG, "destroying line %s\n", line->name);

	AST_LIST_TRAVERSE_SAFE_BEGIN(&line->subchans, subchan, list) {
		AST_LIST_REMOVE_CURRENT(list);
		sccp_subchannel_destroy(subchan);
		ao2_ref(subchan, -1);
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

/*
 * reference count is NOT incremented
 */
static struct sccp_subchannel *sccp_line_new_subchannel(struct sccp_line *line, enum sccp_direction direction)
{
	struct sccp_subchannel *subchan;

	subchan = sccp_subchannel_alloc(line, line->device->serial_callid++, direction);
	if (!subchan) {
		return NULL;
	}

	/* add subchannel to line */
	AST_LIST_INSERT_TAIL(&line->subchans, subchan, list);

	return subchan;
}

static int line_group_init(struct line_group *line_group, struct sccp_device *device, uint32_t instance)
{
	struct sccp_line *line;

	/* A: device->cfg has exactly one line */

	line = sccp_line_alloc(device->cfg->line_cfg, device, instance);
	if (!line) {
		return -1;
	}

	line_group->line = line;
	line_group->count = 1;

	return 0;
}

static void line_group_deinit(struct line_group *line_group)
{
	ao2_ref(line_group->line, -1);
}

static void line_group_destroy(struct line_group *line_group)
{
	sccp_line_destroy(line_group->line);
}

static void sccp_device_destructor(void *data)
{
	struct sccp_device *device = data;

	ast_log(LOG_DEBUG, "in destructor for device %s\n", device->name);

	/* no, it is NOT missing an line_group_deinit(&device->line) nor a
	 * speeddial_group_deinit(&device->group). Only completely created
	 * device object have these field initialized, and completely created
	 * device object must be destroyed via sccp_device_destroy
	 */

	sccp_queue_destroy(&device->nolock_tasks);
	ast_mutex_destroy(&device->lock);
	ast_format_cap_destroy(device->caps);
	ao2_ref(device->session, -1);
	ao2_ref(device->cfg, -1);
}

static int device_type_is_supported(enum sccp_device_type device_type)
{
	int supported;

	switch (device_type) {
	case SCCP_DEVICE_7905:
	case SCCP_DEVICE_7906:
	case SCCP_DEVICE_7911:
	case SCCP_DEVICE_7912:
	case SCCP_DEVICE_7920:
	case SCCP_DEVICE_7921:
	case SCCP_DEVICE_7931:
	case SCCP_DEVICE_7937:
	case SCCP_DEVICE_7940:
	case SCCP_DEVICE_7941:
	case SCCP_DEVICE_7941GE:
	case SCCP_DEVICE_7942:
	case SCCP_DEVICE_7960:
	case SCCP_DEVICE_7961:
	case SCCP_DEVICE_7962:
	case SCCP_DEVICE_7970:
	case SCCP_DEVICE_CIPC:
		supported = 1;
		break;
	default:
		supported = 0;
		break;
	}

	return supported;
}

static struct sccp_device *sccp_device_alloc(struct sccp_device_cfg *cfg, struct sccp_session *session, struct sccp_device_info *info)
{
	struct ast_format_cap *caps;
	struct sccp_device *device;

	caps = ast_format_cap_alloc_nolock();
	if (!caps) {
		return NULL;
	}

	device = ao2_alloc_options(sizeof(*device), sccp_device_destructor, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!device) {
		ast_format_cap_destroy(caps);
		return NULL;
	}

	ast_mutex_init(&device->lock);
	sccp_msg_builder_init(&device->msg_builder, info->type, info->proto_version);
	sccp_queue_init(&device->nolock_tasks, sizeof(struct nolock_task));
	device->session = session;
	ao2_ref(session, +1);
	device->cfg = cfg;
	ao2_ref(cfg, +1);
	device->state = &state_new;
	device->caps = caps;
	device->mwi_event_sub = NULL;
	device->active_subchan = NULL;
	device->serial_callid = 1;
	device->open_receive_channel_pending = 0;
	device->type = info->type;
	device->proto_version = info->proto_version;
	ast_copy_string(device->name, info->name, sizeof(device->name));
	device->exten[0] = '\0';
	device->last_exten[0] = '\0';

	return device;
}

struct sccp_device *sccp_device_create(struct sccp_device_cfg *device_cfg, struct sccp_session *session, struct sccp_device_info *info)
{
	struct sccp_device *device;

	if (!device_cfg) {
		ast_log(LOG_ERROR, "sccp device create failed: device_cfg is null\n");
		return NULL;
	}

	if (!session) {
		ast_log(LOG_ERROR, "sccp device create failed: session is null\n");
		return NULL;
	}

	if (!info) {
		ast_log(LOG_ERROR, "sccp device create failed: info is null\n");
		return NULL;
	}

	if (!device_type_is_supported(info->type)) {
		ast_log(LOG_WARNING, "Rejecting [%s], unsupported device type [%d]\n", info->name, info->type);
		return NULL;
	}

	device = sccp_device_alloc(device_cfg, session, info);
	if (!device) {
		return NULL;
	}

	if (line_group_init(&device->line_group, device, LINE_INSTANCE_START)) {
		ao2_ref(device, -1);
		return NULL;
	}

	if (speeddial_group_init(&device->sd_group, device, LINE_INSTANCE_START + device->line_group.count)) {
		line_group_deinit(&device->line_group);
		ao2_ref(device, -1);
		return NULL;
	}

	return device;
}

/*
 * entry point: yes
 * thread: session
 */
void sccp_device_destroy(struct sccp_device *device)
{
	ast_log(LOG_DEBUG, "destroying device %s\n", device->name);

	unsubscribe_mwi(device);
	unsubscribe_hints(device);

	sccp_device_lock(device);

	device->active_subchan = NULL;

	line_group_destroy(&device->line_group);
	line_group_deinit(&device->line_group);

	speeddial_group_destroy(&device->sd_group);
	speeddial_group_deinit(&device->sd_group);

	switch (device->state->id) {
	case STATE_NEW:
	case STATE_CONNLOST:
		break;
	default:
		transmit_reset(device, SCCP_RESET_SOFT);
		break;
	}

	sccp_device_unlock(device);
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_subchannel *sccp_device_get_subchan(struct sccp_device *device, uint32_t subchan_id)
{
	struct sccp_line *line = sccp_device_get_default_line(device);
	struct sccp_subchannel *subchan;

	AST_LIST_TRAVERSE(&line->subchans, subchan, list) {
		if (subchan->id == subchan_id) {
			return subchan;
		}
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_subchannel *sccp_device_get_next_ringin_subchan(struct sccp_device *device)
{
	struct sccp_line *line = sccp_device_get_default_line(device);
	struct sccp_subchannel *subchan;

	AST_LIST_TRAVERSE(&line->subchans, subchan, list) {
		if (subchan->state == SCCP_RINGIN) {
			return subchan;
		}
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_subchannel *sccp_device_get_next_offhook_subchan(struct sccp_device *device)
{
	struct sccp_line *line = sccp_device_get_default_line(device);
	struct sccp_subchannel *subchan;

	AST_LIST_TRAVERSE(&line->subchans, subchan, list) {
		if (subchan->state == SCCP_OFFHOOK) {
			return subchan;
		}
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_line *sccp_device_get_line(struct sccp_device *device, uint32_t instance)
{
	struct sccp_line *line = device->line_group.line;

	if (line->instance == instance) {
		return line;
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_line *sccp_device_get_default_line(struct sccp_device *device)
{
	return device->line_group.line;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_speeddial *sccp_device_get_speeddial(struct sccp_device *device, uint32_t instance)
{
	struct sccp_speeddial **speeddials = device->sd_group.speeddials;
	size_t i;

	for (i = 0; i < device->sd_group.count; i++) {
		if (speeddials[i]->instance == instance) {
			return speeddials[i];
		}
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_speeddial *sccp_device_get_speeddial_by_index(struct sccp_device *device, uint32_t index)
{
	struct sccp_speeddial **speeddials = device->sd_group.speeddials;
	size_t i;

	for (i = 0; i < device->sd_group.count; i++) {
		if (speeddials[i]->index == index) {
			return speeddials[i];
		}
	}

	return NULL;
}

static void sccp_device_lock(struct sccp_device *device)
{
	ast_mutex_lock(&device->lock);
}

static void sccp_device_unlock(struct sccp_device *device)
{
	struct sccp_queue tasks;

	if (sccp_queue_empty(&device->nolock_tasks)) {
		ast_mutex_unlock(&device->lock);
		return;
	}

	sccp_queue_move(&tasks, &device->nolock_tasks);
	ast_mutex_unlock(&device->lock);

	exec_nolock_tasks(&tasks);
	sccp_queue_destroy(&tasks);
}

static enum sccp_codecs codec_ast2sccp(struct ast_format *format)
{
	switch (format->id) {
	case AST_FORMAT_ALAW:
		return SCCP_CODEC_G711_ALAW;
	case AST_FORMAT_ULAW:
		return SCCP_CODEC_G711_ULAW;
	case AST_FORMAT_G723_1:
		return SCCP_CODEC_G723_1;
	case AST_FORMAT_G729A:
		return SCCP_CODEC_G729A;
	case AST_FORMAT_G726_AAL2:
		return SCCP_CODEC_G726_32;
	case AST_FORMAT_H261:
		return SCCP_CODEC_H261;
	case AST_FORMAT_H263:
		return SCCP_CODEC_H263;
	default:
		return -1;
	}
}

static void codec_sccp2ast(enum sccp_codecs sccpcodec, struct ast_format *result)
{
	switch (sccpcodec) {
	case SCCP_CODEC_G711_ALAW:
		ast_format_set(result, AST_FORMAT_ALAW, 0);
		break;
	case SCCP_CODEC_G711_ULAW:
		ast_format_set(result, AST_FORMAT_ULAW, 0);
		break;
	case SCCP_CODEC_G723_1:
		ast_format_set(result, AST_FORMAT_G723_1, 0);
		break;
	case SCCP_CODEC_G729A:
		ast_format_set(result, AST_FORMAT_G729A, 0);
		break;
	case SCCP_CODEC_H261:
		ast_format_set(result, AST_FORMAT_H261, 0);
		break;
	case SCCP_CODEC_H263:
		ast_format_set(result, AST_FORMAT_H263, 0);
		break;
	default:
		ast_format_clear(result);
		break;
	}
}

static void transmit_button_template_res(struct sccp_device *device)
{
	struct sccp_msg msg;
	struct button_definition definition[MAX_BUTTON_DEFINITION];
	struct sccp_speeddial **speeddials = device->sd_group.speeddials;
	size_t n = 0;
	size_t i;

	/* add the line */
	definition[n].buttonDefinition = BT_LINE;
	definition[n].lineInstance = device->line_group.line->instance;
	n++;

	/* add the speeddials */
	for (i = 0; i < device->sd_group.count && n < MAX_BUTTON_DEFINITION; i++) {
		definition[n].buttonDefinition = BT_FEATUREBUTTON;
		definition[n].lineInstance = speeddials[i]->instance;
		n++;
	}

	sccp_msg_button_template_res(&msg, definition, n);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_callinfo(struct sccp_device *device, const char *from_name, const char *from_num, const char *to_name, const char *to_num, uint32_t line_instance, uint32_t callid, enum sccp_direction direction)
{
	struct sccp_msg msg;

	sccp_msg_builder_callinfo(&device->msg_builder, &msg, from_name, from_num, to_name, to_num, line_instance, callid, direction);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_capabilities_req(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_capabilities_req(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_close_receive_channel(struct sccp_device *device, uint32_t callid)
{
	struct sccp_msg msg;

	sccp_msg_close_receive_channel(&msg, callid);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_config_status_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_config_status_res(&msg, device->name, device->line_group.count, device->sd_group.count);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_clear_message(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_clear_message(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_dialed_number(struct sccp_device *device, const char *extension, uint32_t line_instance, uint32_t callid)
{
	struct sccp_msg msg;

	sccp_msg_dialed_number(&msg, extension, line_instance, callid);
	sccp_session_transmit_msg(device->session, &msg);
}

static enum sccp_blf_status extstate_ast2sccp(int state)
{
	switch (state) {
	case AST_EXTENSION_DEACTIVATED:
		return SCCP_BLF_STATUS_UNKNOWN;
	case AST_EXTENSION_REMOVED:
		return SCCP_BLF_STATUS_UNKNOWN;
	case AST_EXTENSION_RINGING:
		return SCCP_BLF_STATUS_ALERTING;
	case AST_EXTENSION_INUSE | AST_EXTENSION_RINGING:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_UNAVAILABLE:
		return SCCP_BLF_STATUS_UNKNOWN;
	case AST_EXTENSION_BUSY:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_INUSE:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_ONHOLD:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_INUSE | AST_EXTENSION_ONHOLD:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_NOT_INUSE:
		return SCCP_BLF_STATUS_IDLE;
	default:
		return SCCP_BLF_STATUS_UNKNOWN;
	}
}

static void transmit_feature_status(struct sccp_device *device, struct sccp_speeddial *sd)
{
	struct sccp_msg msg;
	enum sccp_blf_status status = SCCP_BLF_STATUS_UNKNOWN;

	if (sd->cfg->blf) {
		status = extstate_ast2sccp(sd->state);
	}

	sccp_msg_feature_status(&msg, sd->instance, BT_FEATUREBUTTON, status, sd->cfg->label);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_forward_status_res(struct sccp_device *device, struct sccp_line *line)
{
	struct sccp_msg msg;

	sccp_msg_forward_status_res(&msg, line->instance, "", 0);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_keep_alive_ack(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_keep_alive_ack(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_lamp_state(struct sccp_device *device, enum sccp_stimulus_type stimulus, uint32_t instance, enum sccp_lamp_state indication)
{
	struct sccp_msg msg;

	sccp_msg_lamp_state(&msg, stimulus, instance, indication);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_line_status_res(struct sccp_device *device, struct sccp_line *line)
{
	struct sccp_msg msg;
	struct sccp_line_cfg *line_cfg = line->cfg;

	sccp_msg_builder_line_status_res(&device->msg_builder, &msg, line_cfg->cid_name, line_cfg->cid_num, line->instance);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_register_ack(struct sccp_device *device)
{
	struct sccp_msg msg;
	struct sccp_device_cfg *device_cfg = device->cfg;

	sccp_msg_builder_register_ack(&device->msg_builder, &msg, device_cfg->dateformat, device_cfg->keepalive);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_reset(struct sccp_device *device, enum sccp_reset_type type)
{
	struct sccp_msg msg;

	sccp_msg_reset(&msg, type);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_ringer_mode(struct sccp_device *device, enum sccp_ringer_mode mode)
{
	struct sccp_msg msg;

	sccp_msg_ringer_mode(&msg, mode);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_selectsoftkeys(struct sccp_device *device, uint32_t line_instance, uint32_t callid, enum sccp_softkey_status softkey)
{
	struct sccp_msg msg;

	sccp_msg_select_softkeys(&msg, line_instance, callid, softkey);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_speeddial_stat_res(struct sccp_device *device, struct sccp_speeddial *sd)
{
	struct sccp_msg msg;

	sccp_msg_speeddial_stat_res(&msg, sd->index, sd->cfg->extension, sd->cfg->label);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_softkey_set_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_softkey_set_res(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_softkey_template_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_softkey_template_res(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_speaker_mode(struct sccp_device *device, enum sccp_speaker_mode mode)
{
	struct sccp_msg msg;

	sccp_msg_speaker_mode(&msg, mode);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_stop_media_transmission(struct sccp_device *device, uint32_t callid)
{
	struct sccp_msg msg;

	sccp_msg_stop_media_transmission(&msg, callid);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_stop_tone(struct sccp_device *device, uint32_t line_instance, uint32_t callid)
{
	struct sccp_msg msg;

	sccp_msg_stop_tone(&msg, line_instance, callid);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_time_date_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_time_date_res(&msg);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_tone(struct sccp_device *device, enum sccp_tone tone, uint32_t line_instance, uint32_t callid)
{
	struct sccp_msg msg;

	sccp_msg_tone(&msg, tone, line_instance, callid);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_line_lamp_state(struct sccp_device *device, struct sccp_line *line, enum sccp_lamp_state indication)
{
	transmit_lamp_state(device, STIMULUS_LINE, line->instance, indication);
}

static void transmit_subchan_callstate(struct sccp_device *device, struct sccp_subchannel *subchan, enum sccp_state state)
{
	struct sccp_msg msg;

	sccp_msg_callstate(&msg, state, subchan->line->instance, subchan->id);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_subchan_open_receive_channel(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	struct sccp_msg msg;
	struct ast_format_list fmt;

	if (device->open_receive_channel_pending) {
		ast_debug(1, "open_receive_channel already sent\n");
		return;
	}

	device->open_receive_channel_pending = 1;

	fmt = ast_codec_pref_getsize(&subchan->line->cfg->codec_pref, &subchan->fmt);
	sccp_msg_open_receive_channel(&msg, subchan->id, fmt.cur_ms, codec_ast2sccp(&fmt.format));
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_subchan_selectsoftkeys(struct sccp_device *device, struct sccp_subchannel *subchan, enum sccp_softkey_status softkey)
{
	transmit_selectsoftkeys(device, subchan->line->instance, subchan->id, softkey);
}

static void transmit_subchan_start_media_transmission(struct sccp_device *device, struct sccp_subchannel *subchan, struct sockaddr_in *endpoint)
{
	struct sccp_msg msg;
	struct ast_format_list fmt;

	fmt = ast_codec_pref_getsize(&subchan->line->cfg->codec_pref, &subchan->fmt);

	ast_debug(2, "Sending start media transmission to %s: %s %d\n", sccp_session_remote_addr_ch(device->session), ast_inet_ntoa(endpoint->sin_addr), ntohs(endpoint->sin_port));
	sccp_msg_start_media_transmission(&msg, subchan->id, fmt.cur_ms, codec_ast2sccp(&fmt.format), subchan->line->cfg->tos_audio, endpoint);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_subchan_stop_tone(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	transmit_stop_tone(device, subchan->line->instance, subchan->id);
}

static void transmit_subchan_tone(struct sccp_device *device, struct sccp_subchannel *subchan, enum sccp_tone tone)
{
	transmit_tone(device, tone, subchan->line->instance, subchan->id);
}

static void transmit_voicemail_lamp_state(struct sccp_device *device, int new_msgs)
{
	enum sccp_lamp_state indication = new_msgs ? SCCP_LAMP_ON : SCCP_LAMP_OFF;

	transmit_lamp_state(device, STIMULUS_VOICEMAIL, 0, indication);
}

static void handle_msg_button_template_req(struct sccp_device *device)
{
	transmit_button_template_res(device);
}

static void sccp_device_panic(struct sccp_device *device)
{
	ast_log(LOG_WARNING, "panic for device %s\n", device->name);

	transmit_reset(device, SCCP_RESET_HARD_RESTART);
	sccp_session_stop(device->session);
	/* XXX put into a "panic" state, instead of the conn lost state (which is the closest we have right now) */
	device->state = &state_connlost;
}

/*
 * entry point: yes
 * thread: event processing thread
 */
static void on_mwi_event(const struct ast_event *event, void *data)
{
	struct sccp_device *device = data;
	int new_msgs = ast_event_get_ie_uint(event, AST_EVENT_IE_NEWMSGS);

	sccp_device_lock(device);
	transmit_voicemail_lamp_state(device, new_msgs);
	sccp_device_unlock(device);
}

/*
 * thread: session
 * locked: MUST NOT
 */
static void subscribe_mwi(struct sccp_device *device)
{
	if (ast_strlen_zero(device->cfg->voicemail)) {
		return;
	}

	device->mwi_event_sub = ast_event_subscribe(AST_EVENT_MWI, on_mwi_event, "sccp mwi subsciption", device,
			AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR, device->cfg->voicemail,
			AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR, device->line_group.line->cfg->context,
			AST_EVENT_IE_NEWMSGS, AST_EVENT_IE_PLTYPE_EXISTS,
			AST_EVENT_IE_END);
	if (!device->mwi_event_sub) {
		ast_log(LOG_WARNING, "device %s subscribe mwi failed\n", device->name);
	}
}

/*
 * thread: session
 * locked: MUST NOT
 */
static void unsubscribe_mwi(struct sccp_device *device)
{
	if (device->mwi_event_sub) {
		ast_event_unsubscribe(device->mwi_event_sub);
	}
}

/*
 * entry point: yes
 * thread: XXX good question, but not session thread, and not pbx thread I supose
 */
static int on_hint_state_change(char *context, char *id, struct ast_state_cb_info *info, void *data)
{
	struct sccp_speeddial *sd = data;
	struct sccp_device *device = sd->device;

	sccp_device_lock(device);
	sd->state = info->exten_state;

	transmit_feature_status(device, sd);
	sccp_device_unlock(device);

	return 0;
}

/*
 * thread: session
 * locked: MUST NOT
 */
static void subscribe_hints(struct sccp_device *device)
{
	struct sccp_speeddial *sd;
	char *context = device->line_group.line->cfg->context;
	size_t i;

	for (i = 0; i < device->sd_group.count; i++) {
		sd = device->sd_group.speeddials[i];
		if (sd->cfg->blf) {
			sd->state = ast_extension_state(NULL, context, sd->cfg->extension);
			sd->state_id = ast_extension_state_add(context, sd->cfg->extension, on_hint_state_change, sd);
			if (sd->state_id == -1) {
				ast_log(LOG_WARNING, "Could not subscribe to %s@%s\n", sd->cfg->extension, context);
			}
		}
	}
}

/*
 * thread: session
 * locked: MUST NOT
 */
static void unsubscribe_hints(struct sccp_device *device)
{
	struct sccp_speeddial *sd;
	size_t i;

	for (i = 0; i < device->sd_group.count; i++) {
		sd = device->sd_group.speeddials[i];
		if (sd->cfg->blf && sd->state_id != -1) {
			ast_extension_state_del(sd->state_id, NULL);
		}
	}
}

/*
 * device must have an active_subchan
 */
static int do_hold(struct sccp_device *device)
{
	struct sccp_subchannel *subchan = device->active_subchan;

	if (subchan->channel) {
		if (add_ast_queue_control_task(device, subchan->channel, AST_CONTROL_HOLD)) {
			return -1;
		}
	}

	if (subchan->rtp) {
		ast_rtp_instance_stop(subchan->rtp);
		ast_sockaddr_setnull(&subchan->direct_media_addr);
	}

	transmit_subchan_callstate(device, subchan, SCCP_HOLD);
	transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_ONHOLD);

	/* close our speaker */
	transmit_speaker_mode(device, SCCP_SPEAKEROFF);

	/* stop audio stream */
	transmit_close_receive_channel(device, subchan->id);
	transmit_stop_media_transmission(device, subchan->id);

	subchan->on_hold = 1;

	device->active_subchan = NULL;

	return 0;
}

/*
 * device must have no active_subchan
 */
static int do_resume(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	if (subchan->channel) {
		if (add_ast_queue_control_task(device, subchan->channel, AST_CONTROL_UNHOLD)) {
			return -1;
		}
	}

	subchan->line->state = SCCP_CONNECTED;

	/* put on connected */
	transmit_subchan_callstate(device, subchan, SCCP_CONNECTED);
	transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_CONNECTED);

	/* open our speaker */
	transmit_speaker_mode(device, SCCP_SPEAKERON);

	/* restart the audio stream, which has been stopped in handle_softkey_hold */
	if (subchan->rtp) {
		subchan->resuming = 1;
		transmit_subchan_open_receive_channel(device, subchan);
	}

	subchan->on_hold = 0;

	device->active_subchan = subchan;

	return 0;
}

static struct sccp_subchannel *do_newcall(struct sccp_device *device)
{
	struct sccp_line *line = sccp_device_get_default_line(device);
	struct sccp_subchannel *subchan;

	subchan = sccp_device_get_next_offhook_subchan(device);
	if (subchan) {
		ast_log(LOG_DEBUG, "Found an already offhook subchan\n");
		return subchan;
	}

	if (device->active_subchan) {
		if (do_hold(device)) {
			ast_log(LOG_NOTICE, "do newcall failed: could not put active subchan on hold\n");
			return NULL;
		}
	}

	subchan = sccp_line_new_subchannel(line, SCCP_DIR_OUTGOING);
	if (!subchan) {
		return NULL;
	}

	device->active_subchan = subchan;
	line->state = SCCP_OFFHOOK;

	transmit_line_lamp_state(device, subchan->line, SCCP_LAMP_ON);
	transmit_subchan_callstate(device, subchan, SCCP_OFFHOOK);
	transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_OFFHOOK);
	transmit_subchan_tone(device, subchan, SCCP_TONE_DIAL);

	/* TODO update line devstate */

	return subchan;
}

static int do_answer(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;

	if (!subchan->channel) {
		ast_log(LOG_NOTICE, "do answer failed: subchan has no channel\n");
		return -1;
	}

	if (device->active_subchan) {
		if (do_hold(device)) {
			ast_log(LOG_NOTICE, "do answer failed: could not put active subchan on hold\n");
			return -1;
		}
	}

	device->active_subchan = subchan;

	transmit_ringer_mode(device, SCCP_RING_OFF);
	transmit_subchan_callstate(device, subchan, SCCP_OFFHOOK);
	transmit_subchan_callstate(device, subchan, SCCP_CONNECTED);
	transmit_subchan_stop_tone(device, subchan);
	transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_CONNECTED);
	transmit_subchan_open_receive_channel(device, subchan);

	line->state = SCCP_CONNECTED;
	subchan->state = SCCP_CONNECTED;

	/* TODO update line devstate */

	return 0;
}

static void do_clear_subchannel(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;

	/* XXX hum, that's a bit ugly */

	if (subchan->rtp) {
		transmit_close_receive_channel(device, subchan->id);
		transmit_stop_media_transmission(device, subchan->id);

		ast_rtp_instance_stop(subchan->rtp);
		ast_rtp_instance_destroy(subchan->rtp);
		subchan->rtp = NULL;
	}

	transmit_ringer_mode(device, SCCP_RING_OFF);
	transmit_subchan_callstate(device, subchan, SCCP_ONHOOK);
	transmit_subchan_stop_tone(device, subchan);

	subchan->channel = NULL;

	/* the ref is decremented at the end of the function */
	AST_LIST_REMOVE(&line->subchans, subchan, list);
	if (AST_LIST_EMPTY(&line->subchans)) {
		transmit_speaker_mode(device, SCCP_SPEAKEROFF);
		/* TODO update line devstate */
	}

	if (subchan == device->active_subchan) {
		device->active_subchan = NULL;
	}

	ao2_ref(subchan, -1);
}

static int do_hangup(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	device->exten[0] = '\0';

	remove_dialtimeout_task(device, subchan);

	if (subchan->channel) {
		if (subchan->state == SCCP_RINGIN) {
			ast_channel_hangupcause_set(subchan->channel, AST_CAUSE_BUSY);
		}

		if (add_ast_queue_hangup_task(device, subchan->channel)) {
			return -1;
		}
	} else {
		do_clear_subchannel(device, subchan);
		/* XXX subchan is now invalid here if the caller had no reference */
	}

	return 0;
}

static int start_the_call(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;

	remove_dialtimeout_task(device, subchan);

	if (sccp_subchannel_new_channel(subchan, NULL, NULL)) {
		do_clear_subchannel(device, subchan);

		return -1;
	}

	line->state = SCCP_RINGOUT;
	subchan->state = SCCP_RINGOUT;
	ast_setstate(subchan->channel, AST_STATE_RING);
	if (subchan->transferring) {
		transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_CONNINTRANSFER);
	} else {
		transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_RINGOUT);
	}

	transmit_dialed_number(device, device->exten, line->instance, subchan->id);
	transmit_subchan_callstate(device, subchan, SCCP_PROGRESS);
	transmit_subchan_stop_tone(device, subchan);
	transmit_subchan_tone(device, subchan, SCCP_TONE_ALERT);
	transmit_callinfo(device, "", line->cfg->cid_num, "", line->device->exten, line->instance, subchan->id, subchan->direction);

	memcpy(line->device->last_exten, line->device->exten, AST_MAX_EXTENSION);
	line->device->exten[0] = '\0';

	if (add_start_channel_task(device, subchan->channel, line->cfg)) {
		return -1;
	}

	return 0;
}

static void handle_msg_capabilities_res(struct sccp_device *device, struct sccp_msg *msg)
{
	struct ast_format format;
	uint32_t count = letohl(msg->data.caps.count);
	uint32_t sccpcodec;
	uint32_t i;

	if (count > SCCP_MAX_CAPABILITIES) {
		count = SCCP_MAX_CAPABILITIES;
		ast_log(LOG_WARNING, "Received more capabilities (%d) than we can handle (%d)\n", count, SCCP_MAX_CAPABILITIES);
	}

	ast_format_cap_remove_all(device->caps);

	for (i = 0; i < count; i++) {
		sccpcodec = letohl(msg->data.caps.caps[i].codec);
		codec_sccp2ast(sccpcodec, &format);

		ast_format_cap_add(device->caps, &format);
	}
}

static void handle_msg_config_status_req(struct sccp_device *device)
{
	transmit_config_status_res(device);
}

static void handle_msg_enbloc_call(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_subchannel *subchan = sccp_device_get_next_offhook_subchan(device);
	size_t len;

	/* XXX this 2 steps stuff should be simplified */
	if (subchan) {
		ast_copy_string(device->exten, msg->data.enbloc.extension, sizeof(device->exten));

		/* allow a terminating '#' character */
		len = strlen(device->exten);
		if (len > 0 && device->exten[len - 1] == '#') {
			device->exten[len - 1] = '\0';
		}

		start_the_call(device, subchan);
	}
}

static void handle_msg_feature_status_req(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_speeddial *speeddial;
	uint32_t instance = letohl(msg->data.feature.instance);

	speeddial = sccp_device_get_speeddial(device, instance);
	if (!speeddial) {
		ast_log(LOG_DEBUG, "No speeddial [%d] on device [%s]\n", instance, device->name);
		sccp_session_stop(device->session);
		return;
	}

	transmit_feature_status(device, speeddial);
}

static void handle_msg_keep_alive(struct sccp_device *device)
{
	transmit_keep_alive_ack(device);
}

static void handle_msg_keypad_button(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_line *line;
	struct sccp_subchannel *subchan;
	uint32_t button;
	uint32_t instance;
	size_t len;
	char digit;

	button = letohl(msg->data.keypad.button);
	instance = letohl(msg->data.keypad.lineInstance);

	switch (device->type) {
	case SCCP_DEVICE_7905:
	case SCCP_DEVICE_7912:
	case SCCP_DEVICE_7920:
		line = sccp_device_get_default_line(device);
		break;
	default:
		line = sccp_device_get_line(device, instance);
		break;
	}

	if (!line) {
		ast_log(LOG_DEBUG, "Device [%s] has no line instance [%d]\n", device->name, instance);
		return;
	}

	if (button == 14) {
		digit = '*';
	} else if (button == 15) {
		digit = '#';
	} else if (button <= 9) {
		digit = '0' + button;
	} else {
		digit = '0' + button;
		ast_log(LOG_WARNING, "Unsupported digit %d\n", button);
	}

	/* TODO add dtmf stuff */

	if (line->state == SCCP_OFFHOOK) {
		len = strlen(device->exten);
		if (len < sizeof(device->exten) - 1 && digit != '#') {
			device->exten[len] = digit;
			device->exten[len+1] = '\0';
		}

		/* TODO add the callforward stuff */

		subchan = device->active_subchan;
		if (!subchan) {
			ast_log(LOG_WARNING, "active subchan is NULL, ignoring keypad button\n");
			return;
		}

		if (!len) {
			/* XXX we are not using line->instance, which works ok on 7912, 7940, 7942,
			 * but need more testing
			 */
			transmit_tone(device, SCCP_TONE_NONE, 0, 0);
			transmit_stop_tone(device, 0, 0);
		}

		if (digit == '#') {
			start_the_call(device, subchan);
		} else {
			add_dialtimeout_task(device, subchan);
		}

		/* TODO add dialtimeout stuff */
	}
}

static void handle_msg_line_status_req(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_line *line;
	uint32_t instance = letohl(msg->data.line.lineInstance);

	line = sccp_device_get_line(device, instance);
	if (!line) {
		ast_log(LOG_DEBUG, "Line instance [%d] is not attached to device [%s]\n", instance, device->name);
		sccp_session_stop(device->session);
		return;
	}

	transmit_line_status_res(device, line);
	transmit_forward_status_res(device, line);
}

static void handle_msg_onhook(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_subchannel *subchan;
	uint32_t subchan_id;

	if (device->proto_version == 11) {
		subchan_id = letohl(msg->data.onhook.callInstance);
		subchan = sccp_device_get_subchan(device, subchan_id);
		if (!subchan) {
			ast_log(LOG_NOTICE, "handle msg onhook failed: no subchan %u\n", subchan_id);
			return;
		}
	} else {
		subchan = device->active_subchan;
		if (!subchan) {
			ast_log(LOG_NOTICE, "handle msg onhook failed: no active subchan\n");
			return;
		}
	}

	do_hangup(device, subchan);
}

static void handle_msg_offhook(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_subchannel *subchan;
	uint32_t subchan_id;

	if (device->proto_version >= 11) {
		if (!msg->data.offhook.lineInstance) {
			do_newcall(device);
		} else {
			subchan_id = letohl(msg->data.offhook.callInstance);
			subchan = sccp_device_get_subchan(device, subchan_id);
			if (!subchan) {
				ast_log(LOG_NOTICE, "handle msg offhook failed: no subchan %u\n", subchan_id);
				return;
			}

			do_answer(device, subchan);
		}
	} else {
		subchan = sccp_device_get_next_ringin_subchan(device);
		if (subchan) {
			do_answer(device, subchan);
		} else if (!device->active_subchan) {
			do_newcall(device);
		}
	}
}

static void subchan_init_rtp_instance(struct sccp_subchannel *subchan)
{
	ast_rtp_instance_set_prop(subchan->rtp, AST_RTP_PROPERTY_RTCP, 1);

	if (subchan->channel) {
		ast_channel_set_fd(subchan->channel, 0, ast_rtp_instance_fd(subchan->rtp, 0));
		ast_channel_set_fd(subchan->channel, 1, ast_rtp_instance_fd(subchan->rtp, 1));
	}

	ast_rtp_instance_set_qos(subchan->rtp, subchan->line->cfg->tos_audio, 0, "sccp rtp");
	ast_rtp_instance_set_prop(subchan->rtp, AST_RTP_PROPERTY_NAT, 0);

	/*
	 *  hack that add the 0 payload type (i.e. the G.711 mu-law payload type) to the list
	 *  of payload that the rtp_instance knows about. It works because currently, we are
	 *  always using G.711 mu-law, alaw or g729 (0, 8, 18)
	 */
	ast_rtp_codecs_payloads_set_m_type(ast_rtp_instance_get_codecs(subchan->rtp), subchan->rtp, 0);
	ast_rtp_codecs_payloads_set_m_type(ast_rtp_instance_get_codecs(subchan->rtp), subchan->rtp, 8);
	ast_rtp_codecs_payloads_set_m_type(ast_rtp_instance_get_codecs(subchan->rtp), subchan->rtp, 18);

	ast_rtp_codecs_packetization_set(ast_rtp_instance_get_codecs(subchan->rtp),
					subchan->rtp, &subchan->line->cfg->codec_pref);
}

static int start_rtp(struct sccp_subchannel *subchan)
{
	struct ast_sockaddr bindaddr_tmp;

	ast_sockaddr_from_sin(&bindaddr_tmp, sccp_session_local_addr(subchan->line->device->session));
	subchan->rtp = ast_rtp_instance_new("asterisk", sccp_sched, &bindaddr_tmp, NULL);
	if (!subchan->rtp) {
		ast_log(LOG_ERROR, "RTP instance creation failed\n");
		return -1;
	}

	subchan_init_rtp_instance(subchan);

	sccp_subchannel_start_media_transmission(subchan);

	return 0;
}

static void handle_msg_open_receive_channel_ack(struct sccp_device *device, struct sccp_msg *msg)
{
	uint32_t addr = msg->data.openreceivechannelack.ipAddr;
	uint32_t port = letohl(msg->data.openreceivechannelack.port);

	if (!device->active_subchan) {
		ast_log(LOG_DEBUG, "active_subchan is NULL\n");
		return;
	}

	device->open_receive_channel_pending = 0;
	device->remote.sin_family = AF_INET;
	device->remote.sin_addr.s_addr = addr;
	device->remote.sin_port = htons(port);

	if (device->active_subchan->resuming) {
		device->active_subchan->resuming = 0;
		sccp_subchannel_start_media_transmission(device->active_subchan);
	} else {
		start_rtp(device->active_subchan);
	}

	if (device->active_subchan->channel) {
		if (add_ast_queue_control_task(device, device->active_subchan->channel, AST_CONTROL_ANSWER)) {
			return;
		}
	}
}

static void handle_softkey_answer(struct sccp_device *device, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan = sccp_device_get_subchan(device, subchan_id);

	transmit_speaker_mode(device, SCCP_SPEAKERON);

	if (!subchan) {
		ast_log(LOG_NOTICE, "handle softkey answer failed: no subchan %u\n", subchan_id);
		return;
	}

	do_answer(device, subchan);
}

static void handle_softkey_endcall(struct sccp_device *device, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan = sccp_device_get_subchan(device, subchan_id);

	if (!subchan) {
		ast_log(LOG_NOTICE, "handle softkey endcall failed: no subchan %u\n", subchan_id);
		return;
	}

	do_hangup(device, subchan);
}

static void handle_softkey_hold(struct sccp_device *device)
{
	if (!device->active_subchan) {
		ast_log(LOG_NOTICE, "handle softkey hold failed: no active subchan\n");
		return;
	}

	do_hold(device);
}

static void handle_softkey_newcall(struct sccp_device *device)
{
	transmit_speaker_mode(device, SCCP_SPEAKERON);

	do_newcall(device);
}

static void handle_softkey_redial(struct sccp_device *device)
{
	struct sccp_subchannel *subchan;

	if (!ast_strlen_zero(device->last_exten)) {
		transmit_speaker_mode(device, SCCP_SPEAKERON);

		/* XXX this 3 steps stuff should be simplified into one function */
		subchan = do_newcall(device);
		if (!subchan) {
			return;
		}

		ast_copy_string(device->exten, device->last_exten, sizeof(device->exten));
		start_the_call(device, subchan);
	}
}

static void handle_softkey_resume(struct sccp_device *device, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan = sccp_device_get_subchan(device, subchan_id);

	if (!subchan) {
		ast_log(LOG_NOTICE, "handle softkey resume failed: no subchan %u\n", subchan_id);
		return;
	}

	if (subchan == device->active_subchan) {
		ast_log(LOG_NOTICE, "handle softkey resume failed: subchan is already active\n");
		return;
	}

	if (device->active_subchan) {
		if (do_hold(device)) {
			ast_log(LOG_NOTICE, "handle softkey resume failed: could not put active subchan on hold\n");
			return;
		}
	}

	do_resume(device, subchan);
}

static void handle_msg_softkey_event(struct sccp_device *device, struct sccp_msg *msg)
{
	uint32_t softkey_event = letohl(msg->data.softkeyevent.softKeyEvent);
	uint32_t line_instance = letohl(msg->data.softkeyevent.lineInstance);
	uint32_t call_instance = letohl(msg->data.softkeyevent.callInstance);

	ast_debug(1, "Softkey event message: event 0x%02X, line_instance %u, subchan_id %u\n",
			softkey_event, line_instance, call_instance);

	switch (softkey_event) {
	case SOFTKEY_REDIAL:
		handle_softkey_redial(device);
		break;

	case SOFTKEY_NEWCALL:
		handle_softkey_newcall(device);
		break;

	case SOFTKEY_HOLD:
		handle_softkey_hold(device);
		break;

	case SOFTKEY_ENDCALL:
		handle_softkey_endcall(device, call_instance);
		break;

	case SOFTKEY_RESUME:
		handle_softkey_resume(device, call_instance);
		break;

	case SOFTKEY_ANSWER:
		handle_softkey_answer(device, call_instance);
		break;

	default:
		break;
	}
}

static void handle_msg_softkey_set_req(struct sccp_device *device)
{
	transmit_softkey_set_res(device);
	transmit_selectsoftkeys(device, 0, 0, KEYDEF_ONHOOK);
}

static void handle_msg_softkey_template_req(struct sccp_device *device)
{
	transmit_softkey_template_res(device);
}

static void handle_msg_speeddial_status_req(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_speeddial *speeddial;
	uint32_t index = letohl(msg->data.speeddial.instance);

	speeddial = sccp_device_get_speeddial_by_index(device, index);
	if (!speeddial) {
		ast_debug(2, "No speeddial [%d] on device [%s]\n", index, device->name);
		return;
	}

	transmit_speeddial_stat_res(device, speeddial);
}

static void handle_stimulus_voicemail(struct sccp_device *device)
{
	struct sccp_subchannel *subchan;

	if (ast_strlen_zero(device->cfg->voicemail) || ast_strlen_zero(device->cfg->vmexten)) {
		return;
	}

	/* XXX 3 stuff steps should be replaced with something simpler */
	subchan = do_newcall(device);
	if (!subchan) {
		return;
	}

	/* open our speaker */
	transmit_speaker_mode(device, SCCP_SPEAKERON);

	ast_copy_string(device->exten, device->cfg->vmexten, sizeof(device->exten));
	start_the_call(device, subchan);
}

static void handle_msg_stimulus(struct sccp_device *device, struct sccp_msg *msg)
{
	uint32_t stimulus = letohl(msg->data.stimulus.stimulus);

	switch (stimulus) {
	case STIMULUS_VOICEMAIL:
		handle_stimulus_voicemail(device);
		break;
	}
}

static void handle_msg_time_date_req(struct sccp_device *device)
{
	transmit_time_date_res(device);
}

static void handle_msg_unregister(struct sccp_device *device)
{
	sccp_session_stop(device->session);
}

static void handle_msg_state_common(struct sccp_device *device, struct sccp_msg *msg, uint32_t msg_id)
{
	switch (msg_id) {
	case KEEP_ALIVE_MESSAGE:
		handle_msg_keep_alive(device);
		break;

	case ALARM_MESSAGE:
		ast_debug(1, "Alarm message: %s\n", msg->data.alarm.displayMessage);
		break;

	case ENBLOC_CALL_MESSAGE:
		handle_msg_enbloc_call(device, msg);
		break;

	case STIMULUS_MESSAGE:
		handle_msg_stimulus(device, msg);
		break;

	case KEYPAD_BUTTON_MESSAGE:
		handle_msg_keypad_button(device, msg);
		break;

	case OFFHOOK_MESSAGE:
		handle_msg_offhook(device, msg);
		break;

	case ONHOOK_MESSAGE:
		handle_msg_onhook(device, msg);
		break;

	case FORWARD_STATUS_REQ_MESSAGE:
		/* do nothing here, not all phone query the forward status */
		break;

	case CAPABILITIES_RES_MESSAGE:
		handle_msg_capabilities_res(device, msg);
		break;

	case SPEEDDIAL_STAT_REQ_MESSAGE:
		handle_msg_speeddial_status_req(device, msg);
		break;

	case FEATURE_STATUS_REQ_MESSAGE:
		handle_msg_feature_status_req(device, msg);
		break;

	case LINE_STATUS_REQ_MESSAGE:
		handle_msg_line_status_req(device, msg);
		break;

	case CONFIG_STATUS_REQ_MESSAGE:
		handle_msg_config_status_req(device);
		break;

	case TIME_DATE_REQ_MESSAGE:
		handle_msg_time_date_req(device);
		break;

	case BUTTON_TEMPLATE_REQ_MESSAGE:
		handle_msg_button_template_req(device);
		break;

	case UNREGISTER_MESSAGE:
		handle_msg_unregister(device);
		break;

	case SOFTKEY_TEMPLATE_REQ_MESSAGE:
		handle_msg_softkey_template_req(device);
		break;

	case SOFTKEY_EVENT_MESSAGE:
		handle_msg_softkey_event(device, msg);
		break;

	case OPEN_RECEIVE_CHANNEL_ACK_MESSAGE:
		handle_msg_open_receive_channel_ack(device, msg);
		break;

	case SOFTKEY_SET_REQ_MESSAGE:
		handle_msg_softkey_set_req(device);
		break;
	}
}

/*
 * entry point: true
 * thread: session
 */
int sccp_device_handle_msg(struct sccp_device *device, struct sccp_msg *msg)
{
	uint32_t msg_id;

	if (!msg) {
		ast_log(LOG_ERROR, "sccp device handle msg failed: msg is null\n");
		return -1;
	}

	msg_id = letohl(msg->id);

	sccp_device_lock(device);
	if (device->state->handle_msg) {
		device->state->handle_msg(device, msg, msg_id);
	}

	sccp_device_unlock(device);

	return 0;
}

/*
 * thread: session
 */
static int sccp_device_test_apply_config(struct sccp_device *device, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_device_cfg *old_device_cfg = device->cfg;
	struct sccp_line_cfg *new_line_cfg = new_device_cfg->line_cfg;
	struct sccp_line_cfg *old_line_cfg = old_device_cfg->line_cfg;
	struct sccp_speeddial_cfg *new_sd_cfg;
	struct sccp_speeddial_cfg *old_sd_cfg;
	size_t i;

	if (strcmp(old_device_cfg->dateformat, new_device_cfg->dateformat)) {
		return 0;
	}

	if (strcmp(old_device_cfg->voicemail, new_device_cfg->voicemail)) {
		return 0;
	}

	if (old_device_cfg->keepalive != new_device_cfg->keepalive) {
		return 0;
	}

	if (old_device_cfg->speeddial_count != new_device_cfg->speeddial_count) {
		return 0;
	}

	/** check for line **/
	if (strcmp(old_line_cfg->name, new_line_cfg->name)) {
		return 0;
	}

	if (strcmp(old_line_cfg->cid_num, new_line_cfg->cid_num)) {
		return 0;
	}

	if (strcmp(old_line_cfg->cid_name, new_line_cfg->cid_name)) {
		return 0;
	}

	/* right now, the context is also used as the voicemail context and speeddial hint context */
	if (strcmp(old_line_cfg->context, new_line_cfg->context)) {
		return 0;
	}

	/** check for speeddials **/
	/* A: new_device_cfg->speeddial_count == old_device_cfg->speeddial_count */
	for (i = 0; i < new_device_cfg->speeddial_count; i++) {
		new_sd_cfg = new_device_cfg->speeddials_cfg[i];
		old_sd_cfg = old_device_cfg->speeddials_cfg[i];

		if (strcmp(old_sd_cfg->label, new_sd_cfg->label)) {
			return 0;
		}

		if (old_sd_cfg->blf != new_sd_cfg->blf) {
			return 0;
		}

		if (new_sd_cfg->blf && strcmp(old_sd_cfg->extension, new_sd_cfg->extension)) {
			return 0;
		}
	}

	return 1;
}

/*
 * entry point: yes
 * thread: session
 */
int sccp_device_reload_config(struct sccp_device *device, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_line *line = device->line_group.line;
	struct sccp_speeddial *speeddial;
	size_t i;

	if (!new_device_cfg) {
		ast_log(LOG_ERROR, "sccp device reload config failed: device_cfg is null\n");
		return -1;
	}

	if (!sccp_device_test_apply_config(device, new_device_cfg)) {
		sccp_device_lock(device);
		transmit_reset(device, SCCP_RESET_SOFT);
		sccp_device_unlock(device);

		return 0;
	}

	sccp_device_lock(device);

	ao2_ref(device->cfg, -1);
	device->cfg = new_device_cfg;
	ao2_ref(device->cfg, +1);

	ao2_ref(line->cfg, -1);
	line->cfg = new_device_cfg->line_cfg;
	ao2_ref(line->cfg, +1);

	for (i = 0; i < device->sd_group.count; i++) {
		speeddial = device->sd_group.speeddials[i];
		ao2_ref(speeddial->cfg, -1);
		speeddial->cfg = new_device_cfg->speeddials_cfg[i];
		ao2_ref(speeddial->cfg, +1);
	}

	sccp_device_unlock(device);

	return 0;
}

/*
 * entry point: yes
 * thread: any
 */
int sccp_device_reset(struct sccp_device *device, enum sccp_reset_type type)
{
	sccp_device_lock(device);
	transmit_reset(device, type);
	sccp_device_unlock(device);

	return 0;
}

/*
 * entry point: yes
 * thread: session
 */
void sccp_device_on_connection_lost(struct sccp_device *device)
{
	sccp_device_lock(device);
	device->state = &state_connlost;
	sccp_device_unlock(device);
}

/*
 * entry point: yes
 * thread: session
 */
static void on_keepalive_timeout(struct sccp_device *device, void __attribute__((unused)) *data)
{
	ast_log(LOG_WARNING, "Device %s has timed out\n", device->name);

	sccp_session_stop(device->session);
}

/*
 * thread: session
 */
static int add_keepalive_task(struct sccp_device *device)
{
	int timeout = 2 * device->cfg->keepalive;

	return sccp_session_add_device_task(device->session, on_keepalive_timeout, NULL, timeout);
}

/*
 * entry point:  yes
 * thread: session
 */
static void on_dial_timeout(struct sccp_device *device, void *data)
{
	struct sccp_subchannel *subchan = data;

	sccp_device_lock(device);
	start_the_call(device, subchan);
	sccp_device_unlock(device);
}

/*
 * thread: session
 */
static int add_dialtimeout_task(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	int timeout = device->cfg->dialtimeout;

	return sccp_session_add_device_task(device->session, on_dial_timeout, subchan, timeout);
}

static void remove_dialtimeout_task(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	sccp_session_remove_device_task(device->session, on_dial_timeout, subchan);
}

/*
 * entry point: yes
 * thread: session
 */
void sccp_device_on_data_read(struct sccp_device *device)
{
	add_keepalive_task(device);
}

/*
 * thread: session
 */
static void init_voicemail_lamp_state(struct sccp_device *device)
{
	int new_msgs;
	int old_msgs;

	if (ast_strlen_zero(device->cfg->voicemail)) {
		return;
	}

	if (ast_app_inboxcount(device->cfg->voicemail, &new_msgs, &old_msgs) == -1) {
		ast_log(LOG_NOTICE, "could not get voicemail count for %s\n", device->cfg->voicemail);
		return;
	}

	transmit_voicemail_lamp_state(device, new_msgs);
}

/*
 * entry point: yes
 * thread: session
 */
void sccp_device_on_registration_success(struct sccp_device *device)
{
	sccp_device_lock(device);

	transmit_register_ack(device);
	transmit_capabilities_req(device);
	transmit_clear_message(device);

	init_voicemail_lamp_state(device);

	add_keepalive_task(device);

	device->state = &state_registering;
	/* TODO update line devstate (even if it's in fact a bit early since we don't know
	 * yet the device capabilities) */

	sccp_device_unlock(device);

	subscribe_mwi(device);
	subscribe_hints(device);
}

/*
 * entry point: yes
 * thread: any
 */
void sccp_device_take_snapshot(struct sccp_device *device, struct sccp_device_snapshot *snapshot)
{
	sccp_device_lock(device);
	snapshot->type = device->type;
	snapshot->proto_version = device->proto_version;
	ast_copy_string(snapshot->name, device->name, sizeof(snapshot->name));
	ast_copy_string(snapshot->ipaddr, sccp_session_remote_addr_ch(device->session), sizeof(snapshot->ipaddr));
	ast_getformatname_multiple(snapshot->capabilities, sizeof(snapshot->capabilities), device->caps);
	sccp_device_unlock(device);
}

unsigned int sccp_device_line_count(const struct sccp_device *device)
{
	return device->line_group.count;
}

struct sccp_line* sccp_device_line(struct sccp_device *device, unsigned int i)
{
	if (i >= device->line_group.count) {
		ast_log(LOG_ERROR, "sccp device line failed: %u is out of bound\n", i);
		return NULL;
	}

	return device->line_group.line;
}

const char *sccp_device_name(const struct sccp_device *device)
{
	return device->name;
}

const char *sccp_line_name(const struct sccp_line *line)
{
	return line->name;
}

struct ast_channel *sccp_line_request(struct sccp_line *line, struct ast_format_cap *cap, const char *linkedid, int *cause)
{
	struct sccp_device *device = line->device;
	struct sccp_subchannel *subchan;
	struct ast_channel *channel = NULL;

	sccp_device_lock(device);

	/* TODO add dnd support */
	/* TODO add call forward support */

	subchan = sccp_line_new_subchannel(line, SCCP_DIR_INCOMING);
	if (!subchan) {
		goto unlock;
	}

	if (sccp_subchannel_new_channel(subchan, linkedid, cap)) {
		do_clear_subchannel(device, subchan);
		goto unlock;
	}

	channel = subchan->channel;

unlock:
	sccp_device_unlock(device);

	return channel;
}

static void format_party_name(struct ast_channel *channel, char *name, size_t n)
{
	struct ast_party_redirecting *redirect = ast_channel_redirecting(channel);
	struct ast_party_connected_line *connected = ast_channel_connected(channel);

	if (redirect->from.name.valid) {
		snprintf(name, n, "%s -> %s", redirect->from.name.str, connected->id.name.str);
	} else if (redirect->from.number.valid) {
		snprintf(name, n, "%s -> %s", redirect->from.number.str, connected->id.name.str);
	} else {
		snprintf(name, n, "%s", connected->id.name.str);
	}
}

static void format_party_number(struct ast_channel *channel, char **number)
{
	*number = ast_channel_connected(channel)->id.number.str;
}

int sccp_subchannel_call(struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	struct ast_channel *channel = subchan->channel;
	char name[64];
	char *number;

	ast_setstate(channel, AST_STATE_RINGING);
	ast_queue_control(channel, AST_CONTROL_RINGING);

	sccp_device_lock(device);

	/* TODO add callfwd stuff */

	subchan->state = SCCP_RINGIN;

	if (!device->active_subchan) {
		line->state = SCCP_RINGIN;
		transmit_ringer_mode(device, SCCP_RING_INSIDE);
	}

	format_party_name(channel, name, sizeof(name));
	format_party_number(channel, &number);

	transmit_subchan_callstate(device, subchan, SCCP_RINGIN);
	transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_RINGIN);
	transmit_callinfo(device, name, number, "", line->cfg->cid_num, line->instance, subchan->id, subchan->direction);
	transmit_line_lamp_state(device, line, SCCP_LAMP_BLINK);

	/* TODO add autoanswer stuff */
	/* TODO add update line devstate stuff */

	sccp_device_unlock(device);

	return 0;
}

int sccp_subchannel_hangup(struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	struct ast_channel *channel = subchan->channel;

	sccp_device_lock(device);
	do_clear_subchannel(device, subchan);
	sccp_device_unlock(device);

	ast_setstate(channel, AST_STATE_DOWN);
	ast_channel_tech_pvt_set(channel, NULL);
	ao2_ref(subchan, -1);

	return 0;
}

int sccp_subchannel_answer(struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	struct ast_channel *channel = subchan->channel;

	sccp_device_lock(device);

	if (!subchan->rtp) {
		transmit_subchan_open_receive_channel(device, subchan);

		/* Wait for the phone to provide his ip:port information
		   before the bridging is being done. */
		/* XXX hum... need to review this sleep, which is even more suspicious now that we are locking */
		usleep(500000);
	}

	if (subchan->on_hold) {
		return 0;
	}

	transmit_subchan_callstate(device, subchan, SCCP_CONNECTED);
	transmit_subchan_stop_tone(device, subchan);
	transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_CONNECTED);

	line->state = SCCP_CONNECTED;

	sccp_device_unlock(device);

	ast_setstate(channel, AST_STATE_UP);

	return 0;
}

struct ast_frame *sccp_subchannel_read(struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	struct ast_channel *channel = subchan->channel;
	struct ast_frame *frame;
	struct ast_rtp_instance *rtp;

	sccp_device_lock(device);
	if (subchan->rtp) {
		rtp = subchan->rtp;
		ao2_ref(rtp, +1);
	}

	sccp_device_unlock(device);

	if (!rtp) {
		ast_log(LOG_DEBUG, "rtp is NULL\n");
		return &ast_null_frame;
	}

	switch (ast_channel_fdno(channel)) {
	case 0:
		frame = ast_rtp_instance_read(rtp, 0);
		break;

	case 1:
		frame = ast_rtp_instance_read(rtp, 1);
		break;

	default:
		frame = &ast_null_frame;
	}

	if (frame && frame->frametype == AST_FRAME_VOICE) {
		if (!(ast_format_cap_iscompatible(ast_channel_nativeformats(channel), &frame->subclass.format))) {
			ast_format_cap_set(ast_channel_nativeformats(channel), &frame->subclass.format);
			ast_set_read_format(channel, ast_channel_readformat(channel));
			ast_set_write_format(channel, ast_channel_writeformat(channel));
		}
	}

	ao2_ref(rtp, -1);

	return frame;
}

int sccp_subchannel_write(struct sccp_subchannel *subchan, struct ast_frame *frame)
{
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	int res = 0;

	sccp_device_lock(device);

	if (subchan->rtp) {
		if (line->state == SCCP_CONNECTED || line->state == SCCP_PROGRESS) {
			res = ast_rtp_instance_write(subchan->rtp, frame);
		}
	} else if (line->state == SCCP_PROGRESS) {
		/* handle early rtp during progress state */
		transmit_subchan_stop_tone(device, subchan);
		transmit_subchan_open_receive_channel(device, subchan);
	}

	sccp_device_unlock(device);

	return res;
}

static void indicate_connected_line(struct sccp_device *device, struct sccp_line *line, struct sccp_subchannel *subchan, struct ast_channel *channel) {
	char name[64];
	char *number;

	format_party_name(channel, name, sizeof(name));
	format_party_number(channel, &number);

	switch (subchan->direction) {
	case SCCP_DIR_INCOMING:
		transmit_callinfo(device, name, number, "", "", line->instance, subchan->id, subchan->direction);
		break;
	case SCCP_DIR_OUTGOING:
		transmit_callinfo(device, "", "", name, number, line->instance, subchan->id, subchan->direction);
		break;
	}
}

int sccp_subchannel_indicate(struct sccp_subchannel *subchan, int ind, const void *data, size_t datalen)
{
#define _AST_PROVIDE_INBAND_SIGNALLING -1
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	struct ast_channel *channel = subchan->channel;
	int res = 0;
	int start_moh = 0;
	int stop_moh = 0;

	sccp_device_lock(device);

	switch (ind) {
	case AST_CONTROL_RINGING:
		if (ast_channel_state(channel) == AST_STATE_RING) {
			transmit_subchan_callstate(device, subchan, SCCP_RINGOUT);
		} else {
			res = _AST_PROVIDE_INBAND_SIGNALLING;
		}

		break;

	case AST_CONTROL_BUSY:
		transmit_ringer_mode(device, SCCP_RING_OFF);
		transmit_subchan_tone(device, subchan, SCCP_TONE_BUSY);
		break;

	case AST_CONTROL_CONGESTION:
		transmit_ringer_mode(device, SCCP_RING_OFF);
		transmit_subchan_tone(device, subchan, SCCP_TONE_BUSY);
		break;

	case AST_CONTROL_PROGRESS:
		line->state = SCCP_PROGRESS;
		break;

	case AST_CONTROL_PROCEEDING:
		break;

	case AST_CONTROL_HOLD:
		if (subchan->rtp) {
			ast_rtp_instance_update_source(subchan->rtp);
			start_moh = 1;
		}

		break;

	case AST_CONTROL_UNHOLD:
		if (subchan->rtp) {
			ast_rtp_instance_update_source(subchan->rtp);
			stop_moh = 1;
		}

		break;

	case AST_CONTROL_SRCUPDATE:
		ast_log(LOG_DEBUG, "src update\n");
		if (subchan->rtp) {
			ast_rtp_instance_update_source(subchan->rtp);
		}

		break;

	case AST_CONTROL_SRCCHANGE:
		ast_log(LOG_DEBUG, "src change\n");
		if (subchan->rtp) {
			ast_rtp_instance_change_source(subchan->rtp);
		}

		break;

	case AST_CONTROL_CONNECTED_LINE:
		indicate_connected_line(device, line, subchan, channel);
		break;
	}

	sccp_device_unlock(device);

	/* XXX ugly solution... */
	if (start_moh) {
		ast_moh_start(channel, data, NULL);
	} else if (stop_moh) {
		ast_moh_stop(channel);
	}

	return res;
}

int sccp_subchannel_fixup(struct sccp_subchannel *subchan, struct ast_channel *newchannel)
{
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;

	sccp_device_lock(device);
	subchan->channel = newchannel;
	sccp_device_unlock(device);

	return 0;
}
