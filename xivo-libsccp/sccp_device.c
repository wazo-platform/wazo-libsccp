#include <asterisk.h>
#include <asterisk/app.h>
#include <asterisk/astdb.h>
#include <asterisk/astobj2.h>
#include <asterisk/callerid.h>
#include <asterisk/causes.h>
#include <asterisk/channelstate.h>
#include <asterisk/event.h>
#include <asterisk/features.h>
#include <asterisk/format.h>
#include <asterisk/lock.h>
#include <asterisk/module.h>
#include <asterisk/musiconhold.h>
#include <asterisk/network.h>
#include <asterisk/pbx.h>
#include <asterisk/rtp_engine.h>
#include <asterisk/utils.h>

#include "device/sccp_channel_tech.h"
#include "device/sccp_rtp_glue.h"
#include "sccp.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_session.h"
#include "sccp_msg.h"
#include "sccp_queue.h"
#include "sccp_utils.h"

#define LINE_INSTANCE_START 1
#define SPEEDDIAL_INDEX_START 1

struct sccp_speeddial {
	/* const */
	struct sccp_device *device;
	/* updated in session thread only */
	struct sccp_speeddial_cfg *cfg;

	/* const */
	uint32_t instance;
	/* const */
	uint32_t index;

	/* updated in session thread only */
	int exten_state_cb_id;
	/* updated in >1 threads */
	int exten_state;
};

struct sccp_speeddials {
	/* const */
	struct sccp_speeddial **arr;
	/* const */
	size_t count;
};

struct sccp_subchannel {
	/* (dynamic) */
	struct ast_sockaddr direct_media_addr;
	/* (static) */
	struct ast_format fmt;

	/* (static) */
	struct sccp_line *line;
	/* (dynamic) */
	struct ast_channel *channel;
	/* (dynamic) */
	struct ast_rtp_instance *rtp;
	/* (dynamic) */
	struct sccp_subchannel *related;

	/* (static) */
	uint32_t id;
	/* (dynamic) */
	enum sccp_state state;
	/* (static) */
	enum sccp_direction direction;

	uint8_t resuming;
	uint8_t autoanswer;
	uint8_t transferring;

	AST_LIST_ENTRY(sccp_subchannel) list;
};

struct sccp_line {
	AST_LIST_HEAD_NOLOCK(, sccp_subchannel) subchans;

	/* const */
	struct sccp_device *device;
	/* updated in session thread only */
	struct sccp_line_cfg *cfg;

	/* const */
	uint32_t instance;
	enum sccp_state state;

	/* const, same string as cfg->name, but this one can be used safely in
	 * non-session thread without holding the device lock
	 */
	char name[SCCP_LINE_NAME_MAX];
};

/* limited to exactly 1 line for now, but is the way on to the support of multiple lines,
 * and more important, it offers symmetry with speeddial_group, so there's only one system
 * to understand
 */
struct sccp_lines {
	struct sccp_line *line;
	size_t count;
};

enum call_forward_status {
	SCCP_CFWD_INACTIVE,
	SCCP_CFWD_INPUTEXTEN,
	SCCP_CFWD_ACTIVE,
};

enum receive_channel_status {
	SCCP_RECV_CHAN_CLOSED,
	SCCP_RECV_CHAN_OPENING,
	SCCP_RECV_CHAN_OPENED,
};

enum sccp_device_state {
	STATE_NEW,
	STATE_WORKING,
	STATE_CONNLOST,
};

enum {
	DEVICE_DESTROYED = (1 << 0),
};

struct sccp_device {
	/* (static) */
	ast_mutex_t lock;

	/* updated in session thread only, on device destroy */
	struct sccp_speeddials speeddials;
	/* updated in session thread only, on device destroy */
	struct sccp_lines lines;

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
	struct ast_format_cap *caps;	/* Supported capabilities */
	/* (dynamic, modified in thread session only) */
	struct ast_event_sub *mwi_event_sub;
	/* (dynamic) */
	struct sccp_subchannel *active_subchan;

	uint32_t serial_callid;
	uint32_t callfwd_id;
	enum call_forward_status callfwd;
	enum receive_channel_status recv_chan_status;
	enum sccp_device_state state;
	int reset_on_idle;
	int dnd;
	unsigned int flags;
	enum sccp_device_type type;
	uint8_t proto_version;

	/* if the device is a guest, then the name will be different then the
	 * device config name (static)
	 */
	char name[SCCP_DEVICE_NAME_MAX];
	char exten[AST_MAX_EXTENSION];
	char last_exten[AST_MAX_EXTENSION];
	char callfwd_exten[AST_MAX_EXTENSION];
};

struct nolock_task_ast_channel_xfer_masquerade {
	struct ast_channel *active_chan;
	struct ast_channel *related_chan;
};

struct nolock_task_ast_queue_control {
	struct ast_channel *channel;
	enum ast_control_frame_type control;
};

struct nolock_task_ast_queue_frame_dtmf {
	struct ast_channel *channel;
	int digit;
};

struct nolock_task_ast_queue_hangup {
	struct ast_channel *channel;
};

struct nolock_task_pickup_channel {
	struct ast_channel *channel;
};

struct nolock_task_start_channel {
	struct ast_channel *channel;
	struct sccp_line_cfg *line_cfg;
};

union nolock_task_data {
	struct nolock_task_ast_channel_xfer_masquerade xfer_masquerade;
	struct nolock_task_ast_queue_control queue_control;
	struct nolock_task_ast_queue_frame_dtmf queue_frame_dtmf;
	struct nolock_task_ast_queue_hangup queue_hangup;
	struct nolock_task_pickup_channel pickup_channel;
	struct nolock_task_start_channel start_channel;
};

struct nolock_task {
	union nolock_task_data data;
	void (*exec)(union nolock_task_data *data);
};

static int add_ast_queue_hangup_task(struct sccp_device *device, struct ast_channel *channel);
static void sccp_line_update_devstate(struct sccp_line *line, enum ast_device_state state);
static struct sccp_line *sccp_lines_get_default(struct sccp_lines *lines);
static void sccp_device_lock(struct sccp_device *device);
static void sccp_device_unlock(struct sccp_device *device);
static void sccp_device_panic(struct sccp_device *device);
static void subscribe_mwi(struct sccp_device *device);
static void unsubscribe_mwi(struct sccp_device *device);
static void handle_msg_state_common(struct sccp_device *device, struct sccp_msg *msg, uint32_t msg_id);
static void transmit_feature_status(struct sccp_device *device, struct sccp_speeddial *sd);
static void transmit_reset(struct sccp_device *device, enum sccp_reset_type type);
static void transmit_subchan_start_media_transmission(struct sccp_device *device, struct sccp_subchannel *subchan, struct sockaddr_in *endpoint);
static int add_dialtimeout_task(struct sccp_device *device, struct sccp_subchannel *subchan);
static void remove_dialtimeout_task(struct sccp_device *device, struct sccp_subchannel *subchan);
static int add_fwdtimeout_task(struct sccp_device *device);
static void remove_fwdtimeout_task(struct sccp_device *device);

static unsigned int chan_idx = 0;

static void sccp_speeddial_destructor(void *data)
{
	struct sccp_speeddial *sd = data;

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
	sd->exten_state_cb_id = -1;

	return sd;
}

static int on_extension_state_change(char *context, char *id, struct ast_state_cb_info *info, void *data)
{
	struct sccp_speeddial *sd = data;
	struct sccp_device *device = sd->device;

	sccp_device_lock(device);
	sd->exten_state = info->exten_state;
	transmit_feature_status(device, sd);
	sccp_device_unlock(device);

	return 0;
}

/*
 * Must be called only from the thread session, with the device NOT locked.
 */
static void sccp_speeddial_add_extension_state_cb(struct sccp_speeddial *sd)
{
	const char *context = sccp_lines_get_default(&sd->device->lines)->cfg->context;

	/* XXX fetching the state isn't part of "add extension state callback", still it makes
	 *     some sense to do it there...
	 */
	sd->exten_state = ast_extension_state(NULL, context, sd->cfg->extension);
	sd->exten_state_cb_id = ast_extension_state_add(context, sd->cfg->extension, on_extension_state_change, sd);
	if (sd->exten_state_cb_id == -1) {
		ast_log(LOG_WARNING, "Could not subscribe to %s@%s\n", sd->cfg->extension, context);
	}
}

/*
 * Must be called only from the thread session, with the device NOT locked.
 */
static void sccp_speeddial_del_extension_state_cb(struct sccp_speeddial *sd)
{
	if (sd->exten_state_cb_id != -1) {
		ast_extension_state_del(sd->exten_state_cb_id, NULL);
	}
}

static enum sccp_blf_status extstate_ast2sccp(const struct sccp_device *device, int state)
{
	switch (state) {
	case AST_EXTENSION_DEACTIVATED:
	case AST_EXTENSION_REMOVED:
		return SCCP_BLF_STATUS_UNKNOWN;
	}

	if (state & AST_EXTENSION_INUSE) {
		return SCCP_BLF_STATUS_INUSE;
	}

	switch (state) {
	case AST_EXTENSION_RINGING:
		switch (device->type) {
		case SCCP_DEVICE_7940:
		case SCCP_DEVICE_7960:
			return SCCP_BLF_STATUS_INUSE;
		default:
			return SCCP_BLF_STATUS_ALERTING;
		}

	case AST_EXTENSION_UNAVAILABLE:
		return SCCP_BLF_STATUS_UNKNOWN;
	case AST_EXTENSION_BUSY:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_ONHOLD:
		return SCCP_BLF_STATUS_INUSE;
	case AST_EXTENSION_NOT_INUSE:
		return SCCP_BLF_STATUS_IDLE;
	default:
		return SCCP_BLF_STATUS_UNKNOWN;
	}
}

static enum sccp_blf_status sccp_speeddial_status(const struct sccp_device *device, const struct sccp_speeddial *sd) {
	if (sd->cfg->blf) {
		return extstate_ast2sccp(device, sd->exten_state);
	} else {
		return SCCP_BLF_STATUS_UNKNOWN;
	}
}

static int sccp_speeddials_init(struct sccp_speeddials *speeddials, struct sccp_device *device, uint32_t instance)
{
	struct sccp_speeddial **arr;
	struct sccp_speeddial_cfg **speeddials_cfg = device->cfg->speeddials_cfg;
	size_t i;
	size_t n = device->cfg->speeddial_count;
	uint32_t index = SPEEDDIAL_INDEX_START;

	if (!n) {
		speeddials->count = 0;
		return 0;
	}

	arr = ast_calloc(n, sizeof(*arr));
	if (!arr) {
		return -1;
	}

	for (i = 0; i < n; i++) {
		arr[i] = sccp_speeddial_alloc(speeddials_cfg[i], device, instance++, index++);
		if (!arr[i]) {
			goto error;
		}
	}

	speeddials->arr = arr;
	speeddials->count = n;

	return 0;

error:
	for (; i > 0; i--) {
		ao2_ref(arr[i - 1], -1);
	}

	ast_free(arr);

	return -1;
}

static void sccp_speeddials_deinit(struct sccp_speeddials *speeddials)
{
	size_t i;

	if (!speeddials->count) {
		return;
	}

	for (i = 0; i < speeddials->count; i++) {
		ao2_ref(speeddials->arr[i], -1);
	}

	ast_free(speeddials->arr);
}

/*
 * Must be called only from the thread session, with the device NOT locked.
 */
static void sccp_speeddials_destroy(struct sccp_speeddials *speeddials)
{
	struct sccp_speeddial *sd;
	size_t i;

	/* unsubscribe to extension state notifications for BLFs */
	for (i = 0; i < speeddials->count; i++) {
		sd = speeddials->arr[i];
		if (sd->cfg->blf) {
			sccp_speeddial_del_extension_state_cb(sd);
		}
	}
}

/*
 * Must be called only from the thread session, with the device NOT locked.
 */
static void sccp_speeddials_on_registration_success(struct sccp_speeddials *speeddials)
{
	struct sccp_speeddial *sd;
	size_t i;

	/* subscribe to extension state notifications for BLFs */
	for (i = 0; i < speeddials->count; i++) {
		sd = speeddials->arr[i];
		if (sd->cfg->blf) {
			sccp_speeddial_add_extension_state_cb(sd);
		}
	}
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_speeddial *sccp_speeddials_get_by_instance(struct sccp_speeddials *speeddials, uint32_t instance)
{
	struct sccp_speeddial *sd;
	size_t i;

	for (i = 0; i < speeddials->count; i++) {
		sd = speeddials->arr[i];
		if (sd->instance == instance) {
			return sd;
		}
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_speeddial *sccp_speeddials_get_by_index(struct sccp_speeddials *speeddials, uint32_t index)
{
	size_t i = index - SPEEDDIAL_INDEX_START;

	if (i >= speeddials->count) {
		return NULL;
	}

	return speeddials->arr[i];
}

static int sccp_speeddials_test_apply_config(struct sccp_speeddials *speeddials, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_speeddial_cfg *new_sd_cfg;
	struct sccp_speeddial_cfg *old_sd_cfg;
	size_t i;

	if (speeddials->count != new_device_cfg->speeddial_count) {
		return 0;
	}

	/* A: speeddials->count == new_device_cfg->speeddial_count */
	for (i = 0; i < speeddials->count; i++) {
		new_sd_cfg = new_device_cfg->speeddials_cfg[i];
		old_sd_cfg = speeddials->arr[i]->cfg;

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

static void sccp_speeddials_apply_config(struct sccp_speeddials *speeddials, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_speeddial *sd;
	size_t i;

	for (i = 0; i < speeddials->count; i++) {
		sd = speeddials->arr[i];
		ao2_ref(sd->cfg, -1);
		sd->cfg = new_device_cfg->speeddials_cfg[i];
		ao2_ref(sd->cfg, +1);
	}
}

static void sccp_subchannel_destructor(void *data)
{
	struct sccp_subchannel *subchan = data;

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
	subchan->related = NULL;
	subchan->id = id;
	subchan->state = SCCP_OFFHOOK;
	subchan->direction = direction;
	subchan->resuming = 0;
	subchan->autoanswer = 0;
	subchan->transferring = 0;

	return subchan;
}

static void sccp_subchannel_destroy(struct sccp_subchannel *subchan)
{
	if (subchan->channel) {
		add_ast_queue_hangup_task(subchan->line->device, subchan->channel);
	} else {
		if (subchan->rtp) {
			ast_rtp_instance_stop(subchan->rtp);
			ast_rtp_instance_destroy(subchan->rtp);
			subchan->rtp = NULL;
		}
	}
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

static void exec_ast_channel_transfer_masquerade(union nolock_task_data *data)
{
	struct ast_channel *active_chan = data->xfer_masquerade.active_chan;
	struct ast_channel *related_chan = data->xfer_masquerade.related_chan;

	ast_channel_transfer_masquerade(active_chan, ast_channel_connected(active_chan), 0,
			ast_bridged_channel(related_chan), ast_channel_connected(related_chan), 1);

	ast_channel_unref(active_chan);
	ast_channel_unref(related_chan);
}

static int add_ast_channel_transfer_masquerade_task(struct sccp_device *device, struct ast_channel *active_chan, struct ast_channel *related_chan)
{
	struct nolock_task task;

	task.exec = exec_ast_channel_transfer_masquerade;
	task.data.xfer_masquerade.active_chan = active_chan;
	task.data.xfer_masquerade.related_chan = related_chan;

	if (add_nolock_task(device, &task)) {
		return -1;
	}

	ast_channel_ref(active_chan);
	ast_channel_ref(related_chan);

	return 0;
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

static void exec_ast_queue_frame_dtmf(union nolock_task_data *data)
{
	struct ast_frame frame = { .frametype = AST_FRAME_DTMF, };
	struct ast_channel *channel = data->queue_frame_dtmf.channel;
	int digit = data->queue_frame_dtmf.digit;

	frame.subclass.integer = digit;
	frame.src = "sccp";
	frame.len = 100;
	frame.offset = 0;
	frame.datalen = 0;

	ast_queue_frame(channel, &frame);

	ast_channel_unref(channel);
}

static int add_ast_queue_frame_dtmf_task(struct sccp_device *device, struct ast_channel *channel, int digit)
{
	struct nolock_task task;

	task.exec = exec_ast_queue_frame_dtmf;
	task.data.queue_frame_dtmf.channel = channel;
	task.data.queue_frame_dtmf.digit = digit;

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

static void exec_pickup_channel(union nolock_task_data *data)
{
	struct ast_channel *channel = data->pickup_channel.channel;

	if (ast_pickup_call(channel)) {
		ast_channel_hangupcause_set(channel, AST_CAUSE_CALL_REJECTED);
	} else {
		ast_channel_hangupcause_set(channel, AST_CAUSE_NORMAL_CLEARING);
	}

	ast_hangup(channel);
	ast_channel_unref(channel);
}

static int add_pickup_channel_task(struct sccp_device *device, struct ast_channel *channel)
{
	struct nolock_task task;

	task.exec = exec_pickup_channel;
	task.data.pickup_channel.channel = channel;

	if (add_nolock_task(device, &task)) {
		return -1;
	}

	ast_channel_ref(channel);

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

/*
 * The device must NOT be locked.
 *
 * Since the device lock is shared between subchannels, if we are holding the device lock before calling
 * ast_channel_alloc, then it's possible that we are indirectly holding a channel lock if another
 * subchannel is trying to lock the device.
 */
static struct ast_channel *alloc_channel(struct sccp_line_cfg *line_cfg, const char *exten, const char *linkedid)
{
	struct ast_channel *channel;
	struct ast_variable *var_itr;
	char valuebuf[1024];

	channel = ast_channel_alloc(1, AST_STATE_DOWN, line_cfg->cid_num, line_cfg->cid_name, "code", exten, line_cfg->context, linkedid, 0, SCCP_LINE_PREFIX "/%s-%08x", line_cfg->name, ast_atomic_fetchadd_int((int *)&chan_idx, +1));
	if (!channel) {
		return NULL;
	}

	for (var_itr = line_cfg->chanvars; var_itr; var_itr = var_itr->next) {
		ast_get_encoded_str(var_itr->value, valuebuf, sizeof(valuebuf));
		pbx_builtin_setvar_helper(channel, var_itr->name, valuebuf);
	}

	if (!ast_strlen_zero(line_cfg->language)) {
		ast_channel_language_set(channel, line_cfg->language);
	}

	ast_channel_callgroup_set(channel, line_cfg->callgroups);
	ast_channel_pickupgroup_set(channel, line_cfg->pickupgroups);

	ast_channel_named_callgroups_set(channel, line_cfg->named_callgroups);
	ast_channel_named_pickupgroups_set(channel, line_cfg->named_pickupgroups);

	return channel;
}

static int sccp_subchannel_set_channel(struct sccp_subchannel *subchan, struct ast_channel *channel, struct ast_format_cap *cap)
{
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	RAII_VAR(struct ast_format_cap *, joint, ast_format_cap_alloc(), ast_format_cap_destroy);
	struct ast_format_cap *tmpcaps = NULL;
	int has_joint;
	char buf[256];

	if (subchan->channel) {
		ast_log(LOG_ERROR, "subchan already has a channel\n");
		return -1;
	}

	has_joint = ast_format_cap_joint_copy(line->cfg->caps, device->caps, joint);
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

	ast_channel_tech_set(channel, &sccp_tech);
	ast_channel_tech_pvt_set(channel, subchan);
	ao2_ref(subchan, +1);
	subchan->channel = channel;

	ast_codec_choose(&line->cfg->codec_pref, joint, 1, &subchan->fmt);
	ast_debug(1, "best codec %s\n", ast_getformatname(&subchan->fmt));

	ast_format_cap_set(ast_channel_nativeformats(channel), &subchan->fmt);
	ast_format_copy(ast_channel_writeformat(channel), &subchan->fmt);
	ast_format_copy(ast_channel_rawwriteformat(channel), &subchan->fmt);
	ast_format_copy(ast_channel_readformat(channel), &subchan->fmt);
	ast_format_copy(ast_channel_rawreadformat(channel), &subchan->fmt);

	ast_module_ref(sccp_module_info->self);

	return 0;
}

static void sccp_subchannel_set_rtp_remote_address(struct sccp_subchannel *subchan)
{
	struct ast_sockaddr remote_tmp;

	ast_sockaddr_from_sin(&remote_tmp, &subchan->line->device->remote);
	ast_rtp_instance_set_remote_address(subchan->rtp, &remote_tmp);
}

static void sccp_subchannel_get_rtp_local_address(struct sccp_subchannel *subchan, struct sockaddr_in *local)
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

	/* destroy the subchans */
	AST_LIST_TRAVERSE_SAFE_BEGIN(&line->subchans, subchan, list) {
		sccp_subchannel_destroy(subchan);
		AST_LIST_REMOVE_CURRENT(list);
		ao2_ref(subchan, -1);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	/* update the line devstate */
	sccp_line_update_devstate(line, AST_DEVICE_UNAVAILABLE);
}

static void sccp_line_update_devstate(struct sccp_line *line, enum ast_device_state state)
{
	ast_devstate_changed(state, AST_DEVSTATE_CACHABLE, SCCP_LINE_PREFIX "/%s", line->cfg->name);
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

static int sccp_lines_init(struct sccp_lines *lines, struct sccp_device *device, uint32_t instance)
{
	struct sccp_line *line;

	line = sccp_line_alloc(device->cfg->line_cfg, device, instance);
	if (!line) {
		return -1;
	}

	lines->line = line;
	lines->count = 1;

	return 0;
}

static void sccp_lines_deinit(struct sccp_lines *lines)
{
	ao2_ref(lines->line, -1);
}

/*
 * Must be called with the device locked.
 */
static void sccp_lines_destroy(struct sccp_lines *lines)
{
	sccp_line_destroy(lines->line);
}

/*
 * Must be called with the device locked.
 */
static void sccp_lines_on_registration_success(struct sccp_lines *lines)
{
	sccp_line_update_devstate(lines->line, AST_DEVICE_NOT_INUSE);
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_line *sccp_lines_get_by_instance(struct sccp_lines *lines, uint32_t instance)
{
	struct sccp_line *line = lines->line;

	if (line->instance == instance) {
		return line;
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_line *sccp_lines_get_default(struct sccp_lines *lines)
{
	return lines->line;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_subchannel *sccp_lines_get_subchan(struct sccp_lines *lines, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan;

	AST_LIST_TRAVERSE(&lines->line->subchans, subchan, list) {
		if (subchan->id == subchan_id) {
			return subchan;
		}
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_subchannel *sccp_lines_get_next_ringin_subchan(struct sccp_lines *lines)
{
	struct sccp_subchannel *subchan;

	AST_LIST_TRAVERSE(&lines->line->subchans, subchan, list) {
		if (subchan->state == SCCP_RINGIN) {
			return subchan;
		}
	}

	return NULL;
}

/*
 * \note reference count is NOT incremented
 */
static struct sccp_subchannel *sccp_lines_get_next_offhook_subchan(struct sccp_lines *lines)
{
	struct sccp_subchannel *subchan;

	AST_LIST_TRAVERSE(&lines->line->subchans, subchan, list) {
		if (subchan->state == SCCP_OFFHOOK) {
			return subchan;
		}
	}

	return NULL;
}

static int sccp_lines_has_subchans(struct sccp_lines *lines)
{
	return !AST_LIST_EMPTY(&lines->line->subchans);
}

static int sccp_lines_test_apply_config(struct sccp_lines *lines, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_line_cfg *new_line_cfg = new_device_cfg->line_cfg;
	struct sccp_line_cfg *old_line_cfg = lines->line->cfg;

	if (strcmp(old_line_cfg->name, new_line_cfg->name)) {
		return 0;
	}

	if (strcmp(old_line_cfg->cid_num, new_line_cfg->cid_num)) {
		return 0;
	}

	if (strcmp(old_line_cfg->cid_name, new_line_cfg->cid_name)) {
		return 0;
	}

	/* the context is also used as the voicemail context and speeddial hint context */
	if (strcmp(old_line_cfg->context, new_line_cfg->context)) {
		return 0;
	}

	return 1;
}

static void sccp_lines_apply_config(struct sccp_lines *lines, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_line *line = lines->line;

	ao2_ref(line->cfg, -1);
	line->cfg = new_device_cfg->line_cfg;
	ao2_ref(line->cfg, +1);
}

static void sccp_device_destructor(void *data)
{
	struct sccp_device *device = data;

	/* no, it is NOT missing an sccp_lines_deinit(&device->line) nor a
	 * sccp_speeddials_deinit(&device->group). Only completely created
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
	sccp_msg_builder_init(&device->msg_builder, info->proto_version);
	sccp_queue_init(&device->nolock_tasks, sizeof(struct nolock_task));
	device->session = session;
	ao2_ref(session, +1);
	device->cfg = cfg;
	ao2_ref(cfg, +1);
	device->state = STATE_NEW;
	device->caps = caps;
	device->mwi_event_sub = NULL;
	device->active_subchan = NULL;
	/* The callid is not initialized to 1 since the 7940 needs a power cycle
	   to track calls with a callid lower than the last callid in it's outgoing
	   call history. ie after an asterisk restart
	   A power cycle will be required when the variable overflows...
	 */
	device->serial_callid = time(NULL);
	device->recv_chan_status = SCCP_RECV_CHAN_CLOSED;
	device->reset_on_idle = 0;
	device->dnd = 0;
	device->callfwd = SCCP_CFWD_INACTIVE;
	device->flags = 0;
	device->type = info->type;
	device->proto_version = info->proto_version;
	ast_copy_string(device->name, info->name, sizeof(device->name));
	device->exten[0] = '\0';
	device->last_exten[0] = '\0';
	device->callfwd_exten[0] = '\0';

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

	if (sccp_lines_init(&device->lines, device, LINE_INSTANCE_START)) {
		ao2_ref(device, -1);
		return NULL;
	}

	if (sccp_speeddials_init(&device->speeddials, device, LINE_INSTANCE_START + device->lines.count)) {
		sccp_lines_deinit(&device->lines);
		ao2_ref(device, -1);
		return NULL;
	}

	return device;
}

void sccp_device_destroy(struct sccp_device *device)
{
	sccp_speeddials_destroy(&device->speeddials);
	unsubscribe_mwi(device);

	sccp_device_lock(device);

	sccp_lines_destroy(&device->lines);
	sccp_lines_deinit(&device->lines);
	sccp_speeddials_deinit(&device->speeddials);

	switch (device->state) {
	case STATE_NEW:
	case STATE_CONNLOST:
		break;
	default:
		transmit_reset(device, SCCP_RESET_SOFT);
		break;
	}

	ast_set_flag(device, DEVICE_DESTROYED);

	sccp_device_unlock(device);
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

static int sccp_device_is_idle(struct sccp_device *device)
{
	return !sccp_lines_has_subchans(&device->lines);
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
	size_t n = 0;
	size_t i;

	/* add the line */
	definition[n].buttonDefinition = BT_LINE;
	definition[n].lineInstance = device->lines.line->instance;
	n++;

	/* add the speeddials */
	for (i = 0; i < device->speeddials.count && n < MAX_BUTTON_DEFINITION; i++) {
		definition[n].buttonDefinition = BT_FEATUREBUTTON;
		definition[n].lineInstance = device->speeddials.arr[i]->instance;
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

static void transmit_callstate(struct sccp_device *device, enum sccp_state state, uint32_t line_instance, uint32_t callid)
{
	struct sccp_msg msg;

	sccp_msg_callstate(&msg, state, line_instance, callid);
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

	device->recv_chan_status = SCCP_RECV_CHAN_CLOSED;

	sccp_msg_close_receive_channel(&msg, callid);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_config_status_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	sccp_msg_config_status_res(&msg, device->name, device->lines.count, device->speeddials.count);
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

static void transmit_display_message(struct sccp_device *device, const char *text)
{
	struct sccp_msg msg;

	sccp_msg_display_message(&msg, text);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_feature_status(struct sccp_device *device, struct sccp_speeddial *sd)
{
	struct sccp_msg msg;

	sccp_msg_feature_status(&msg, sd->instance, BT_FEATUREBUTTON, sccp_speeddial_status(device, sd), sd->cfg->label);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_forward_status_res(struct sccp_device *device, uint32_t line_instance, const char *extension, uint32_t status)
{
	struct sccp_msg msg;

	sccp_msg_forward_status_res(&msg, line_instance, extension, status);
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

	sccp_msg_time_date_res(&msg, device->cfg->tzoffset);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_tone(struct sccp_device *device, enum sccp_tone tone, uint32_t line_instance, uint32_t callid)
{
	struct sccp_msg msg;

	sccp_msg_tone(&msg, tone, line_instance, callid);
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_version_res(struct sccp_device *device)
{
	struct sccp_msg msg;

	/* hardcoded firmware version value taken from chan_skinny */
	sccp_msg_version_res(&msg, "P002F202");
	sccp_session_transmit_msg(device->session, &msg);
}

static void transmit_line_forward_status_res(struct sccp_device *device, struct sccp_line *line)
{
	uint32_t status = device->callfwd == SCCP_CFWD_ACTIVE ? 1 : 0;

	transmit_forward_status_res(device, line->instance, device->callfwd_exten, status);
}

static void transmit_line_lamp_state(struct sccp_device *device, struct sccp_line *line, enum sccp_lamp_state indication)
{
	transmit_lamp_state(device, STIMULUS_LINE, line->instance, indication);
}

static void transmit_subchan_callstate(struct sccp_device *device, struct sccp_subchannel *subchan, enum sccp_state state)
{
	transmit_callstate(device, state, subchan->line->instance, subchan->id);
}

static void transmit_subchan_open_receive_channel(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	struct sccp_msg msg;
	struct ast_format_list fmt;

	switch (device->recv_chan_status) {
	case SCCP_RECV_CHAN_CLOSED:
		break;
	case SCCP_RECV_CHAN_OPENING:
	case SCCP_RECV_CHAN_OPENED:
		ast_debug(1, "%s: receive channel already opening/opened (%d)\n", device->name, device->recv_chan_status);
		return;
	}

	device->recv_chan_status = SCCP_RECV_CHAN_OPENING;

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
	device->state = STATE_CONNLOST;
}

static void update_displaymessage(struct sccp_device *device)
{
	char text[AST_MAX_EXTENSION + 21];

	if (!device->dnd && device->callfwd != SCCP_CFWD_ACTIVE) {
		transmit_clear_message(device);
		return;
	}

	text[0] = '\0';

	if (device->dnd) {
		strcat(text, "\200\77");
	}

	strcat(text, "     ");

	if (device->callfwd == SCCP_CFWD_ACTIVE) {
		strcat(text, "\200\5: ");
		strncat(text, device->callfwd_exten, AST_MAX_EXTENSION);
	}

	transmit_display_message(device, text);
}

static void set_callforward(struct sccp_device *device, const char *exten)
{
	struct sccp_line *line = sccp_lines_get_default(&device->lines);

	device->callfwd = SCCP_CFWD_ACTIVE;
	ast_copy_string(device->callfwd_exten, exten, sizeof(device->callfwd_exten));
	line->state = SCCP_ONHOOK;

	remove_fwdtimeout_task(device);
	ast_db_put("sccp/cfwdall", device->name, device->callfwd_exten);

	transmit_callstate(device, SCCP_ONHOOK, line->instance, device->callfwd_id);
	transmit_line_forward_status_res(device, line);
	transmit_speaker_mode(device, SCCP_SPEAKEROFF);
	update_displaymessage(device);
}

static void set_callforward_from_device_exten(struct sccp_device *device)
{
	set_callforward(device, device->exten);
	device->exten[0] = '\0';
}

static void clear_callforward(struct sccp_device *device)
{
	struct sccp_line *line = sccp_lines_get_default(&device->lines);

	device->callfwd = SCCP_CFWD_INACTIVE;
	device->callfwd_exten[0] = '\0';

	ast_db_del("sccp/cfwdall", device->name);

	transmit_line_forward_status_res(device, line);
	update_displaymessage(device);
}

static void cancel_callforward_input(struct sccp_device *device)
{
	struct sccp_line *line = sccp_lines_get_default(&device->lines);

	device->callfwd = SCCP_CFWD_INACTIVE;
	device->exten[0] = '\0';
	line->state = SCCP_ONHOOK;

	remove_fwdtimeout_task(device);

	transmit_callstate(device, SCCP_ONHOOK, line->instance, device->callfwd_id);
	transmit_speaker_mode(device, SCCP_SPEAKEROFF);
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
	const char *context = sccp_lines_get_default(&device->lines)->cfg->context;

	if (ast_strlen_zero(device->cfg->voicemail)) {
		return;
	}

	device->mwi_event_sub = ast_event_subscribe(AST_EVENT_MWI, on_mwi_event, "sccp mwi subsciption", device,
			AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR, device->cfg->voicemail,
			AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR, context,
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

	device->active_subchan = subchan;

	return 0;
}

static struct sccp_subchannel *do_newcall(struct sccp_device *device)
{
	struct sccp_line *line = sccp_lines_get_default(&device->lines);
	struct sccp_subchannel *subchan;

	subchan = sccp_lines_get_next_offhook_subchan(&device->lines);
	if (subchan) {
		ast_debug(1, "Found an already offhook subchan\n");
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

	sccp_line_update_devstate(line, AST_DEVICE_INUSE);

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

	sccp_line_update_devstate(line, AST_DEVICE_INUSE);

	return 0;
}

static void do_clear_subchannel(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;

	/* XXX hum, that's a bit ugly */

	if (subchan->rtp) {
		if (subchan == device->active_subchan) {
			transmit_close_receive_channel(device, subchan->id);
			transmit_stop_media_transmission(device, subchan->id);
		}

		ast_rtp_instance_stop(subchan->rtp);
		ast_rtp_instance_destroy(subchan->rtp);
		subchan->rtp = NULL;
	} else {
		if (subchan == device->active_subchan && device->recv_chan_status == SCCP_RECV_CHAN_OPENING) {
			transmit_close_receive_channel(device, subchan->id);
		}
	}

	transmit_ringer_mode(device, SCCP_RING_OFF);
	transmit_subchan_callstate(device, subchan, SCCP_ONHOOK);
	transmit_subchan_stop_tone(device, subchan);

	subchan->channel = NULL;

	if (subchan->related) {
		subchan->related->related = NULL;
		subchan->related = NULL;
	}

	/* the ref is decremented at the end of the function */
	AST_LIST_REMOVE(&line->subchans, subchan, list);
	if (AST_LIST_EMPTY(&line->subchans)) {
		transmit_speaker_mode(device, SCCP_SPEAKEROFF);
		line->state = SCCP_ONHOOK;
		sccp_line_update_devstate(line, AST_DEVICE_NOT_INUSE);
	}

	if (subchan == device->active_subchan) {
		device->active_subchan = NULL;
	}

	if (device->reset_on_idle && sccp_device_is_idle(device)) {
		transmit_reset(device, SCCP_RESET_SOFT);
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

/* XXX whacked out operation that unlock the device and relock it after
 *     when this function returns, you must check that your invariant still make
 *     sense -- the better is to have nothing to do after this function return
 * Must be called from the session thread.
 */
static int start_the_call(struct sccp_device *device, struct sccp_subchannel *subchan)
{
	struct sccp_line *line = subchan->line;
	struct ast_channel *channel;

	remove_dialtimeout_task(device, subchan);

	sccp_device_unlock(device);

	/* we are in the session thread and line->cfg and device->exten are updated only in
	 * session thread, so there's no race condition here
	 */
	channel = alloc_channel(line->cfg, device->exten, NULL);

	sccp_device_lock(device);

	if (!channel) {
		return -1;
	}

	/* XXX if panic can be called from any thread, maybe we should test that we have
	 * not panicked here -- no need to check if the device is destroyed though, since
	 * device destroying happens only in the session thread
	 */

	if (sccp_subchannel_set_channel(subchan, channel, NULL)) {
		do_clear_subchannel(device, subchan);
		goto error;
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
	transmit_callinfo(device, "", line->cfg->cid_num, "", device->exten, line->instance, subchan->id, subchan->direction);

	memcpy(device->last_exten, device->exten, AST_MAX_EXTENSION);
	line->device->exten[0] = '\0';

	if (!strcmp(device->last_exten, ast_pickup_ext())) {
		add_pickup_channel_task(device, subchan->channel);
	} else {
		add_start_channel_task(device, subchan->channel, line->cfg);
	}

	return 0;

error:
	sccp_device_unlock(device);
	ast_channel_release(channel);
	sccp_device_lock(device);

	return -1;
}

static void do_speeddial_action(struct sccp_device *device, struct sccp_speeddial *sd)
{
	struct sccp_subchannel *subchan;

	if (device->callfwd == SCCP_CFWD_INPUTEXTEN) {
		set_callforward(device, sd->cfg->extension);
	} else {
		/* XXX this 3 steps stuff should be simplified into one function */
		subchan = do_newcall(device);
		if (!subchan) {
			return;
		}

		/* open our speaker */
		transmit_speaker_mode(device, SCCP_SPEAKERON);

		ast_copy_string(device->exten, sd->cfg->extension, sizeof(device->exten));
		start_the_call(device, subchan);
	}
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
	struct sccp_subchannel *subchan = sccp_lines_get_next_offhook_subchan(&device->lines);
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
	struct sccp_speeddial *sd;
	uint32_t instance = letohl(msg->data.feature.instance);

	sd = sccp_speeddials_get_by_instance(&device->speeddials, instance);
	if (!sd) {
		ast_log(LOG_NOTICE, "No speeddial [%d] on device [%s]\n", instance, device->name);
		sccp_session_stop(device->session);
		return;
	}

	transmit_feature_status(device, sd);
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
		line = sccp_lines_get_default(&device->lines);
		break;
	default:
		line = sccp_lines_get_by_instance(&device->lines, instance);
		break;
	}

	if (!line) {
		ast_debug(1, "Device [%s] has no line instance [%d]\n", device->name, instance);
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

	if (line->state == SCCP_CONNECTED || line->state == SCCP_PROGRESS) {
		/* Workaround for bug #4503 and bug #4841 */
		if (device->active_subchan && device->active_subchan->channel) {
			add_ast_queue_frame_dtmf_task(device, device->active_subchan->channel, digit);
		}
	} else if (line->state == SCCP_OFFHOOK) {
		len = strlen(device->exten);
		if (len < sizeof(device->exten) - 1 && digit != '#') {
			device->exten[len] = digit;
			device->exten[len+1] = '\0';
		}

		if (device->callfwd == SCCP_CFWD_INPUTEXTEN) {
			if (digit == '#') {
				set_callforward_from_device_exten(device);
			} else {
				add_fwdtimeout_task(device);
			}
		} else {
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
		}
	}
}

static void handle_msg_line_status_req(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_line *line;
	uint32_t instance = letohl(msg->data.line.lineInstance);

	line = sccp_lines_get_by_instance(&device->lines, instance);
	if (!line) {
		ast_log(LOG_NOTICE, "Line instance [%d] is not attached to device [%s]\n", instance, device->name);
		sccp_session_stop(device->session);
		return;
	}

	transmit_line_status_res(device, line);
	transmit_line_forward_status_res(device, line);
}

static void handle_msg_onhook(struct sccp_device *device, struct sccp_msg *msg)
{
	struct sccp_subchannel *subchan;
	uint32_t subchan_id;

	if (device->proto_version == 11) {
		subchan_id = letohl(msg->data.onhook.callInstance);
		subchan = sccp_lines_get_subchan(&device->lines, subchan_id);
		if (!subchan) {
			ast_log(LOG_NOTICE, "handle msg onhook failed: no subchan %u\n", subchan_id);
			return;
		}
	} else {
		subchan = device->active_subchan;
		if (!subchan) {
			ast_debug(1, "handle msg onhook failed: no active subchan\n");
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
			subchan = sccp_lines_get_subchan(&device->lines, subchan_id);
			if (!subchan) {
				ast_log(LOG_NOTICE, "handle msg offhook failed: no subchan %u\n", subchan_id);
				return;
			}

			do_answer(device, subchan);
		}
	} else {
		subchan = sccp_lines_get_next_ringin_subchan(&device->lines);
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

	if (device->recv_chan_status == SCCP_RECV_CHAN_OPENING) {
		device->recv_chan_status = SCCP_RECV_CHAN_OPENED;
	}

	if (!device->active_subchan) {
		return;
	}

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
	struct sccp_subchannel *subchan = sccp_lines_get_subchan(&device->lines, subchan_id);

	transmit_speaker_mode(device, SCCP_SPEAKERON);

	if (!subchan) {
		ast_log(LOG_NOTICE, "handle softkey answer failed: no subchan %u\n", subchan_id);
		return;
	}

	do_answer(device, subchan);
}

static void handle_softkey_bkspc(struct sccp_device *device)
{
	if (device->callfwd == SCCP_CFWD_INPUTEXTEN) {
		cancel_callforward_input(device);
	}
}

static void handle_softkey_cfwdall(struct sccp_device *device)
{
	struct sccp_line *line = sccp_lines_get_default(&device->lines);

	switch (device->callfwd) {
	case SCCP_CFWD_INACTIVE:
		device->callfwd_id = device->serial_callid++;
		device->callfwd = SCCP_CFWD_INPUTEXTEN;
		line->state = SCCP_OFFHOOK;

		transmit_callstate(device, SCCP_OFFHOOK, line->instance, device->callfwd_id);
		transmit_selectsoftkeys(device, line->instance, device->callfwd_id, KEYDEF_CALLFWD);
		transmit_speaker_mode(device, SCCP_SPEAKERON);
		break;

	case SCCP_CFWD_INPUTEXTEN:
		if (ast_strlen_zero(device->exten)) {
			cancel_callforward_input(device);
		} else {
			set_callforward_from_device_exten(device);
		}

		break;

	case SCCP_CFWD_ACTIVE:
		clear_callforward(device);
		break;
	}
}

static void handle_softkey_dnd(struct sccp_device *device)
{
	if (device->dnd) {
		device->dnd = 0;
		ast_db_del("sccp/dnd", device->name);
	} else {
		device->dnd = 1;
		ast_db_put("sccp/dnd", device->name, "on");
	}

	update_displaymessage(device);
}

static void handle_softkey_endcall(struct sccp_device *device, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan = sccp_lines_get_subchan(&device->lines, subchan_id);

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
	struct sccp_subchannel *subchan = sccp_lines_get_subchan(&device->lines, subchan_id);

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

static void handle_softkey_transfer(struct sccp_device *device, uint32_t line_instance)
{
	struct sccp_line *line = sccp_lines_get_by_instance(&device->lines, line_instance);
	struct sccp_subchannel *subchan;
	struct sccp_subchannel *xfer_subchan;

	if (!line) {
		ast_log(LOG_NOTICE, "handle softkey transfer failed: no line %u\n", line_instance);
		return;
	}

	if (!device->active_subchan) {
		ast_log(LOG_NOTICE, "handle softkey transfer failed: no active subchan\n");
		return;
	}

	if (!device->active_subchan->channel) {
		ast_log(LOG_NOTICE, "handle softkey transfer failed: no channel on subchan\n");
		return;
	}

	/* first time we press transfer */
	if (!device->active_subchan->related) {
		xfer_subchan = device->active_subchan;

		/* put on hold */
		if (do_hold(device)) {
			ast_log(LOG_NOTICE, "handle softkey transfer failed: could not put active subchan on hold\n");
			return;
		}

		/* XXX compatibility stuff, would probably be better to add a parameter to do_hold... */
		transmit_speaker_mode(device, SCCP_SPEAKERON);

		/* spawn a new subchannel instance and mark both as related */
		subchan = sccp_line_new_subchannel(line, SCCP_DIR_OUTGOING);
		if (!subchan) {
			return;
		}

		subchan->transferring = 1;

		xfer_subchan->related = subchan;
		subchan->related = xfer_subchan;

		device->active_subchan = subchan;
		line->state = SCCP_OFFHOOK;

		transmit_subchan_callstate(device, subchan, SCCP_OFFHOOK);
		transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_DIALINTRANSFER);
		transmit_subchan_tone(device, subchan, SCCP_TONE_DIAL);
	} else {
		struct ast_channel *active_channel = device->active_subchan->channel;
		struct ast_channel *related_channel = device->active_subchan->related->channel;

		if (!related_channel) {
			ast_log(LOG_NOTICE, "ignoring transfer softkey event; related channel is NULL\n");
			return;
		}

		if (ast_channel_state(active_channel) == AST_STATE_DOWN
			|| ast_channel_state(related_channel) == AST_STATE_DOWN) {
			return;
		}

		if (ast_bridged_channel(related_channel)) {
			add_ast_channel_transfer_masquerade_task(device, active_channel, related_channel);
			add_ast_queue_hangup_task(device, related_channel);
		} else {
			add_ast_queue_hangup_task(device, active_channel);
		}
	}
}

static void handle_msg_softkey_event(struct sccp_device *device, struct sccp_msg *msg)
{
	uint32_t softkey_event = letohl(msg->data.softkeyevent.softKeyEvent);
	uint32_t line_instance = letohl(msg->data.softkeyevent.lineInstance);
	uint32_t call_instance = letohl(msg->data.softkeyevent.callInstance);

	ast_debug(1, "Softkey event message: event 0x%02X, line_instance %u, subchan_id %u\n",
			softkey_event, line_instance, call_instance);

	switch (softkey_event) {
	case SOFTKEY_DND:
		handle_softkey_dnd(device);
		break;

	case SOFTKEY_REDIAL:
		handle_softkey_redial(device);
		break;

	case SOFTKEY_NEWCALL:
		handle_softkey_newcall(device);
		break;

	case SOFTKEY_HOLD:
		handle_softkey_hold(device);
		break;

	case SOFTKEY_TRNSFER:
		handle_softkey_transfer(device, line_instance);
		break;

	case SOFTKEY_CFWDALL:
		handle_softkey_cfwdall(device);
		break;

	case SOFTKEY_BKSPC:
		handle_softkey_bkspc(device);
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
	struct sccp_speeddial *sd;
	uint32_t index = letohl(msg->data.speeddial.instance);

	sd = sccp_speeddials_get_by_index(&device->speeddials, index);
	if (!sd) {
		ast_debug(2, "No speeddial [%d] on device [%s]\n", index, device->name);
		return;
	}

	transmit_speeddial_stat_res(device, sd);
}

static void handle_stimulus_featurebutton(struct sccp_device *device, uint32_t instance)
{
	struct sccp_speeddial *sd = sccp_speeddials_get_by_instance(&device->speeddials, instance);

	if (!sd) {
		ast_log(LOG_NOTICE, "handle stimulus featurebutton failed: speeddial %u not found\n", instance);
		return;
	}

	do_speeddial_action(device, sd);
}

static void handle_stimulus_speeddial(struct sccp_device *device, uint32_t index)
{
	struct sccp_speeddial *sd = sccp_speeddials_get_by_index(&device->speeddials, index);

	if (!sd) {
		ast_log(LOG_NOTICE, "handle stimulus speeddial failed: speeddial %u not found\n", index);
		return;
	}

	do_speeddial_action(device, sd);
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
	uint32_t line_instance = letohl(msg->data.stimulus.lineInstance);

	switch (stimulus) {
	case STIMULUS_FEATUREBUTTON:
		handle_stimulus_featurebutton(device, line_instance);
		break;
	case STIMULUS_SPEEDDIAL:
		handle_stimulus_speeddial(device, line_instance);
		break;
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

static void handle_msg_version_req(struct sccp_device *device)
{
	transmit_version_res(device);
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

	case VERSION_REQ_MESSAGE:
		handle_msg_version_req(device);
		break;
	}
}

int sccp_device_handle_msg(struct sccp_device *device, struct sccp_msg *msg)
{
	uint32_t msg_id;

	if (!msg) {
		ast_log(LOG_ERROR, "sccp device handle msg failed: msg is null\n");
		return -1;
	}

	msg_id = letohl(msg->id);

	sccp_device_lock(device);

	switch (device->state) {
	case STATE_WORKING:
		handle_msg_state_common(device, msg, msg_id);
		break;
	default:
		break;
	}

	sccp_device_unlock(device);

	return 0;
}

static int sccp_device_test_apply_config(struct sccp_device *device, struct sccp_device_cfg *new_device_cfg)
{
	struct sccp_device_cfg *old_device_cfg = device->cfg;

	if (strcmp(old_device_cfg->dateformat, new_device_cfg->dateformat)) {
		return 0;
	}

	if (strcmp(old_device_cfg->voicemail, new_device_cfg->voicemail)) {
		return 0;
	}

	if (old_device_cfg->keepalive != new_device_cfg->keepalive) {
		return 0;
	}

	if (old_device_cfg->tzoffset != new_device_cfg->tzoffset) {
		return 0;
	}

	if (!sccp_lines_test_apply_config(&device->lines, new_device_cfg)) {
		return 0;
	}

	if (!sccp_speeddials_test_apply_config(&device->speeddials, new_device_cfg)) {
		return 0;
	}

	return 1;
}

int sccp_device_reload_config(struct sccp_device *device, struct sccp_device_cfg *new_device_cfg)
{
	if (!new_device_cfg) {
		ast_log(LOG_ERROR, "sccp device reload config failed: device_cfg is null\n");
		return -1;
	}

	if (!sccp_device_test_apply_config(device, new_device_cfg)) {
		sccp_device_lock(device);
		if (sccp_device_is_idle(device)) {
			transmit_reset(device, SCCP_RESET_SOFT);
		} else {
			device->reset_on_idle = 1;
		}

		sccp_device_unlock(device);

		return 0;
	}

	sccp_device_lock(device);

	ao2_ref(device->cfg, -1);
	device->cfg = new_device_cfg;
	ao2_ref(device->cfg, +1);

	sccp_lines_apply_config(&device->lines, new_device_cfg);
	sccp_speeddials_apply_config(&device->speeddials, new_device_cfg);

	sccp_device_unlock(device);

	return 0;
}

void sccp_device_on_connection_lost(struct sccp_device *device)
{
	sccp_device_lock(device);
	device->state = STATE_CONNLOST;
	sccp_device_unlock(device);
}

/*
 * entry point: yes
 * thread: session
 */
static void on_keepalive_timeout(struct sccp_device *device, void __attribute__((unused)) *data)
{
	ast_log(LOG_NOTICE, "Device %s has timed out\n", device->name);

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
 * entry point:  yes
 * thread: session
 */
static void on_fwd_timeout(struct sccp_device *device, void __attribute__((unused)) *data)
{
	sccp_device_lock(device);
	set_callforward_from_device_exten(device);
	sccp_device_unlock(device);
}

/*
 * thread: session
 */
static int add_fwdtimeout_task(struct sccp_device *device)
{
	return sccp_session_add_device_task(device->session, on_fwd_timeout, NULL, 5);
}

static void remove_fwdtimeout_task(struct sccp_device *device)
{
	sccp_session_remove_device_task(device->session, on_fwd_timeout, NULL);
}

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

static void init_dnd(struct sccp_device *device)
{
	char dnd_status[4];

	if (!ast_db_get("sccp/dnd", device->name, dnd_status, sizeof(dnd_status))) {
		device->dnd = 1;
	} else {
		device->dnd = 0;
	}
}

static void init_callfwd(struct sccp_device *device)
{
	char exten[AST_MAX_EXTENSION];

	if (!ast_db_get("sccp/cfwdall", device->name, exten, sizeof(exten))) {
		set_callforward(device, exten);
	} else {
		struct sccp_line *line = sccp_lines_get_default(&device->lines);

		/* callforward was set on the default line previously, so also check
		 * if the default line has an entry in the ast_db
		 */
		if (!ast_db_get("sccp/cfwdall", line->name, exten, sizeof(exten))) {
			set_callforward(device, exten);
			ast_db_del("sccp/cfwdall", line->name);
		}
	}
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

	init_dnd(device);
	init_callfwd(device);
	init_voicemail_lamp_state(device);

	update_displaymessage(device);

	add_keepalive_task(device);

	device->state = STATE_WORKING;

	sccp_lines_on_registration_success(&device->lines);

	sccp_device_unlock(device);

	subscribe_mwi(device);
	sccp_speeddials_on_registration_success(&device->speeddials);
}

int sccp_device_reset(struct sccp_device *device, enum sccp_reset_type type)
{
	sccp_device_lock(device);
	if (!ast_test_flag(device, DEVICE_DESTROYED)) {
		transmit_reset(device, type);
	}

	sccp_device_unlock(device);

	return 0;
}

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
	return device->lines.count;
}

struct sccp_line* sccp_device_line(struct sccp_device *device, unsigned int i)
{
	if (i >= device->lines.count) {
		return NULL;
	}

	return device->lines.line;
}

const char *sccp_device_name(const struct sccp_device *device)
{
	return device->name;
}

const char *sccp_line_name(const struct sccp_line *line)
{
	return line->name;
}

static int channel_tech_requester_locked(struct sccp_device *device, struct sccp_line *line, struct ast_channel *channel, const char *options, struct ast_format_cap *cap, int *cause)
{
	struct sccp_subchannel *subchan;

	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		return -1;
	}

	if (device->dnd && device->callfwd == SCCP_CFWD_INACTIVE) {
		*cause = AST_CAUSE_BUSY;
		return -1;
	}

	subchan = sccp_line_new_subchannel(line, SCCP_DIR_INCOMING);
	if (!subchan) {
		return -1;
	}

	if (sccp_subchannel_set_channel(subchan, channel, cap)) {
		do_clear_subchannel(device, subchan);
		return -1;
	}

	if (options && !strncmp(options, "autoanswer", 10)) {
		subchan->autoanswer = 1;
	}

	if (device->callfwd == SCCP_CFWD_ACTIVE) {
		ast_debug(1, "setting call forward to %s\n", device->callfwd_exten);
		ast_channel_call_forward_set(channel, device->callfwd_exten);
	}

	return 0;
}

struct ast_channel *sccp_channel_tech_requester(struct sccp_line *line, const char *options, struct ast_format_cap *cap, const struct ast_channel *requestor, int *cause)
{
	struct sccp_device *device = line->device;
	struct sccp_line_cfg *line_cfg;
	struct ast_channel *channel;
	int res;

	sccp_device_lock(device);
	line_cfg = line->cfg;
	ao2_ref(line_cfg, +1);
	sccp_device_unlock(device);

	channel = alloc_channel(line_cfg, "", requestor ? ast_channel_linkedid(requestor) : NULL);
	ao2_ref(line_cfg, -1);
	if (!channel) {
		return NULL;
	}

	sccp_device_lock(device);
	res = channel_tech_requester_locked(device, line, channel, options, cap, cause);
	sccp_device_unlock(device);

	if (res) {
		ast_channel_release(channel);
		return NULL;
	}

	return channel;
}

int sccp_channel_tech_devicestate(const struct sccp_line *line)
{
	struct sccp_device *device = line->device;
	int res;

	sccp_device_lock(device);
	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		res = AST_DEVICE_UNAVAILABLE;
	} else if (line->state == SCCP_ONHOOK) {
		res = AST_DEVICE_NOT_INUSE;
	} else {
		res = AST_DEVICE_INUSE;
	}

	sccp_device_unlock(device);

	return res;
}

static void format_party_name(struct ast_channel *channel, char *name, size_t n)
{
	struct ast_party_redirecting *redirect = ast_channel_redirecting(channel);
	struct ast_party_connected_line *connected = ast_channel_connected(channel);

	if (redirect->from.name.valid) {
		snprintf(name, n, "%s -> %s", redirect->from.name.str, connected->id.name.str);
	} else if (redirect->from.number.valid) {
		snprintf(name, n, "%s -> %s", redirect->from.number.str, connected->id.name.str);
	} else if (connected->id.name.str) {
		snprintf(name, n, "%s", connected->id.name.str);
	} else {
		ast_copy_string(name, "unknown", n);
	}
}

static void format_party_number(struct ast_channel *channel, char **number)
{
	if (ast_channel_connected(channel)->id.number.str) {
		*number = ast_channel_connected(channel)->id.number.str;
	} else {
		*number = "";
	}
}

int sccp_channel_tech_call(struct ast_channel *channel, const char *dest, int timeout)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	char name[64];
	char *number;
	int res = 0;

	ast_setstate(channel, AST_STATE_RINGING);
	ast_queue_control(channel, AST_CONTROL_RINGING);

	sccp_device_lock(device);

	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		res = -1;
		goto unlock;
	}

	if (device->callfwd == SCCP_CFWD_ACTIVE) {
		struct ast_party_redirecting redirecting;
		struct ast_set_party_redirecting update_redirecting;

		ast_party_redirecting_init(&redirecting);
		memset(&update_redirecting, 0, sizeof(update_redirecting));

		redirecting.from.name.str = ast_strdup(line->cfg->cid_name);
		redirecting.from.name.valid = 1;
		update_redirecting.from.name = 1;
		redirecting.from.number.str = ast_strdup(line->cfg->cid_num);
		redirecting.from.number.valid = 1;
		update_redirecting.from.number = 1;
		redirecting.reason = AST_REDIRECTING_REASON_UNCONDITIONAL;
		redirecting.count = 1;

		ast_channel_set_redirecting(channel, &redirecting, &update_redirecting);
		ast_party_redirecting_free(&redirecting);

		goto unlock;
	}

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

	if (subchan->autoanswer) {
		transmit_speaker_mode(device, SCCP_SPEAKERON);
		do_answer(device, subchan);
	} else {
		sccp_line_update_devstate(line, AST_DEVICE_RINGING);
	}

unlock:
	sccp_device_unlock(device);

	return res;
}

int sccp_channel_tech_hangup(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;

	sccp_device_lock(device);

	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		if (subchan->rtp) {
			ast_rtp_instance_stop(subchan->rtp);
			ast_rtp_instance_destroy(subchan->rtp);
			subchan->rtp = NULL;
		}

		subchan->channel = NULL;
	} else {
		do_clear_subchannel(device, subchan);
	}

	sccp_device_unlock(device);

	ast_setstate(channel, AST_STATE_DOWN);
	ast_channel_tech_pvt_set(channel, NULL);
	ao2_ref(subchan, -1);

	ast_module_unref(sccp_module_info->self);

	return 0;
}

int sccp_channel_tech_answer(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	int wait_subchan_rtp = 0;

	sccp_device_lock(device);

	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		sccp_device_unlock(device);
		return -1;
	}

	if (!subchan->rtp) {
		transmit_subchan_open_receive_channel(device, subchan);
		wait_subchan_rtp = 1;
	}

	if (subchan != device->active_subchan) {
		sccp_device_unlock(device);
		return 0;
	}

	transmit_subchan_callstate(device, subchan, SCCP_CONNECTED);
	transmit_subchan_stop_tone(device, subchan);
	transmit_subchan_selectsoftkeys(device, subchan, KEYDEF_CONNECTED);

	line->state = SCCP_CONNECTED;

	sccp_device_unlock(device);

	/* Wait for the phone to provide his ip:port information
	 * before the bridging is being done.
	 *
	 * Must not be done while the device is locked.
	 *
	 * XXX should use something more robust...
	 */
	if (wait_subchan_rtp) {
		usleep(500000);
	}

	ast_setstate(channel, AST_STATE_UP);

	return 0;
}

struct ast_frame *sccp_channel_tech_read(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	struct ast_frame *frame;
	struct ast_rtp_instance *rtp = NULL;

	sccp_device_lock(device);

	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		goto unlock;
	}

	if (subchan->rtp) {
		rtp = subchan->rtp;
		ao2_ref(rtp, +1);
	}

unlock:
	sccp_device_unlock(device);

	if (!rtp) {
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

int sccp_channel_tech_write(struct ast_channel *channel, struct ast_frame *frame)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	int res = 0;

	sccp_device_lock(device);

	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		res = -1;
		goto unlock;
	}

	if (subchan->rtp) {
		if (line->state == SCCP_CONNECTED || line->state == SCCP_PROGRESS) {
			res = ast_rtp_instance_write(subchan->rtp, frame);
		}
	} else if (line->state == SCCP_PROGRESS) {
		/* handle early rtp during progress state */
		transmit_subchan_stop_tone(device, subchan);
		transmit_subchan_open_receive_channel(device, subchan);
	}

unlock:
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

int sccp_channel_tech_indicate(struct ast_channel *channel, int ind, const void *data, size_t datalen)
{
#define _AST_PROVIDE_INBAND_SIGNALLING -1
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	int res = 0;
	int start_moh = 0;
	int stop_moh = 0;

	sccp_device_lock(device);

	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		goto unlock;
	}

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
		if (subchan->rtp) {
			ast_rtp_instance_update_source(subchan->rtp);
		}

		break;

	case AST_CONTROL_SRCCHANGE:
		if (subchan->rtp) {
			ast_rtp_instance_change_source(subchan->rtp);
		}

		break;

	case AST_CONTROL_CONNECTED_LINE:
		indicate_connected_line(device, line, subchan, channel);
		break;
	}

unlock:
	sccp_device_unlock(device);

	/* XXX ugly solution... */
	if (start_moh) {
		ast_moh_start(channel, data, NULL);
	} else if (stop_moh) {
		ast_moh_stop(channel);
	}

	return res;
}

int sccp_channel_tech_fixup(struct ast_channel *oldchannel, struct ast_channel *newchannel)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(newchannel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;

	sccp_device_lock(device);
	subchan->channel = newchannel;
	sccp_device_unlock(device);

	return 0;
}

int sccp_channel_tech_send_digit_end(struct ast_channel *channel, char digit, unsigned int duration)
{
	return 0;
}

enum ast_rtp_glue_result sccp_rtp_glue_get_rtp_info(struct ast_channel *channel, struct ast_rtp_instance **instance)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	enum ast_rtp_glue_result res = AST_RTP_GLUE_RESULT_LOCAL;

	sccp_device_lock(device);

	if (!subchan->rtp) {
		ast_debug(1, "rtp is NULL\n");
		res = AST_RTP_GLUE_RESULT_FORBID;
		goto unlock;
	}

	ao2_ref(subchan->rtp, +1);
	*instance = subchan->rtp;

	if (line->cfg->directmedia) {
		res = AST_RTP_GLUE_RESULT_REMOTE;
	}

unlock:
	sccp_device_unlock(device);

	return res;
}

int sccp_rtp_glue_update_peer(struct ast_channel *channel, struct ast_rtp_instance *rtp, struct ast_rtp_instance *vrtp, struct ast_rtp_instance *trtp, const struct ast_format_cap *cap, int nat_active)
{
	struct sockaddr_in local;
	struct sockaddr_in endpoint;
	struct ast_sockaddr endpoint_tmp;
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);
	struct sccp_line *line = subchan->line;
	struct sccp_device *device = line->device;
	int changed;

	sccp_device_lock(device);

	if (ast_test_flag(device, DEVICE_DESTROYED)) {
		goto unlock;
	}

	if (subchan != device->active_subchan) {
		ast_debug(1, "not updating peer: subchan is not active\n");
		goto unlock;
	}

	sccp_subchannel_get_rtp_local_address(subchan, &local);

	if (!rtp) {
		transmit_stop_media_transmission(device, subchan->id);
		transmit_subchan_start_media_transmission(device, subchan, &local);
		ast_sockaddr_setnull(&subchan->direct_media_addr);
		goto unlock;
	}

	changed = ast_rtp_instance_get_and_cmp_remote_address(rtp, &subchan->direct_media_addr);
	if (!changed) {
		ast_debug(1, "not updating peer: remote address has not changed\n");
		goto unlock;
	}

	ast_rtp_instance_get_remote_address(rtp, &endpoint_tmp);
	ast_debug(1, "remote address %s\n", ast_sockaddr_stringify(&endpoint_tmp));

	ast_sockaddr_to_sin(&endpoint_tmp, &endpoint);
	if (endpoint.sin_addr.s_addr != 0) {
		transmit_stop_media_transmission(device, subchan->id);
		transmit_subchan_start_media_transmission(device, subchan, &endpoint);
		add_ast_queue_control_task(device, subchan->channel, AST_CONTROL_UPDATE_RTP_PEER);
	} else {
		ast_debug(1, "updating peer: remote address is 0, device will send media to asterisk\n");

		transmit_stop_media_transmission(device, subchan->id);
		transmit_subchan_start_media_transmission(device, subchan, &local);
	}

unlock:
	sccp_device_unlock(device);

	return 0;
}

void sccp_rtp_glue_get_codec(struct ast_channel *channel, struct ast_format_cap *result)
{
	struct sccp_subchannel *subchan = ast_channel_tech_pvt(channel);

	ast_format_cap_set(result, &subchan->fmt);
}
