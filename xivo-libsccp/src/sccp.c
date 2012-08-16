#include <asterisk.h>
#include <asterisk/app.h>
#include <asterisk/astdb.h>
#include <asterisk/causes.h>
#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/devicestate.h>
#include <asterisk/event.h>
#include <asterisk/io.h>
#include <asterisk/linkedlists.h>
#include <asterisk/module.h>
#include <asterisk/musiconhold.h>
#include <asterisk/netsock.h>
#include <asterisk/pbx.h>
#include <asterisk/poll-compat.h>
#include <asterisk/rtp_engine.h>
#include <asterisk/test.h>
#include <asterisk/utils.h>

#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <iconv.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "device.h"
#include "message.h"
#include "sccp.h"
#include "utils.h"

#define SCCP_PORT "2000"
#define SCCP_BACKLOG 50

static struct sccp_configs *sccp_config; /* global */
static AST_LIST_HEAD_STATIC(list_session, sccp_session);
static struct sched_context *sched = NULL;

static int handle_callforward(struct sccp_session *session, uint32_t softkey);
static int handle_softkey_hold(uint32_t line_instance, uint32_t subchan_id, struct sccp_session *session);

static struct ast_channel *cb_ast_request(const char *type, format_t format, const struct ast_channel *requestor, void *destination, int *cause);
static int cb_ast_call(struct ast_channel *ast, char *dest, int timeout);
static int cb_ast_devicestate(void *data);
static int cb_ast_hangup(struct ast_channel *ast);
static int cb_ast_answer(struct ast_channel *ast);
static struct ast_frame *cb_ast_read(struct ast_channel *ast);
static int cb_ast_write(struct ast_channel *ast, struct ast_frame *frame);
static int cb_ast_indicate(struct ast_channel *ast, int ind, const void *data, size_t datalen);
static int cb_ast_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
static int cb_ast_senddigit_begin(struct ast_channel *ast, char digit);
static int cb_ast_senddigit_end(struct ast_channel *ast, char digit, unsigned int duration);
static enum ast_rtp_glue_result cb_ast_get_rtp_peer(struct ast_channel *channel, struct ast_rtp_instance **instance);
static int cb_ast_set_rtp_peer(struct ast_channel *channel,
				struct ast_rtp_instance *rtp,
				struct ast_rtp_instance *vrtp,
				struct ast_rtp_instance *trtp,
				format_t codecs,
				int nat_active);


static const struct ast_channel_tech sccp_tech = {
	.type = "sccp",
	.description = "Skinny Client Control Protocol",
	.capabilities = AST_FORMAT_AUDIO_MASK,
	.properties = AST_CHAN_TP_WANTSJITTER | AST_CHAN_TP_CREATESJITTER,
	.requester = cb_ast_request,
	.devicestate = cb_ast_devicestate,
	.call = cb_ast_call,
	.hangup = cb_ast_hangup,
	.answer = cb_ast_answer,
	.read = cb_ast_read,
	.write = cb_ast_write,
	.indicate = cb_ast_indicate,
	.fixup = cb_ast_fixup,
	.send_digit_begin = cb_ast_senddigit_begin,
	.send_digit_end = cb_ast_senddigit_end,
	.bridge = ast_rtp_instance_bridge,
};

static struct ast_rtp_glue sccp_rtp_glue = {
	.type = "sccp",
	.get_rtp_info = cb_ast_get_rtp_peer,
	.update_peer = cb_ast_set_rtp_peer,
};

static void mwi_event_cb(const struct ast_event *event, void *data)
{
	int new_msgs = 0;
	int old_msgs = 0;
	struct sccp_device *device = data;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	if (event != NULL)
		new_msgs = ast_event_get_ie_uint(event, AST_EVENT_IE_NEWMSGS);
	else
		ast_app_inboxcount(device->voicemail, &new_msgs, &old_msgs);

	if (new_msgs > 0)
		transmit_lamp_state(device->session, STIMULUS_VOICEMAIL, 0, SCCP_LAMP_ON);

	else if (event != NULL)
		transmit_lamp_state(device->session, STIMULUS_VOICEMAIL, 0, SCCP_LAMP_OFF);
}

static void mwi_subscribe(struct sccp_device *device)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	if (device->voicemail[0] == '\0') {
		ast_log(LOG_DEBUG, "no voicemail set\n");
		return;
	}

	device->mwi_event_sub = ast_event_subscribe(AST_EVENT_MWI, mwi_event_cb, "sccp mwi subsciption", device,
		AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR, device->voicemail,
		AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR, device->default_line->context,
		AST_EVENT_IE_NEWMSGS, AST_EVENT_IE_PLTYPE_EXISTS,
		AST_EVENT_IE_END);
}

static void post_line_register_check(struct sccp_session *session)
{
	int result = 0;
	char exten[AST_MAX_EXTENSION];
	struct sccp_line *line = NULL;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return;
	}

	/* if an entry exist, update the device callfoward status */
	line = session->device->default_line;
	result = ast_db_get("sccp/cfwdall", line->name, exten, sizeof(exten));

	if (result == 0) {
		line->callfwd_id = line->serial_callid++;
		line->callfwd = SCCP_CFWD_INPUTEXTEN;
		ast_copy_string(line->device->exten, exten, sizeof(exten));
		handle_callforward(session, SOFTKEY_CFWDALL);
	}
}

static void post_register_check(struct sccp_session *session)
{
	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return;
	}

	transmit_displaymessage(session, "");

	if (session->device->mwi_event_sub)
		mwi_event_cb(NULL, session->device);
}

static int handle_softkey_template_req_message(struct sccp_session *session)
{
	int ret = 0;
	struct sccp_msg *msg = NULL;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct softkey_template_res_message), SOFTKEY_TEMPLATE_RES_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	msg->data.softkeytemplate.softKeyOffset = htolel(0);
	msg->data.softkeytemplate.softKeyCount = htolel(sizeof(softkey_template_default) / sizeof(struct softkey_template_definition));
	msg->data.softkeytemplate.totalSoftKeyCount = htolel(sizeof(softkey_template_default) / sizeof(struct softkey_template_definition));
	memcpy(msg->data.softkeytemplate.softKeyTemplateDefinition, softkey_template_default, sizeof(softkey_template_default));

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_config_status_req_message(struct sccp_session *session)
{
	int ret = 0;
	struct sccp_msg *msg = NULL;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct config_status_res_message), CONFIG_STATUS_RES_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	memcpy(msg->data.configstatus.deviceName, session->device->name, sizeof(msg->data.configstatus.deviceName));
	msg->data.configstatus.stationUserId = htolel(0);
	msg->data.configstatus.stationInstance = htolel(1);
	msg->data.configstatus.numberLines = htolel(session->device->line_count);
	msg->data.configstatus.numberSpeedDials = htolel(session->device->speeddial_count);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_time_date_req_message(struct sccp_session *session)
{
	int ret = 0;
	struct sccp_msg *msg = NULL;
	time_t now = 0;
	struct tm *cmtime = NULL;
	time_t systime = 0;

	systime = time(0); /* + tz_offset * 3600 */

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct time_date_res_message), DATE_TIME_RES_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	now = time(NULL);
	cmtime = localtime(&now);
	if (cmtime == NULL)
		return -1;

	msg->data.timedate.year = htolel(cmtime->tm_year + 1900);
	msg->data.timedate.month = htolel(cmtime->tm_mon + 1);
	msg->data.timedate.dayOfWeek = htolel(cmtime->tm_wday);
	msg->data.timedate.day = htolel(cmtime->tm_mday);
	msg->data.timedate.hour = htolel(cmtime->tm_hour);
	msg->data.timedate.minute = htolel(cmtime->tm_min);
	msg->data.timedate.seconds = htolel(cmtime->tm_sec);
	msg->data.timedate.milliseconds = htolel(0);
	msg->data.timedate.systemTime = htolel(systime);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_button_template_req_message(struct sccp_session *session)
{
	int ret = 0;
	struct sccp_msg *msg = NULL;
	struct button_definition_template btl[42] = {0};
	int button_count = 0;
	uint32_t line_instance = 1;
	struct sccp_line *line_itr = NULL;
	int i = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct button_template_res_message), BUTTON_TEMPLATE_RES_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	ret = device_get_button_template(session->device, btl);
	if (ret == -1)
		return -1;

	for (i = 0; i < 42; i++) {
		switch (btl[i].buttonDefinition) {
			case BT_CUST_LINESPEEDDIAL:

				msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
				msg->data.buttontemplate.definition[i].lineInstance = htolel(0);

				AST_RWLIST_RDLOCK(&session->device->lines);
				AST_RWLIST_TRAVERSE(&session->device->lines, line_itr, list_per_device) {
					if (line_itr->instance == line_instance) {
						msg->data.buttontemplate.definition[i].buttonDefinition = BT_LINE;
						msg->data.buttontemplate.definition[i].lineInstance = htolel(line_instance);

						line_instance++;
						button_count++;
					}
				}
				AST_RWLIST_UNLOCK(&session->device->lines);

				break;

			case BT_NONE:
			default:
				msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
				msg->data.buttontemplate.definition[i].lineInstance = htolel(0);
				break;
		}
	}

	msg->data.buttontemplate.buttonOffset = htolel(0);
	msg->data.buttontemplate.buttonCount = htolel(button_count);
	msg->data.buttontemplate.totalButtonCount = htolel(button_count);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;
	
	return 0;
}

static int handle_keep_alive_message(struct sccp_session *session)
{
	int ret = 0;
	struct sccp_msg *msg = NULL;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(0, KEEP_ALIVE_ACK_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int register_device(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_device *device_itr = NULL;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	AST_RWLIST_RDLOCK(&sccp_config->list_device);
	AST_RWLIST_TRAVERSE(&sccp_config->list_device, device_itr, list) {
		if (!strcasecmp(device_itr->name, msg->data.reg.name))
			break;
	}
	AST_RWLIST_UNLOCK(&sccp_config->list_device);

	if (device_itr == NULL) {

		ast_log(LOG_WARNING, "Device is not configured [%s]\n", msg->data.reg.name);
		ret = -1;

	} else if (device_itr->line_count == 0) {

		ast_log(LOG_WARNING, "Device [%s] has no valid line\n", device_itr->name);
		ret = -1;

	} else if (device_itr->registered == DEVICE_REGISTERED_TRUE) {

		ast_log(LOG_WARNING, "Device already registered [%s]\n", device_itr->name);
		ret = -1;

	} else {

		struct sockaddr_in localip;
		socklen_t slen;

		getsockname(session->sockfd, (struct sockaddr *)&localip, &slen);

		ast_log(LOG_NOTICE, "Device found [%s]\n", device_itr->name);

		device_prepare(device_itr);
		device_register(device_itr,
				letohl(msg->data.reg.protoVersion),
				letohl(msg->data.reg.type),
				session,
				localip);

		session->device = device_itr;
		mwi_subscribe(device_itr);
		ast_devstate_changed(AST_DEVICE_NOT_INUSE, "SCCP/%s", device_itr->default_line->name);
		ret = 1;
	}

	if (ret == 0)
		ast_log(LOG_WARNING, "Device is not configured [%s]\n", msg->data.reg.name);

	return ret;
}

static struct sccp_subchannel *sccp_new_subchannel(struct sccp_line *line)
{
	struct sccp_subchannel *subchan = NULL;

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return NULL;
	}

	subchan = ast_calloc(1, sizeof(struct sccp_subchannel));
	if (subchan == NULL) {
		ast_log(LOG_ERROR, "subchannel allocation failed\n");
		return NULL;
	}

	/* initialise subchannel and add it to the list */
	subchan->state = SCCP_OFFHOOK;
	subchan->on_hold = 0;
	subchan->line = line;
	subchan->id = line->serial_callid++;
	subchan->channel = NULL;
	subchan->related = NULL;

	AST_LIST_INSERT_HEAD(&line->subchans, subchan, list);

	return subchan;
}

static struct ast_channel *sccp_new_channel(struct sccp_subchannel *subchan, const char *linkedid)
{
	struct ast_channel *channel = NULL;
	struct ast_variable *var_itr = NULL;
	int audio_format = 0;
	char valuebuf[1024];
	char dbgsub_buf[256];

	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return NULL;
	}

	channel = ast_channel_alloc(	1,				/* needqueue */
					AST_STATE_DOWN,			/* state */
					subchan->line->cid_num,		/* cid_num */
					subchan->line->cid_name,	/* cid_name */
					"code",				/* acctcode */
					subchan->line->device->exten,	/* exten */
					subchan->line->context,		/* context */
					linkedid,			/* linked ID */
					0,				/* amaflag */
					"sccp/%s@%s-%d",		/* format */
					subchan->line->name,		/* name */
					subchan->line->device->name,	/* name */
					1);				/* callnums */

	if (channel == NULL) {
		ast_log(LOG_ERROR, "channel allocation failed\n");
		return NULL;
	}

	channel->tech = &sccp_tech;
	channel->tech_pvt = subchan;
	subchan->channel = channel;

	/* if there is no codec, set a default one */
	if (subchan->line->device->codecs == -1)
		channel->nativeformats = SCCP_CODEC_G711_ULAW;
	else
		channel->nativeformats = subchan->line->device->codecs;

	for (var_itr = subchan->line->chanvars; var_itr != NULL; var_itr = var_itr->next) {

		ast_get_encoded_str(var_itr->value, valuebuf, sizeof(valuebuf));
		ast_log(LOG_DEBUG, "var_name: %s  var_value: %s valuebuf: %s\n",
					var_itr->name, var_itr->value, valuebuf);

		pbx_builtin_setvar_helper(channel, var_itr->name,
			ast_get_encoded_str(var_itr->value, valuebuf, sizeof(valuebuf)));
	}

	audio_format = ast_best_codec(channel->nativeformats);

	channel->writeformat = audio_format;
	channel->rawwriteformat = audio_format;
	channel->readformat = audio_format;
	channel->rawreadformat = audio_format;

	ast_log(LOG_DEBUG, "codec %s %s\n",
		ast_getformatname_multiple(dbgsub_buf, sizeof(dbgsub_buf), channel->nativeformats),
		ast_getformatname(audio_format));

	if (subchan->line->language[0] != '\0')
		ast_string_field_set(channel, language, subchan->line->language);

	if (subchan->line->callfwd == SCCP_CFWD_ACTIVE)
		ast_string_field_set(channel, call_forward, subchan->line->callfwd_exten);

	ast_module_ref(ast_module_info->self);

	return channel;
}

static enum ast_rtp_glue_result cb_ast_get_rtp_peer(struct ast_channel *channel, struct ast_rtp_instance **instance)
{
	struct sccp_subchannel *subchan = NULL;

	if (channel == NULL) {
		ast_log(LOG_DEBUG, "channel is NULL\n");
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	if (instance == NULL) {
		ast_log(LOG_DEBUG, "instance is NULL\n");
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	subchan = channel->tech_pvt;
	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	if (subchan->rtp == NULL) {
		ast_log(LOG_DEBUG, "rtp is NULL\n");
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	ao2_ref(subchan->rtp, +1);
	*instance = subchan->rtp;

	if (sccp_config->directmedia)
		return AST_RTP_GLUE_RESULT_REMOTE;

	return AST_RTP_GLUE_RESULT_LOCAL;
}

static int cb_ast_set_rtp_peer(struct ast_channel *channel,
				struct ast_rtp_instance *rtp,
				struct ast_rtp_instance *vrtp,
				struct ast_rtp_instance *trtp,
				format_t codecs,
				int nat_active)
{
	struct sccp_line *line = NULL;
	struct sccp_subchannel *subchan = NULL;
	struct sockaddr_in endpoint = {0,};
	struct ast_sockaddr endpoint_tmp;

	struct sockaddr_in local = {0,};
	struct ast_sockaddr local_tmp;

	struct ast_format_list fmt = {0};

	subchan = channel->tech_pvt;
	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return -1;
	}

	if (subchan->on_hold) {
		return 0;
	}

	line = subchan->line;
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	if (sccp_config->directmedia && rtp) {

		fmt = ast_codec_pref_getsize(&line->codec_pref, ast_best_codec(line->device->codecs));

		ast_rtp_instance_get_remote_address(rtp, &endpoint_tmp);
		ast_sockaddr_to_sin(&endpoint_tmp, &endpoint);
		ast_log(LOG_DEBUG, "endpoint %s:%d\n", ast_inet_ntoa(endpoint.sin_addr), ntohs(endpoint.sin_port));

		if (endpoint.sin_addr.s_addr != 0) {
			transmit_stop_media_transmission(line, subchan->id);
			transmit_start_media_transmission(line, subchan->id, endpoint, fmt);
		}
		else {
			struct sockaddr_in local = {0};
			struct ast_sockaddr local_tmp;

			ast_rtp_instance_get_local_address(line->active_subchan->rtp, &local_tmp);
			ast_sockaddr_to_sin(&local_tmp, &local);

			if (local.sin_addr.s_addr == 0)
				local.sin_addr.s_addr = line->device->localip.sin_addr.s_addr;

			ast_log(LOG_DEBUG, "local address %s:%d\n", ast_inet_ntoa(local.sin_addr), ntohs(local.sin_port));

			transmit_stop_media_transmission(line, subchan->id);
			transmit_start_media_transmission(line, subchan->id, local, fmt);
		}
	}

	return 0;
}

static int start_rtp(struct sccp_subchannel *subchan)
{
	struct ast_codec_pref default_prefs = {0};
	struct sccp_session *session = NULL;
	struct ast_sockaddr bindaddr_tmp;
	struct ast_sockaddr remote_tmp;

	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return -1;
	}

	ast_log(LOG_DEBUG, "start rtp\n");

	session = subchan->line->device->session;

	ast_sockaddr_from_sin(&bindaddr_tmp, (struct sockaddr_in *)sccp_srv.res->ai_addr);
	subchan->rtp = ast_rtp_instance_new("asterisk", sched, &bindaddr_tmp, NULL);

	if (subchan->rtp) {

		ast_rtp_instance_set_prop(subchan->rtp, AST_RTP_PROPERTY_RTCP, 1);

		ast_channel_set_fd(subchan->channel, 0, ast_rtp_instance_fd(subchan->rtp, 0));
		ast_channel_set_fd(subchan->channel, 1, ast_rtp_instance_fd(subchan->rtp, 1));

		ast_rtp_instance_set_qos(subchan->rtp, 0, 0, "sccp rtp");
		ast_rtp_instance_set_prop(subchan->rtp, AST_RTP_PROPERTY_NAT, 0);

		ast_rtp_codecs_packetization_set(ast_rtp_instance_get_codecs(subchan->rtp),
						subchan->rtp, &subchan->line->codec_pref);

		if (subchan->line->device->early_remote) {
			ast_sockaddr_from_sin(&remote_tmp, &subchan->line->device->remote);
			ast_rtp_instance_set_remote_address(subchan->rtp, &remote_tmp);
			subchan->line->device->early_remote = 0;

		} else {
			transmit_connect(subchan->line, subchan->id);
		}
	}

	return 0;
}

static int sccp_start_the_call(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = NULL;
	struct sccp_line *line = NULL;

	if (channel == NULL) {
		ast_log(LOG_DEBUG, "channel is NULL\n");
		return -1;
	}

	subchan = channel->tech_pvt;
	line = subchan->line;

	set_line_state(line, SCCP_RINGOUT);
	ast_setstate(channel, AST_STATE_RING);

	transmit_callstate(line->device->session, line->instance, SCCP_PROGRESS, subchan->id);
	transmit_tone(line->device->session, SCCP_TONE_ALERT, line->instance, subchan->id);
	transmit_callinfo(line->device->session, "", "", line->device->exten, line->device->exten, line->instance, subchan->id, 2);
	transmit_dialed_number(line->device->session, line->device->exten, line->instance, subchan->id);

	ast_set_callerid(channel,
			line->cid_num,
			line->cid_name,
			NULL);

	ast_party_number_free(&channel->connected.id.number);
	ast_party_number_init(&channel->connected.id.number);
	channel->connected.id.number.valid = 1;
	channel->connected.id.number.str = ast_strdup(channel->exten);
	ast_party_name_free(&channel->connected.id.name);
	ast_party_name_init(&channel->connected.id.name);

	ast_pbx_start(channel);

	return 0;
}

static void *sccp_lookup_exten(void *data)
{
	struct ast_channel *channel = NULL;
	struct sccp_subchannel *subchan = NULL;
	struct sccp_line *line = NULL;
	size_t len = 0, next_len = 0;
	int call_now = 0;
	int timeout = sccp_config->dialtimeout * 2;

	if (data == NULL) {
		ast_log(LOG_DEBUG, "data is NULL\n");
		return NULL;
	}

	subchan = (struct sccp_subchannel *)data;
	line = subchan->line;
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return NULL;
	}

	len = strlen(line->device->exten);
	while (line->device->registered == DEVICE_REGISTERED_TRUE &&
		line->state == SCCP_OFFHOOK && len < AST_MAX_EXTENSION-1) {

		/* when pound key is pressed, call the extension without further waiting */
		if (len > 0 && line->device->exten[len-1] == '#') {
			line->device->exten[len-1] = '\0';
			call_now = 1;
		}

		if (timeout == 0)
			call_now = 1;

		if (call_now) {
				channel = sccp_new_channel(subchan, NULL);
				if (channel == NULL) {
					ast_log(LOG_ERROR, "channel is NULL\n");
					return NULL;
				}

				sccp_start_the_call(channel);
				memcpy(line->device->last_exten, line->device->exten, AST_MAX_EXTENSION);
				line->device->exten[0] = '\0';
				return NULL;
		}

		usleep(500000);

		next_len = strlen(line->device->exten);
		if (len == next_len) {
			if (len == 0)
				len = next_len;
			else
				timeout--;
		} else {
			timeout = sccp_config->dialtimeout * 2;
			len = next_len;
		}
	}

	return NULL;
}

static int do_newcall(uint32_t line_instance, uint32_t subchan_id, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_device *device = NULL;
	struct sccp_line *line = NULL;
	struct sccp_subchannel *subchan = NULL;

	ast_log(LOG_DEBUG, "line_instance(%d) subchan_id(%d)\n", line_instance, subchan_id);

	if (session == NULL) {
		ast_log(LOG_ERROR, "session is NULL\n");
		return -1;
	}

	device = session->device;
	if (device == NULL) {
		ast_log(LOG_ERROR, "device is NULL\n");
		return -1;
	}

	line = device_get_active_line(device);
	if (line == NULL) {
		ast_log(LOG_ERROR, "line is NULL\n");
		return -1;
	}

	subchan = sccp_new_subchannel(line);
	if (subchan == NULL) {
		ast_log(LOG_ERROR, "subchan is NULL\n");
		return -1;
	}

	/* If a subchannel is already active, put it on hold */
	if (line->active_subchan != NULL) {
		handle_softkey_hold(line_instance, line->active_subchan->id, session);
	}

	/* Now, set the new call instance as active */
	line_select_subchan(line, subchan);

	set_line_state(line, SCCP_OFFHOOK);

	ret = transmit_lamp_state(session, 1, line->instance, SCCP_LAMP_ON);
	if (ret == -1)
		return -1;

	ret = transmit_callstate(session, line->instance, SCCP_OFFHOOK, line->active_subchan->id);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, line->instance, line->active_subchan->id, KEYDEF_OFFHOOK);
	if (ret == -1)
		return -1;

	ret = transmit_tone(session, SCCP_TONE_DIAL, line->instance, line->active_subchan->id);
	if (ret == -1)
		return -1;

	if (ast_pthread_create(&device->lookup_thread, NULL, sccp_lookup_exten, subchan)) {
		ast_log(LOG_WARNING, "Unable to create switch thread: %s\n", strerror(errno));
	} else {
		device->lookup = 1;
	}

	ast_devstate_changed(AST_DEVICE_INUSE, "SCCP/%s", line->name);

	return 0;
}

static int do_answer(uint32_t line_instance, uint32_t subchan_id, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_line *line = NULL;
	struct sccp_subchannel *subchan = NULL;

	ast_log(LOG_DEBUG, "line_instance(%d) subchan_id(%d)\n", line_instance, subchan_id);

	if (session == NULL) {
		ast_log(LOG_ERROR, "session is NULL\n");
		return -1;
	}

	line = device_get_line(session->device, line_instance);
	if (line == NULL) {
		ast_log(LOG_ERROR, "line is NULL\n");
		return 0;
	}

	subchan = line_get_subchan(line, subchan_id);
	if (subchan == NULL) {
		ast_log(LOG_ERROR, "subchan is NULL\n");
		return 0;
	}

	/* If a subchannel is already active, put it on hold */
	if (line->active_subchan != NULL) {
		handle_softkey_hold(line_instance, line->active_subchan->id, session);
	}

	/* Now, set the newly answered subchannel as active */
	line_select_subchan(line, subchan);

	if (subchan->channel == NULL) {
		ast_log(LOG_WARNING, "channel is NULL\n");
		return 0;
	}

	ast_queue_control(subchan->channel, AST_CONTROL_ANSWER);

	ret = transmit_ringer_mode(session, SCCP_RING_OFF);
	if (ret == -1)
		return -1;

	ret = transmit_callstate(session, line->instance, SCCP_OFFHOOK, subchan->id);
	if (ret == -1)
		return -1;

	ret = transmit_stop_tone(session, line->instance, subchan->id);
	if (ret == -1)
		return -1;

	ret = transmit_tone(session, SCCP_TONE_NONE, line->instance, subchan->id);
	if (ret == -1)
		return -1;

	ret = transmit_callstate(session, line->instance, SCCP_CONNECTED, subchan->id);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, line->instance, subchan->id, KEYDEF_CONNECTED);
	if (ret == -1)
		return -1;

	start_rtp(subchan);

	set_line_state(line, SCCP_CONNECTED);
	subchan_set_state(subchan, SCCP_CONNECTED);

	ast_devstate_changed(AST_DEVICE_INUSE, "SCCP/%s", line->name);

	return 0;
}

static int handle_offhook_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_line *line = NULL;
	struct sccp_device *device = NULL;
	struct sccp_subchannel *subchan = NULL;
	struct ast_channel *channel = NULL;

	uint32_t line_instance = 0;
	uint32_t subchan_id = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	device = session->device;
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	line = device_get_active_line(device);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	if (session->device->protoVersion == 11) {

		/* Newest protocols provide these informations */
		line_instance = msg->data.offhook.lineInstance;
		subchan_id = msg->data.offhook.callInstance;

		if (line_instance == 0) { /* no active line */
			do_newcall(line_instance, subchan_id, session);
		}
		else {
			do_answer(line_instance, subchan_id, session);
		}
	}
	else {
		/* With older protocols, we manually get the line and the subchannel */
		line = device_get_active_line(session->device);
		if (line == NULL) {
			ast_log(LOG_DEBUG, "line is NULL\n");
			return -1;
		}

		line_instance = line->instance;

		subchan = line_get_next_ringin_subchan(line);
		if (subchan) {
			ast_log(LOG_DEBUG, "subchan exist\n");
			subchan_id = subchan->id;
			do_answer(line_instance, subchan_id, session);
		}
		else {
			ast_log(LOG_DEBUG, "line->active_subchan (%p)\n", line->active_subchan);

			if (line->active_subchan == NULL) {
				ast_log(LOG_DEBUG, "subchan do not exist\n");
				do_newcall(line_instance, 0, session);
			}
		}
	}

	return 0;
}

static int do_clear_subchannel(struct sccp_subchannel *subchan)
{
	int ret = 0;
	struct sccp_line *line = NULL;
	struct sccp_session *session = NULL;

	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return -1;
	}

	line = subchan->line;
	session = line->device->session;

	if (subchan->rtp) {

		ret = transmit_close_receive_channel(line, subchan->id);
		if (ret == -1)
			return -1;

		ret = transmit_stop_media_transmission(line, subchan->id);
		if (ret == -1)
			return -1;

		ast_rtp_instance_stop(subchan->rtp);
		ast_rtp_instance_destroy(subchan->rtp);
		subchan->rtp = NULL;
	}

	ret = transmit_ringer_mode(session, SCCP_RING_OFF);
	if (ret == -1)
		return -1;

	ret = transmit_speaker_mode(line->device->session, SCCP_SPEAKEROFF);
	if (ret == -1)
		return -1;

	ret = transmit_callstate(session, line->instance, SCCP_ONHOOK, subchan->id);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, line->instance, subchan->id, KEYDEF_ONHOOK);
	if (ret == -1)
		return -1;

	ret = transmit_stop_tone(session, line->instance, subchan->id);
	if (ret == -1)
		return -1;

	ret = transmit_tone(session, SCCP_TONE_NONE, line->instance, subchan->id);
	if (ret == -1)
		return -1;

	AST_LIST_REMOVE(&line->subchans, subchan, list);

	if (subchan->related) {
		subchan->related->related = NULL;
	}

	if (subchan == line->active_subchan)
		subchan->line->active_subchan = NULL;

	ast_free(subchan);

	ast_devstate_changed(AST_DEVICE_NOT_INUSE, "SCCP/%s", line->name);
	set_line_state(line, SCCP_ONHOOK);

	return 0;
}

static int do_hangup(uint32_t line_instance, uint32_t subchan_id, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_line *line = NULL;
	struct sccp_subchannel *subchan = NULL;

	ast_log(LOG_DEBUG, "line_instance(%d) subchan_id(%d)\n", line_instance, subchan_id);

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	line = device_get_line(session->device, line_instance);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return 0;
	}

	ast_devstate_changed(AST_DEVICE_NOT_INUSE, "SCCP/%s", line->name);

	 /* this will cause the sccp_lookup_exten thread to terminate*/
	set_line_state(line, SCCP_ONHOOK);

	/* wait for lookup thread to terminate */
	if (session->device->lookup == 1) {
		pthread_join(session->device->lookup_thread, NULL);
		session->device->lookup = 0;
		line->device->exten[0] = '\0';
	}

	subchan = line_get_subchan(line, subchan_id);
	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return 0;
	}

	if (subchan->channel)
		ast_queue_hangup(subchan->channel);
	else
		do_clear_subchannel(subchan);

	return 0;
}

static int handle_onhook_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_line *line = NULL;
	struct sccp_subchannel *subchan = NULL;
	uint32_t line_instance = 0;
	uint32_t subchan_id = 0;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	if (session->device->protoVersion == 11) {
		/* Newest protocols provide these informations */

		line_instance = msg->data.offhook.lineInstance;
		subchan_id = msg->data.offhook.callInstance;

		ast_log(LOG_DEBUG, "line_instance: %d\n", line_instance);
		ast_log(LOG_DEBUG, "subchan_id %d\n", subchan_id);

		do_hangup(line_instance, subchan_id, session);
	}
	else {
		/* With older protocols, we manually get the line and the subchannel */
		line = device_get_active_line(session->device);
		if (line == NULL) {
			ast_log(LOG_DEBUG, "line is NULL\n");
			return -1;
		}

		subchan = line->active_subchan;
		if (subchan) {
			if (!subchan->on_hold) {
				line_instance = line->instance;
				subchan_id = subchan->id;
				do_hangup(line_instance, subchan_id, session);
			}
		}
		else {
			ast_log(LOG_DEBUG, "subchan is NULL\n");
		}
	}

	return 0;
}

static int handle_softkey_hold(uint32_t line_instance, uint32_t subchan_id, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_line *line = NULL;

	ast_log(LOG_DEBUG, "handle_softkey_hold: line_instance(%i) subchan_id(%i)\n", line_instance, subchan_id);

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	line = device_get_line(session->device, line_instance);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	/* put on hold */
	if (line->active_subchan && line->active_subchan->channel) {
		ast_queue_control(line->active_subchan->channel, AST_CONTROL_HOLD);
		ast_channel_set_fd(line->active_subchan->channel, 0, -1);
		ast_channel_set_fd(line->active_subchan->channel, 1, -1);
	}

	if (line->active_subchan && line->active_subchan->rtp) {
		ast_rtp_instance_stop(line->active_subchan->rtp);
		ast_rtp_instance_destroy(line->active_subchan->rtp);
		line->active_subchan->rtp = NULL;
	}

	ret = transmit_callstate(session, line_instance, SCCP_HOLD, subchan_id);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, line_instance, subchan_id, KEYDEF_ONHOLD);
	if (ret == -1)
		return -1;

	/* close our speaker */
	ret = transmit_speaker_mode(session, SCCP_SPEAKEROFF);
	if (ret == -1)
		return -1;

	/* stop audio stream */
	ret = transmit_close_receive_channel(line, subchan_id);
	if (ret == -1)
		return -1;

	ret = transmit_stop_media_transmission(line, subchan_id);
	if (ret == -1)
		return -1;

	subchan_set_on_hold(line, subchan_id);

	if (line->active_subchan && line->active_subchan->id == subchan_id)
		line->active_subchan = NULL;

	return 0;
}

static int handle_softkey_resume(uint32_t line_instance, uint32_t subchan_id, struct sccp_session *session)
{
	struct sccp_line *line = NULL;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	line = device_get_line(session->device, line_instance);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	if (line->active_subchan) {
		/* if another channel is already active */
		if (line->active_subchan->id != subchan_id) {
			handle_softkey_hold(line->instance, line->active_subchan->id, session);
		}
	}

	line_select_subchan_id(line, subchan_id);
	set_line_state(line, SCCP_CONNECTED); 

	/* put on connected */
	transmit_callstate(session, line_instance, SCCP_CONNECTED, subchan_id);
	transmit_selectsoftkeys(session, line_instance, subchan_id, KEYDEF_CONNECTED);

	/* open our speaker */
	transmit_speaker_mode(session, SCCP_SPEAKERON);

	/* start audio stream */
	start_rtp(line->active_subchan);
	//transmit_connect(line, line->active_subchan->id);

	subchan_unset_on_hold(line, subchan_id);
	return 0;
}

static int handle_softkey_transfer(uint32_t line_instance, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_subchannel *subchan, *xfer_subchan;
	struct ast_channel *channel;
	struct sccp_line *line;

	ast_log(LOG_DEBUG, "handle_softkey_transfer: line_instance(%i)\n", line_instance);

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	line = device_get_line(session->device, line_instance);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line instance [%i] doesn't exist\n", line_instance);
		return -1;
	}

	if (line->active_subchan == NULL) {
		ast_log(LOG_DEBUG, "line instance [%i] has no active subchannel\n", line_instance);
		return -1;
	}

	/* first time we press transfer */
	if (line->active_subchan->related == NULL) {

		/* put on hold */
		ret = transmit_callstate(session, line_instance, SCCP_HOLD, line->active_subchan->id);
		if (ret == -1)
			return -1;

		ret = transmit_selectsoftkeys(session, line_instance, line->active_subchan->id, KEYDEF_ONHOLD);
		if (ret == -1)
			return -1;

		/* close our speaker */
		ret = transmit_speaker_mode(session, SCCP_SPEAKEROFF);
		if (ret == -1)
			return -1;

		/* stop audio stream */
		ret = transmit_close_receive_channel(line, line->active_subchan->id);
		if (ret == -1)
			return -1;

		ret = transmit_stop_media_transmission(line, line->active_subchan->id);
		if (ret == -1)
			return -1;

		ast_queue_control(line->active_subchan->channel, AST_CONTROL_HOLD);

		/* spawn a new subchannel instance and mark both as related */
		subchan = sccp_new_subchannel(line);
		xfer_subchan = line->active_subchan;

		line_select_subchan(line, subchan);

		xfer_subchan->related = line->active_subchan;
		line->active_subchan->related = xfer_subchan;

		set_line_state(line, SCCP_OFFHOOK);

		ret = transmit_callstate(session, line_instance, SCCP_OFFHOOK, line->active_subchan->id);
		if (ret == -1)
			return -1;

		ret = transmit_selectsoftkeys(session, line_instance, line->active_subchan->id, KEYDEF_CONNINTRANSFER);
		if (ret == -1)
			return -1;

		/* start dial tone */
		ret = transmit_tone(session, SCCP_TONE_DIAL, line->instance, line->active_subchan->id);
		if (ret == -1)
			return -1;

		if (ast_pthread_create(&line->device->lookup_thread, NULL, sccp_lookup_exten, subchan)) {
			ast_log(LOG_WARNING, "Unable to create switch thread: %s\n", strerror(errno));
		} else {
			line->device->lookup = 1;
		}

	} else if (line->active_subchan->channel) {

		ast_log(LOG_DEBUG, "line->active_subchan->channel->_state: %d\n",
					line->active_subchan->channel->_state);

		ast_log(LOG_DEBUG, "line->active_subchan->related->channel->_state: %d\n",
					line->active_subchan->related->channel->_state);

		if (line->active_subchan->channel->_state == AST_STATE_DOWN
			|| line->active_subchan->related->channel->_state == AST_STATE_DOWN) {
			ast_log(LOG_DEBUG, "channel state AST_STATE_DOWN\n");
			return 0;
		}

		ast_queue_control(line->active_subchan->related->channel, AST_CONTROL_UNHOLD);

		if (ast_bridged_channel(line->active_subchan->channel)) {
			ast_channel_masquerade(line->active_subchan->related->channel, ast_bridged_channel(line->active_subchan->channel));
		}
		else if (ast_bridged_channel(line->active_subchan->related->channel)) {
			ast_channel_masquerade(line->active_subchan->channel, ast_bridged_channel(line->active_subchan->related->channel));
			ast_queue_hangup(line->active_subchan->related->channel);
		} else {
			ast_queue_hangup(line->active_subchan->channel);
			return 0;
		}
	}

	return 0;
}

static int handle_callforward(struct sccp_session *session, uint32_t softkey)
{
	int ret = 0;
	struct sccp_line *line = NULL;
	int line_instance = 1; /* Callforward is statically linked to line 1 */
	char info_fwd[24];

	if (session == NULL)
		return -1;

	line = device_get_line(session->device, line_instance);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	switch (line->callfwd) {
	case SCCP_CFWD_UNACTIVE:

		line->callfwd_id = line->serial_callid++;

		ret = transmit_callstate(session, line->instance, SCCP_OFFHOOK, line->callfwd_id);
		if (ret == -1)
			return -1;

		ret = transmit_selectsoftkeys(session, line->instance, line->callfwd_id, KEYDEF_CALLFWD);
		if (ret == -1)
			return -1;

		ret = transmit_speaker_mode(session, SCCP_SPEAKERON);

		set_line_state(line, SCCP_OFFHOOK);
		line->callfwd = SCCP_CFWD_INPUTEXTEN;

		break;

	case SCCP_CFWD_INPUTEXTEN:

		ret = transmit_callstate(session, line->instance, SCCP_ONHOOK, line->callfwd_id);
		if (ret == -1)
			return -1;

		ret = transmit_selectsoftkeys(session, line->instance, line->callfwd_id, KEYDEF_ONHOOK);
		if (ret == -1)
			return -1;

		set_line_state(line, SCCP_ONHOOK);

		if (softkey == SOFTKEY_CANCEL || line->device->exten[0] == '\0') {
			line->callfwd = SCCP_CFWD_UNACTIVE;

		} else if (softkey == SOFTKEY_CFWDALL) {

			ast_copy_string(line->callfwd_exten, line->device->exten, sizeof(line->callfwd_exten));
			snprintf(info_fwd, sizeof(info_fwd), "\200\5: %s", line->callfwd_exten);

			ret = transmit_forward_status_message(session, line->instance, line->callfwd_exten, 1);
			if (ret == -1)
				return -1;

			ret = transmit_displaymessage(session, info_fwd);
			if (ret == -1)
				return -1;

			line->callfwd = SCCP_CFWD_ACTIVE;

			ast_db_put("sccp/cfwdall", line->name, line->callfwd_exten);
		}

		ret = transmit_speaker_mode(session, SCCP_SPEAKEROFF);
		line->device->exten[0] = '\0';

		break;

	case SCCP_CFWD_ACTIVE:

		ret = transmit_forward_status_message(session, line->instance, "", 0);
		if (ret == -1)
			return -1;

		ret = transmit_displaymessage(session, "");
		if (ret == -1)
			return -1;

		line->callfwd = SCCP_CFWD_UNACTIVE;

		ast_db_del("sccp/cfwdall", line->name);

		break;
	}

	return 0;
}

static int handle_softkey_event_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	if (session->device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	ast_log(LOG_DEBUG, "softKeyEvent: %d\n", letohl(msg->data.softkeyevent.softKeyEvent));
	ast_log(LOG_DEBUG, "instance: %d\n", msg->data.softkeyevent.lineInstance);
	ast_log(LOG_DEBUG, "callid: %d\n", msg->data.softkeyevent.callInstance);

	switch (letohl(msg->data.softkeyevent.softKeyEvent)) {
	case SOFTKEY_NONE:
		break;

	case SOFTKEY_REDIAL:
		if (strlen(session->device->last_exten) > 0) {
			ret = transmit_speaker_mode(session, SCCP_SPEAKERON);
			if (ret == -1)
				return -1;

			ret = do_newcall(msg->data.softkeyevent.lineInstance,
					msg->data.softkeyevent.callInstance,
					session);
			if (ret == -1)
				return -1;
			memcpy(session->device->exten, session->device->last_exten, AST_MAX_EXTENSION);
		}
		break;

	case SOFTKEY_NEWCALL:
		ret = transmit_speaker_mode(session, SCCP_SPEAKERON);
		if (ret == -1)
			return -1;

		ret = do_newcall(msg->data.softkeyevent.lineInstance,
				msg->data.softkeyevent.callInstance,
				session);
		if (ret == -1)
			return -1;
		break;

	case SOFTKEY_HOLD:

		handle_softkey_hold(msg->data.softkeyevent.lineInstance,
					msg->data.softkeyevent.callInstance,
					 session);
		break;

	case SOFTKEY_TRNSFER:
		ret = handle_softkey_transfer(msg->data.softkeyevent.lineInstance, session);
		if (ret == -1)
			return -1;
		break;

	case SOFTKEY_CFWDALL:
		ret = handle_callforward(session, SOFTKEY_CFWDALL);
		if (ret == -1)
			return -1;
		break;

	case SOFTKEY_CANCEL:
		ret = handle_callforward(session, SOFTKEY_CANCEL);
		if (ret == -1)
			return -1;
		break;

	case SOFTKEY_CFWDBUSY:
		break;

	case SOFTKEY_CFWDNOANSWER:
		break;

	case SOFTKEY_ENDCALL:

		ret = do_hangup(msg->data.softkeyevent.lineInstance,
				msg->data.softkeyevent.callInstance,
				session);
		if (ret == -1)
			return -1;
		break;

	case SOFTKEY_RESUME:
		ret = handle_softkey_resume(msg->data.softkeyevent.lineInstance,
					msg->data.softkeyevent.callInstance,
					session);
		if (ret == -1)
			return -1;
		break;

	case SOFTKEY_ANSWER:

		ret = transmit_speaker_mode(session, SCCP_SPEAKERON);
		if (ret == -1)
			return -1;

		ret = do_answer(msg->data.softkeyevent.lineInstance,
				msg->data.softkeyevent.callInstance,
				session);
		if (ret == -1)
			return -1;

		break;

	case SOFTKEY_INFO:
		break;

	case SOFTKEY_CONFRN:
		break;

	case SOFTKEY_PARK:
		break;

	case SOFTKEY_JOIN:
		break;

	case SOFTKEY_MEETME:
		break;

	case SOFTKEY_PICKUP:
		break;

	case SOFTKEY_GPICKUP:
		break;

	default:
		break;
	}

	return 0;
}

static int handle_softkey_set_req_message(struct sccp_session *session)
{
	int ret = 0;
	const struct softkey_definitions *softkeymode = softkey_default_definitions;
	struct sccp_msg *msg = NULL;
	int keyset_count = 0;
	int i = 0;
	int j = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct softkey_set_res_message), SOFTKEY_SET_RES_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	keyset_count = sizeof(softkey_default_definitions) / sizeof(struct softkey_definitions);

        msg->data.softkeysets.softKeySetOffset = htolel(0);
        msg->data.softkeysets.softKeySetCount = htolel(keyset_count);
        msg->data.softkeysets.totalSoftKeySetCount = htolel(keyset_count);

	for (i = 0; i < keyset_count; i++) {

		for (j = 0; j < softkeymode->count; j++) {
			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyTemplateIndex[j]
				= htolel(softkeymode->defaults[j]);

			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyInfoIndex[j]
				= htolel(softkeymode->defaults[j]);
		}
		softkeymode++;
	}

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, 0, 0, KEYDEF_ONHOOK);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_forward_status_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	/* Do nothing here, not all phone query the forward status,
		instead handle it globally in the post_line_register_check() */
	return 0;
}

int codec_ast2sccp(format_t astcodec)
{
        switch (astcodec) {
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

static format_t codec_sccp2ast(enum sccp_codecs sccp_codec)
{
	switch (sccp_codec) {
	case SCCP_CODEC_G711_ALAW:
		return AST_FORMAT_ALAW;
	case SCCP_CODEC_G711_ULAW:
		return AST_FORMAT_ULAW;
	case SCCP_CODEC_G723_1:
		return AST_FORMAT_G723_1;
	case SCCP_CODEC_G729A:
		return AST_FORMAT_G729A;
	case SCCP_CODEC_H261:
		return AST_FORMAT_H261;
	case SCCP_CODEC_H263:
		return AST_FORMAT_H263;
	default:
		return -1;
	}
}

char *utf8_to_iso88591(char *to_convert)
{
	iconv_t cd;

	size_t len;
	size_t outbytesleft;
	size_t inbytesleft;
	size_t iconv_value;

	char *outbuf = NULL;
	char *inbuf = NULL;

	char *outbufptr = NULL;
	char *inbufptr = NULL;

	if (to_convert == NULL) {
		ast_log(LOG_DEBUG, "to_convert is NULL\n");
		return NULL;
	}

	cd = iconv_open("ISO-8859-1", "UTF-8");

	len = strlen(to_convert);

	outbuf = ast_calloc(1, len);
	outbufptr = outbuf;

	outbytesleft = len;
	inbytesleft = len;

	inbuf = ast_strdup(to_convert);
	inbufptr = inbuf;

	iconv_value = iconv(cd,
			&inbuf,
			&inbytesleft,
			&outbuf,
			&outbytesleft);

	if (iconv_value == (size_t)-1) {

		switch (errno) {
		case EILSEQ:
			ast_log(LOG_ERROR, "Invalid multibyte sequence\n");
			break;
		case EINVAL:
			ast_log(LOG_ERROR, "Incomplete multibyte sequebec\n");
			break;
		case E2BIG:
			ast_log(LOG_ERROR, "Not enough space in outbuf\n");
			break;
		}

		free(outbufptr);
		outbufptr = NULL;
	}

	free(inbufptr);
	iconv_close(cd);

	return outbufptr;
}


static int handle_capabilities_res_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int count = 0;
	int sccp_codec = 0;
	int i = 0;
	struct sccp_device *device = NULL;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	device = session->device;
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	count = letohl(msg->data.caps.count);
	ast_log(LOG_DEBUG, "Received %d capabilities\n", count);

	if (count > SCCP_MAX_CAPABILITIES) {
		count = SCCP_MAX_CAPABILITIES;
		ast_log(LOG_WARNING, "Received more capabilities (%d) than we can handle (%d)\n", count, SCCP_MAX_CAPABILITIES);
	}

	for (i = 0; i < count; i++) {

		/* get the device supported codecs */
		sccp_codec = letohl(msg->data.caps.caps[i].codec);

		/* translate to asterisk format */
		device->codecs |= codec_sccp2ast(sccp_codec);
	}

	return 0;
}

static int handle_open_receive_channel_ack_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_line *line = NULL;
	struct sockaddr_in remote = {0};
	struct ast_sockaddr remote_tmp;
	struct sockaddr_in local = {0};
	struct ast_sockaddr local_tmp;
	struct ast_format_list fmt = {0};
	uint32_t passthruid = 0;
	uint32_t addr = 0;
	uint32_t port = 0;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	line = device_get_active_line(session->device);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return 0;
	}

	addr = msg->data.openreceivechannelack.ipAddr;
	port = letohl(msg->data.openreceivechannelack.port);
	passthruid = letohl(msg->data.openreceivechannelack.passThruId);

	remote.sin_family = AF_INET;
	remote.sin_addr.s_addr = addr;
	remote.sin_port = htons(port);

	line->device->remote = remote;

	if (line->active_subchan == NULL) {
		ast_log(LOG_DEBUG, "active_subchan is NULL\n");
		return 0;
	}

	if (line->active_subchan->rtp) {

		ast_sockaddr_from_sin(&remote_tmp, &remote);
		ast_rtp_instance_set_remote_address(line->active_subchan->rtp, &remote_tmp);

		ast_rtp_instance_get_local_address(line->active_subchan->rtp, &local_tmp);
		ast_sockaddr_to_sin(&local_tmp, &local);

		if (local.sin_addr.s_addr == 0)
			local.sin_addr.s_addr = line->device->localip.sin_addr.s_addr;
	}
	else {
		ast_log(LOG_DEBUG, "line->active_subchan->rtp is NULL\n");
		return 0;
	}

	ast_log(LOG_DEBUG, "local address %s:%d\n", ast_inet_ntoa(local.sin_addr), ntohs(local.sin_port));
	ast_log(LOG_DEBUG, "remote address %s:%d\n", ast_inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));

	fmt = ast_codec_pref_getsize(&line->codec_pref, ast_best_codec(line->device->codecs));

	ret = transmit_start_media_transmission(line, line->active_subchan->id, local, fmt);
		if (ret == -1)
			return -1;

	if (line->active_subchan && line->active_subchan->channel) {
		usleep(200000);
		ast_queue_control(line->active_subchan->channel, AST_CONTROL_UNHOLD);
	}

	return 0;
}

static int handle_line_status_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;
	int line_instance = 0;
	char *displayname = NULL;
	struct sccp_line *line = NULL;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	line_instance = letohl(msg->data.line.lineInstance);
	line = device_get_line(session->device, line_instance);

	if (line == NULL) {
		ast_log(LOG_DEBUG, "Line instance [%d] is not attached to device [%s]\n", line_instance, session->device->name);
		return -1;
	}

	msg = msg_alloc(sizeof(struct line_status_res_message), LINE_STATUS_RES_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	msg->data.linestatus.lineNumber = letohl(line_instance);

	if (line->device->protoVersion <= 11) {
		displayname = utf8_to_iso88591(line->cid_name);
	}

	memcpy(msg->data.linestatus.lineDirNumber, line->cid_num, sizeof(msg->data.linestatus.lineDirNumber));
	if (displayname)
		memcpy(msg->data.linestatus.lineDisplayName, displayname, sizeof(msg->data.linestatus.lineDisplayName));
	else
		memcpy(msg->data.linestatus.lineDisplayName, line->cid_name, sizeof(msg->data.linestatus.lineDisplayName));
	memcpy(msg->data.linestatus.lineDisplayAlias, line->cid_num, sizeof(msg->data.linestatus.lineDisplayAlias));

	free(displayname);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	msg = msg_alloc(sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	msg->data.forwardstatus.status = 0;
	msg->data.forwardstatus.lineInstance = htolel(line_instance);
	
	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	post_line_register_check(session);

	return 0; 
}

static int handle_register_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	ast_log(LOG_DEBUG, "name %s\n", msg->data.reg.name);
	ast_log(LOG_DEBUG, "userId %d\n", msg->data.reg.userId);
	ast_log(LOG_DEBUG, "lineInstance %d\n", msg->data.reg.lineInstance);
	ast_log(LOG_DEBUG, "maxStream %d\n", msg->data.reg.maxStreams);
	ast_log(LOG_DEBUG, "activeStreams %d\n", msg->data.reg.activeStreams);
	ast_log(LOG_DEBUG, "protoVersion %d\n", msg->data.reg.protoVersion);

	ret = device_type_is_supported(msg->data.reg.type);
	if (ret == 0) {
		ast_log(LOG_ERROR, "Rejecting [%s], unsupported device type [%d]\n", msg->data.reg.name, msg->data.reg.type);
		msg = msg_alloc(sizeof(struct register_rej_message), REGISTER_REJ_MESSAGE);

		if (msg == NULL) {
			return -1;
		}

		snprintf(msg->data.regrej.errMsg, sizeof(msg->data.regrej.errMsg), "Unsupported device type [%d]\n", msg->data.reg.type);
		ret = transmit_message(msg, session);
		if (ret == -1)
			return -1;

		return 0;
	}

	ret = register_device(msg, session);
	if (ret <= 0) {
		ast_log(LOG_ERROR, "Rejecting device [%s]\n", msg->data.reg.name);
		msg = msg_alloc(sizeof(struct register_rej_message), REGISTER_REJ_MESSAGE);

		if (msg == NULL) {
			ast_log(LOG_ERROR, "msg allocation failed\n");
			return -1;
		}

		snprintf(msg->data.regrej.errMsg, sizeof(msg->data.regrej.errMsg), "Access denied [%s]\n", msg->data.reg.name);
		ret = transmit_message(msg, session);
		if (ret == -1)
			return -1;

		return 0;
	}

	msg = msg_alloc(sizeof(struct register_ack_message), REGISTER_ACK_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

        msg->data.regack.keepAlive = htolel(sccp_config->keepalive);
        memcpy(msg->data.regack.dateTemplate, sccp_config->dateformat, sizeof(msg->data.regack.dateTemplate));

	if (session->device->protoVersion <= 3) {

		msg->data.regack.protoVersion = 3;

		msg->data.regack.unknown1 = 0x00;
		msg->data.regack.unknown2 = 0x00;
		msg->data.regack.unknown3 = 0x00;

	} else if (session->device->protoVersion <= 10) {

		msg->data.regack.protoVersion = session->device->protoVersion;

		msg->data.regack.unknown1 = 0x20;
		msg->data.regack.unknown2 = 0x00;
		msg->data.regack.unknown3 = 0xFE;

	} else if (session->device->protoVersion >= 11) {

		msg->data.regack.protoVersion = 11;

		msg->data.regack.unknown1 = 0x20;
		msg->data.regack.unknown2 = 0xF1;
		msg->data.regack.unknown3 = 0xFF;
	}

        msg->data.regack.secondaryKeepAlive = htolel(sccp_config->keepalive);

        ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	msg = msg_alloc(0, CAPABILITIES_REQ_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return -1;
	}

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1; 

	post_register_check(session);

	return 0;
}

static int handle_ipport_message(struct sccp_msg *msg, struct sccp_session *session)
{
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	session->device->station_port = msg->data.ipport.stationIpPort;

	return 0;
}

static int handle_voicemail_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_line *line = NULL;

	line = device_get_line(session->device, 1);
	do_newcall(1, 0, session);

	/* open our speaker */
	transmit_speaker_mode(session, SCCP_SPEAKERON);

	ast_copy_string(line->device->exten, sccp_config->vmexten, sizeof(line->device->exten));

	line->device->exten[3] = '#';
	line->device->exten[4] = '\0';

	return 0;
}

static int handle_enbloc_call_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_device *device = NULL;
	struct sccp_line *line = NULL;
	size_t len = 0;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	device = session->device;
	line = device_get_active_line(device);

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	/* contains all the digits entered before pressing 'Dial' */
	if (line->state == SCCP_OFFHOOK) {
		len = strlen(msg->data.enbloc.extension);
		ast_copy_string(line->device->exten, msg->data.enbloc.extension,
					sizeof(line->device->exten));
		line->device->exten[len+1] = '#';
		line->device->exten[len+2] = '\0';
	}

	return 0;
}

static int handle_keypad_button_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;
	struct sccp_line *line = NULL;
	struct ast_frame frame = { AST_FRAME_DTMF, };

	char digit;
	int button;
	int instance;
	int callid;
	size_t len;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	button = letohl(msg->data.keypad.button);
	instance = letohl(msg->data.keypad.lineInstance);
	callid = letohl(msg->data.keypad.callInstance);

	if (session->device->type == SCCP_DEVICE_7912
		|| session->device->type == SCCP_DEVICE_7905) {

		instance = 1;
	}

	line = device_get_line(session->device, instance);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "Device [%s] has no line instance [%d]\n", session->device->name, instance);
		return 0;
	}

	if (button == 14) {
		digit = '*';
	} else if (button == 15) {
		digit = '#';
	} else if (button >= 0 && button <= 9) {
		digit = '0' + button;
	} else {
		digit = '0' + button;
		ast_log(LOG_WARNING, "Unsupported digit %d\n", button);
	}

	if (line->state == SCCP_CONNECTED) {

		frame.subclass.integer = digit;
		frame.src = "sccp";
		frame.len = 100;
		frame.offset = 0;
		frame.datalen = 0;

		ast_queue_frame(line->active_subchan->channel, &frame);

	} else if (line->state == SCCP_OFFHOOK) {

		len = strlen(line->device->exten);
		if (len < sizeof(line->device->exten) - 1) {
			line->device->exten[len] = digit;
			line->device->exten[len+1] = '\0';
		}

		ret = transmit_tone(session, SCCP_TONE_NONE, line->instance, 0);
		if (ret == -1)
			return -1;
	}

	return 0;
}

static void destroy_session(struct sccp_session **session)
{
	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return;
	}

	if (*session == NULL) {
		ast_log(LOG_DEBUG, "*session is NULL\n");
		return;
	}

	ast_mutex_destroy(&(*session)->lock);
	ast_free((*session)->ipaddr);
	close((*session)->sockfd);

	if ((*session)->device)
		(*session)->device->session = NULL;

	ast_free(*session);
	*session = NULL;
}

static int handle_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	/* Device is not configured */
	if (session->device == NULL &&
		(msg->id != REGISTER_MESSAGE && msg->id != ALARM_MESSAGE)) {
			return -1;
	}

	/* Prevent unregistered phone from sending non-registering messages */
	if (((session->device != NULL && session->device->registered == DEVICE_REGISTERED_FALSE)) &&
		(msg->id != REGISTER_MESSAGE && msg->id != ALARM_MESSAGE)) {

			ast_log(LOG_ERROR, "Session from [%s::%d] sending non-registering messages\n",
						session->ipaddr, session->sockfd);
			return -1;
	}

	switch (msg->id) {
	case KEEP_ALIVE_MESSAGE:
		ast_log(LOG_DEBUG, "Keep alive message\n");
		ret = handle_keep_alive_message(session);
		break;

	case REGISTER_MESSAGE:
		ast_log(LOG_DEBUG, "Register message\n");
		ret = handle_register_message(msg, session);
		break;

	case IP_PORT_MESSAGE:
		ast_log(LOG_DEBUG, "Ip port message\n");
		ret = handle_ipport_message(msg, session);
		break;

	case ENBLOC_CALL_MESSAGE:
		ast_log(LOG_DEBUG, "Enbloc call message\n");
		ret = handle_enbloc_call_message(msg, session);
		break;

	case VOICEMAIL_MESSAGE:
		ast_log(LOG_DEBUG, "voicemail message\n");
		ret = handle_voicemail_message(msg, session);
		break;

	case KEYPAD_BUTTON_MESSAGE:
		ast_log(LOG_DEBUG, "keypad button message\n");
		ret = handle_keypad_button_message(msg, session);
		break;

	case OFFHOOK_MESSAGE:
		ast_log(LOG_DEBUG, "Offhook message\n");
		ret = handle_offhook_message(msg, session);
		break;

	case ONHOOK_MESSAGE:
		ast_log(LOG_DEBUG, "Onhook message\n");
		ret = handle_onhook_message(msg, session);
		break;

	case FORWARD_STATUS_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Forward status message\n");
		ret = handle_forward_status_req_message(msg, session);
		break;

	case CAPABILITIES_RES_MESSAGE:
		ast_log(LOG_DEBUG, "Capabilities message\n");
		ret = handle_capabilities_res_message(msg, session);
		break;

	case LINE_STATUS_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Line status message\n");
		ret = handle_line_status_req_message(msg, session);
		break;

	case CONFIG_STATUS_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Config status message\n");
		ret = handle_config_status_req_message(session);
		break;

	case TIME_DATE_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Time date message\n");
		ret = handle_time_date_req_message(session);
		break;

	case BUTTON_TEMPLATE_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Button template request message\n");
		ret = handle_button_template_req_message(session);
		break;

	case SOFTKEY_TEMPLATE_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Softkey template request message\n");
		ret = handle_softkey_template_req_message(session);
		break;

	case ALARM_MESSAGE:
		ast_log(LOG_DEBUG, "Alarm message: %s\n", msg->data.alarm.displayMessage);
		break;

	case SOFTKEY_EVENT_MESSAGE:
		ast_log(LOG_DEBUG, "Softkey event message\n");
		ret = handle_softkey_event_message(msg, session);
		break;

	case OPEN_RECEIVE_CHANNEL_ACK_MESSAGE:
		ast_log(LOG_DEBUG, "Open receive channel ack message\n");
		ret = handle_open_receive_channel_ack_message(msg, session);
		break;

	case SOFTKEY_SET_REQ_MESSAGE:
		ast_log(LOG_DEBUG, "Softkey set request message\n");
		ret = handle_softkey_set_req_message(session);
		break;

	case REGISTER_AVAILABLE_LINES_MESSAGE:
		ast_log(LOG_DEBUG, "Register available lines message\n");
		break;

	case START_MEDIA_TRANSMISSION_ACK_MESSAGE:
		ast_log(LOG_DEBUG, "Start media transmission ack message\n");
		break;

	case ACCESSORY_STATUS_MESSAGE:
		break;

	default:
		ast_log(LOG_DEBUG, "Unknown message %x\n", msg->id);
		break;
	}

	return ret;
}

static int fetch_data(struct sccp_session *session)
{
	struct pollfd fds[1] = {0};
	int nfds = 0;
	time_t now = 0;
	ssize_t nbyte = 0;
	int msg_len = 0;

	if (session == NULL)
		return -1;
	
	time(&now);

	/* if no device or device is not registered and time has elapsed */
	if ((session->device == NULL || (session->device != NULL && session->device->registered == DEVICE_REGISTERED_FALSE))
		&& now > session->start_time + sccp_config->authtimeout) {
		ast_log(LOG_WARNING, "Time has elapsed [%dsec]\n", sccp_config->authtimeout);
		return -1;
	}

	fds[0].fd = session->sockfd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	/* wait N times the keepalive frequence */
	nfds = ast_poll(fds, 1, sccp_config->keepalive * 1000 * 2);
	if (nfds == -1) { /* something wrong happend */
		ast_log(LOG_WARNING, "Failed to poll socket: %s\n", strerror(errno));
		return -1;

	} else if (nfds == 0) { /* the file descriptor is not ready */
		ast_log(LOG_WARNING, "Device has timed out\n");
		return -1;

	} else if (fds[0].revents & POLLERR || fds[0].revents & POLLHUP) {
		ast_log(LOG_WARNING, "Device has closed the connection\n");
		return -1;

	} else if (fds[0].revents & POLLIN || fds[0].revents & POLLPRI) {

		/* fetch the field that contain the packet length */
		nbyte = read(session->sockfd, session->inbuf, 4);
		if (nbyte < 0) { /* something wrong happend */
			ast_log(LOG_WARNING, "Failed to read socket: %s\n", strerror(errno));
			return -1;

		} else if (nbyte == 0) { /* EOF */
			ast_log(LOG_NOTICE, "Device has closed the connection\n");
			return -1;

		} else if (nbyte < 4) {
			ast_log(LOG_WARNING, "Client sent less data than expected. Expected at least 4 bytes but got %d\n", nbyte);
			return -1;
		}

		msg_len = letohl(*((int *)session->inbuf));
		if (msg_len > SCCP_MAX_PACKET_SZ || msg_len < 0) {
			ast_log(LOG_WARNING, "Packet length is out of bounds: 0 > %d > %d\n", msg_len, SCCP_MAX_PACKET_SZ);
			return -1;
		}

		/* bypass the length field and fetch the payload */
		nbyte = read(session->sockfd, session->inbuf+4, msg_len+4);
		if (nbyte < 0) {
			ast_log(LOG_WARNING, "Failed to read socket: %s\n", strerror(errno));
			return -1;

		} else if (nbyte == 0) { /* EOF */
			ast_log(LOG_NOTICE, "Device has closed the connection\n");
			return -1;
		}

		return nbyte;
	}

	return -1;	
}

static void *thread_session(void *data)
{
	int ret = 0;
	struct sccp_session *session = data;
	struct sccp_msg *msg = NULL;
	int connected = 1;

	while (connected) {

		ret = fetch_data(session);
		if (ret > 0) {
			msg = (struct sccp_msg *)session->inbuf;
			ret = handle_message(msg, session);
			/* take it easy, prevent DoS attack */
			usleep(100000);
		}

		if (ret == -1) {
			AST_LIST_LOCK(&list_session);
			session = AST_LIST_REMOVE(&list_session, session, list);
			AST_LIST_UNLOCK(&list_session);	

			if (session->device) {
				ast_log(LOG_ERROR, "Disconnecting device [%s]\n", session->device->name);
				device_unregister(session->device);
				ast_devstate_changed(AST_DEVICE_UNAVAILABLE, "SCCP/%s", session->device->default_line->name);
			}

			connected = 0;
		}
	}

	if (session) {
		transmit_reset(session, 2);
		destroy_session(&session);
	}

	return 0;
}

static void *thread_accept(void *data)
{
	int new_sockfd = 0;
	struct sockaddr_in addr = {0};
	struct sccp_session *session = NULL;
	socklen_t addrlen = 0;
	int flag_nodelay = 1;

	while (1) {

		addrlen = sizeof(addr);
		new_sockfd = accept(sccp_srv.sockfd, (struct sockaddr *)&addr, &addrlen);
		if (new_sockfd == -1) {
			ast_log(LOG_ERROR, "Failed to accept new connection: %s... "
						"the main thread is going down now\n", strerror(errno));
			return NULL;
		}

		/* send multiple buffers as individual packets */
		setsockopt(new_sockfd, IPPROTO_TCP, TCP_NODELAY, &flag_nodelay, sizeof(flag_nodelay));

		/* session constructor */	
		session = ast_calloc(1, sizeof(struct sccp_session));
		if (session == NULL) {
			ast_log(LOG_ERROR, "Failed to allocate new session, "
						"the main thread is going down now\n");
			close(new_sockfd);
			return NULL;
		}

		session->tid = AST_PTHREADT_NULL; 
		session->sockfd = new_sockfd;
		session->ipaddr = ast_strdup(ast_inet_ntoa(addr.sin_addr));
		ast_mutex_init(&session->lock);
		time(&session->start_time);
	
		AST_LIST_LOCK(&list_session);
		AST_LIST_INSERT_HEAD(&list_session, session, list);
		AST_LIST_UNLOCK(&list_session);

		ast_log(LOG_NOTICE, "A new device has connected from: %s\n", session->ipaddr);
		ast_pthread_create_background(&session->tid, NULL, thread_session, session);
	}

	return NULL;
}

static int cb_ast_devicestate(void *data)
{
	ast_log(LOG_DEBUG, "devicestate %s\n", (char *)data);

	struct sccp_line *line = NULL;
	char *name = NULL;
	char *ptr = NULL;
	int state = AST_DEVICE_UNKNOWN;

	name = strdup(data);

	ptr = strchr(name, '/');
	if (ptr != NULL)
		*ptr = '\0';

	line = find_line_by_name(name, &sccp_config->list_line);
	if (line != NULL) {
		if (line->state == SCCP_ONHOOK)
			state = AST_DEVICE_NOT_INUSE;
		else
			state = AST_DEVICE_INUSE;
	}

	free(name);
	return state;
}

static struct ast_channel *cb_ast_request(const char *type,
					format_t format,
					const struct ast_channel *requestor,
					void *destination,
					int *cause)
{
	struct sccp_line *line = NULL;
	struct sccp_subchannel *subchan = NULL;
	struct ast_channel *channel = NULL;
	char *option = NULL;

	if (type == NULL)
		return NULL;

	if (destination == NULL)
		return NULL;

	if (cause == NULL)
		return NULL;

	option = strchr(destination, '/');
	if (option != NULL) {
		*option = '\0';
		option++;
	}

	ast_log(LOG_DEBUG, "type: %s "
			"format: %s "
			"destination: %s "
			"option: %s "
			"cause: %d\n",
			type, ast_getformatname(format),
			(char *)destination, option? option: "", *cause);

	line = find_line_by_name((char *)destination, &sccp_config->list_line);

	if (line == NULL) {
		ast_log(LOG_NOTICE, "This line doesn't exist: %s\n", (char *)destination);
		*cause = AST_CAUSE_UNREGISTERED;
		return NULL;
	}

	if (line->device == NULL) {
		ast_log(LOG_NOTICE, "This line has no device: %s\n", (char *)destination);
		*cause = AST_CAUSE_UNREGISTERED;
		return NULL;
	}

	if (line->device->registered == DEVICE_REGISTERED_FALSE) {
		ast_log(LOG_NOTICE, "Line [%s] belong to an unregistered device [%s]\n", line->name, line->device->name);
		*cause = AST_CAUSE_UNREGISTERED;
		return NULL;
	}

	if (option != NULL && !strncmp(option, "autoanswer", 10)) {
		line->device->autoanswer = 1;
	}

	subchan = sccp_new_subchannel(line);
	channel = sccp_new_channel(subchan, requestor ? requestor->linkedid : NULL);

	if (!line->active_subchan && !line->device->early_remote && sccp_config->directmedia) {
		line->device->early_remote = 1;
		transmit_connect(line, subchan->id);
	}

	return channel;
}

static int sccp_autoanswer_call(void *data)
{
	int ret = 0;
	struct sccp_subchannel *subchan = NULL;
	struct sccp_line *line = NULL;
	struct sccp_session *session = NULL;

	subchan = data;
	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "sbuchan is NULL\n");
		return 0;
	}

	line = device_get_active_line(subchan->line->device);
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return 0;
	}

	session = line->device->session;
	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return 0;
	}

	transmit_speaker_mode(session, SCCP_SPEAKERON);

	do_answer(line->instance, subchan->id, session);

	return 0;
}

static int cb_ast_call(struct ast_channel *channel, char *dest, int timeout)
{
	int ret = 0;
	struct sccp_subchannel *subchan = NULL;
	struct sccp_line *line = NULL;
	struct sccp_device *device = NULL;
	struct sccp_session *session = NULL;

	if (channel == NULL) {
		ast_log(LOG_DEBUG, "channel is NULL\n");
		return -1;
	}

	if (dest == NULL) {
		ast_log(LOG_DEBUG, "dest is NULL\n");
		return -1;
	}

	subchan = channel->tech_pvt;
	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "channel has no valid tech_pvt\n");
		return -1;
	}

	line = subchan->line;
	if (line == NULL) {
		ast_log(LOG_DEBUG, "subchan has no valid line\n");
		return -1;
	}

	device = line->device;
	if (device == NULL) {
		ast_log(LOG_DEBUG, "Line [%s] is attached to no device\n", line->name);
		return -1;
	}

	session = device->session;
	if (session == NULL) {
		ast_log(LOG_DEBUG, "Device [%s] has no active session\n", device->name);
		return -1;
	}

	ast_log(LOG_DEBUG, "destination: %s\n", dest);

	ast_setstate(channel, AST_STATE_RINGING);
	ast_queue_control(channel, AST_CONTROL_RINGING);

	if (line->callfwd == SCCP_CFWD_ACTIVE) {
		ast_log(LOG_DEBUG, "CFwdALL: %s\n", line->callfwd_exten);
		return 0;
	}

	device_enqueue_line(device, line);
	subchan_set_state(subchan, SCCP_RINGIN);

	/* If the line has an active subchannel, it means that
	 * a call is already ongoing. */
	if (line->active_subchan == NULL) {
		set_line_state(line, SCCP_RINGIN);

		ret = transmit_ringer_mode(session, SCCP_RING_INSIDE);
		if (ret == -1)
			return -1;
	}

	ret = transmit_callstate(session, line->instance, SCCP_RINGIN, subchan->id);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, line->instance, subchan->id, KEYDEF_RINGIN);
	if (ret == -1)
		return -1;

	char *namestr = NULL;
	char *numberstr = NULL;

	if (line->device->protoVersion <= 11) {
		namestr = utf8_to_iso88591(channel->connected.id.name.str);
		numberstr = utf8_to_iso88591(channel->connected.id.number.str);
	}

	ret = transmit_callinfo(session,
				namestr ? namestr : channel->connected.id.name.str,
				numberstr ? numberstr : channel->connected.id.number.str,
					line->cid_name,
					line->cid_num,
					line->instance,
					subchan->id, 1);

	free(namestr);
	free(numberstr);

	if (ret == -1)
		return -1;

	ret = transmit_lamp_state(session, STIMULUS_LINE, line->instance, SCCP_LAMP_BLINK);
	if (ret == -1)
		return -1;

	if (line->device->autoanswer == 1) {
		line->device->autoanswer = 0;
		sccp_autoanswer_call(subchan);
		return 0;
	}

	ast_devstate_changed(AST_DEVICE_RINGING, "SCCP/%s", line->name);

	return 0;
}

static int cb_ast_hangup(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = NULL;

	if (channel == NULL) {
		ast_log(LOG_DEBUG, "channel is NULL\n");
		return -1;
	}

	subchan = channel->tech_pvt;
	if (subchan != NULL) {
		do_clear_subchannel(subchan);
		subchan->channel = NULL;
	}

	ast_setstate(channel, AST_STATE_DOWN);
	channel->tech_pvt = NULL;
	ast_module_unref(ast_module_info->self);

	return 0;
}

static int cb_ast_answer(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = NULL;
	struct sccp_line *line = NULL;

	if (channel == NULL) {
		ast_log(LOG_DEBUG, "channel is NULL\n");
		return -1;
	}

	subchan = channel->tech_pvt;
	line = subchan->line;

	if (subchan->rtp == NULL) {
		ast_log(LOG_DEBUG, "rtp is NULL\n");
		start_rtp(subchan);

		/* Wait for the phone to provide his ip:port information
		   before the bridging is being done. */
		usleep(500000);
	}

	if (subchan->on_hold) {
		return 0;
	}

	transmit_tone(line->device->session, SCCP_TONE_NONE, line->instance, subchan->id);
	transmit_selectsoftkeys(line->device->session, line->instance, subchan->id, KEYDEF_CONNECTED);
	transmit_callstate(line->device->session, line->instance, SCCP_CONNECTED, subchan->id);

	ast_setstate(channel, AST_STATE_UP);
	set_line_state(line, SCCP_CONNECTED);

	return 0;
}

static struct ast_frame *cb_ast_read(struct ast_channel *channel)
{
	struct sccp_subchannel *subchan = NULL;
	struct sccp_line *line = NULL;
	struct ast_frame *frame = NULL;

	if (channel == NULL) {
		ast_log(LOG_DEBUG, "channel is NULL\n");
		return NULL;
	}

	subchan = channel->tech_pvt;
	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "channel has no valid tech_pvt\n");
		return &ast_null_frame;
	}

	line = subchan->line;
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return &ast_null_frame;
	}

	if (subchan->rtp == NULL) {
		ast_log(LOG_DEBUG, "rtp is NULL\n");
		return &ast_null_frame;
	}

	switch (channel->fdno) {
	case 0:
		frame = ast_rtp_instance_read(subchan->rtp, 0);
		break;

	case 1:
		frame = ast_rtp_instance_read(subchan->rtp, 1);
		break;

	default:
		frame = &ast_null_frame;
	}

	if (frame && frame->frametype == AST_FRAME_VOICE) {
		if (frame->subclass.codec != channel->nativeformats) {
			channel->nativeformats = frame->subclass.codec;
			ast_set_read_format(channel, channel->readformat);
			ast_set_write_format(channel, channel->writeformat);
		}
	}

	return frame;
}

static int cb_ast_write(struct ast_channel *channel, struct ast_frame *frame)
{
	int res = 0;
	struct sccp_subchannel *subchan = NULL;
	struct sccp_line *line = NULL;

	if (channel == NULL) {
		ast_log(LOG_DEBUG, "channel is NULL\n");
		return -1;
	}

	if (frame == NULL) {
		ast_log(LOG_DEBUG, "frame is NULL\n");
		return -1;
	}

	subchan = channel->tech_pvt;
	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "channel has no tech_pvt\n");
		return 0;
	}

	line = subchan->line;
	if (line == NULL) {
		ast_log(LOG_DEBUG, "subchan has no valid line\n");
		return 0;
	}

	if (subchan->rtp != NULL && line->state == SCCP_CONNECTED) {
		res = ast_rtp_instance_write(subchan->rtp, frame);
	}

	return res;
}

static int cb_ast_indicate(struct ast_channel *channel, int indicate, const void *data, size_t datalen)
{
	struct sccp_subchannel *subchan = NULL;
	struct sccp_line *line = NULL;

	if (channel == NULL) {
		ast_log(LOG_DEBUG, "channel is NULL\n");
		return -1;
	}

	subchan = channel->tech_pvt;
	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return 0;
	}

	line = subchan->line;
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return 0;
	}

	switch (indicate) {
	case AST_CONTROL_HANGUP:
		ast_log(LOG_DEBUG, "hangup\n");
		break;

	case AST_CONTROL_RING:
		ast_log(LOG_DEBUG, "ring\n");
		break;

	case AST_CONTROL_RINGING:
		ast_log(LOG_DEBUG, "ringing\n");
		break;

	case AST_CONTROL_ANSWER:
		ast_log(LOG_DEBUG, "answer\n");
		break;

	case AST_CONTROL_BUSY:

		transmit_ringer_mode(line->device->session, SCCP_RING_OFF);
		transmit_tone(line->device->session, SCCP_TONE_BUSY, line->instance, subchan->id);

		ast_log(LOG_DEBUG, "busy\n");
		break;

	case AST_CONTROL_TAKEOFFHOOK:
		ast_log(LOG_DEBUG, "takeoffhook\n");
		break;

	case AST_CONTROL_OFFHOOK:
		ast_log(LOG_DEBUG, "offhook\n");
		break;

	case AST_CONTROL_CONGESTION:

		transmit_ringer_mode(line->device->session, SCCP_RING_OFF);
		transmit_tone(line->device->session, SCCP_TONE_BUSY, line->instance, subchan->id);

		ast_log(LOG_DEBUG, "congestion\n");
		break;

	case AST_CONTROL_FLASH:
		ast_log(LOG_DEBUG, "flash\n");
		break;

	case AST_CONTROL_WINK:
		ast_log(LOG_DEBUG, "wink\n");
		break;

	case AST_CONTROL_OPTION:
		ast_log(LOG_DEBUG, "option\n");
		break;

	case AST_CONTROL_RADIO_KEY:
		ast_log(LOG_DEBUG, "radio key\n");
		break;

	case AST_CONTROL_RADIO_UNKEY:
		ast_log(LOG_DEBUG, "radio unkey\n");
		break;

	case AST_CONTROL_PROGRESS:
		ast_log(LOG_DEBUG, "progress\n");
		break;

	case AST_CONTROL_PROCEEDING:
		ast_log(LOG_DEBUG, "proceeding\n");
		break;

	case AST_CONTROL_HOLD:
		ast_moh_start(channel, data, NULL);
		break;

	case AST_CONTROL_UNHOLD:
		ast_moh_stop(channel);
		break;

	case AST_CONTROL_VIDUPDATE:
		ast_log(LOG_DEBUG, "vid update\n");
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

	case AST_CONTROL_END_OF_Q:
		ast_log(LOG_DEBUG, "end of q\n");
		break;

	default:
		break;
	}

	return 0;
}

static int cb_ast_fixup(struct ast_channel *oldchannel, struct ast_channel *newchannel)
{
	struct sccp_subchannel *subchan = NULL;

	if (oldchannel == NULL) {
		ast_log(LOG_DEBUG, "oldchannel is NULL\n");
		return -1;
	}

	if (newchannel == NULL) {
		ast_log(LOG_DEBUG, "newchannel is NULL\n");
		return -1;
	}

	subchan = newchannel->tech_pvt;
	subchan->channel = newchannel;

	cb_ast_set_rtp_peer(newchannel, NULL, NULL, NULL, 0, 0);

	return 0;
}

static int cb_ast_senddigit_begin(struct ast_channel *channel, char digit)
{
	ast_log(LOG_DEBUG, "senddigit begin\n");
	return 0;
}

static int cb_ast_senddigit_end(struct ast_channel *channel, char digit, unsigned int duration)
{
	ast_log(LOG_DEBUG, "senddigit end\n");
	return 0;
}

AST_TEST_DEFINE(sccp_test_null_arguments)
{
	int ret = 0;
	enum ast_test_result_state result = AST_TEST_PASS;
	void *retptr = NULL;

	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_null_arguments";
		info->category = "/channel/sccp/";
		info->summary = "test null arguments";
		info->description = "Test how functions behave when arguments passed are NULL.";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	ast_test_status_update(test, "Executing sccp test null arguments...\n");

	retptr = (void*)0x1;
	retptr = (void*)utf8_to_iso88591(NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: utf8_to_iso88591(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

        retptr = NULL;
        retptr = utf8_to_iso88591("0sïkö Düô");
        if (retptr == NULL) {
                ast_test_status_update(test, "failed: retptr == NULL\n");
                result = AST_TEST_FAIL;
                goto cleanup;
        }

	retptr = NULL;
	retptr = utf8_to_iso88591("Ã©");
	if (strcmp(retptr, "é")) {
		ast_test_status_update(test, "failed: 'é' != %s\n", retptr);
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_template_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_template_req_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_config_status_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_config_status_req_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_time_date_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_time_date_req_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_button_template_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_button_template_req_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_keep_alive_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_keep_alive_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = register_device(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: register_device(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = register_device((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: register_device(0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = sccp_new_channel(NULL, (void*)0xFF);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: sccp_new_channel(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (cb_ast_get_rtp_peer(NULL, (void*)0xFF) != AST_RTP_GLUE_RESULT_FORBID) {
		ast_test_status_update(test, "failed: cb_ast_get_rtp_peer(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (cb_ast_get_rtp_peer((void*)0xFF, NULL) != AST_RTP_GLUE_RESULT_FORBID) {
		ast_test_status_update(test, "failed: cb_ast_get_rtp_peer(0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = start_rtp(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: start_rtp(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = sccp_start_the_call(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: sccp_start_the_call(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = sccp_lookup_exten(NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: sccp_lookup_exten(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_offhook_message(NULL, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_offhook_message(NULL, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_onhook_message(NULL, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_onhook_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_hold(0, 0, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_hold(0, 0, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_resume(0, 0, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_resume(0, 0, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_transfer(0, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_transfer(0, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_event_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_event_message(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_event_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_event_message(0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_set_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_set_req_message(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = codec_ast2sccp(0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: codec_ast2sccp(0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = codec_sccp2ast(0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: codec_sccp2ast(0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_capabilities_res_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_capabilities_res_message(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_capabilities_res_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_capabilities_res_message(0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_open_receive_channel_ack_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_open_receive_channel_ack_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_open_receive_channel_ack_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_open_receive_channel_ack_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_line_status_req_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_line_status_req_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_line_status_req_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_line_status_req_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_register_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_register_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_register_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_register_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_ipport_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_ipport_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_ipport_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_ipport_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_keypad_button_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_ipport_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_keypad_button_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_keypad_button_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = fetch_data(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: fetch_data(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = cb_ast_request(NULL, 0xFF, (void*)0xFF, (void*)0xFF, (void*)0xFF);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: cb_ast_request(NULL, 0xFF, (void*)0xFF, (void*)0xFF, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = cb_ast_request((void*)0xFF, 0xFF, (void*)0xFF, NULL, (void*)0xFF);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: cb_ast_request((void*)0xFF, 0xFF, NULL, (void*)0xFF, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = cb_ast_request((void*)0xFF, 0xFF, (void*)0xFF, (void*)0xFF, NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: cb_ast_request((void*)0xFF, 0xFF, (void*)0xFF, (void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_call(NULL, (void*)0xFF, 0);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_call(NULL, (void*)0xFF, 0)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_call((void*)0xFF, NULL, 0);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_call((void*)0xFF, NULL, 0)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_hangup(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_hangup(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_answer(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_answer(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = cb_ast_read(NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: cb_ast_read(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_write(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_write(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_write((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_write((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_indicate(NULL, 0xFF, (void*)0xFF, 0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_indicate(NULL, 0xFF, (void*)0xFF, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_fixup(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_fixup(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_fixup((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_fixup((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

cleanup:
	return result;
}

static char *sccp_show_lines(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct sccp_line *line_itr = NULL;
	int line_cnt = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show lines";
		e->usage = "Usage: sccp show lines\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ast_cli(a->fd, "%-9s %-8s\n", "Line", "state");
	ast_cli(a->fd, "=======   ==========\n");
	AST_RWLIST_RDLOCK(&sccp_config->list_line);
	AST_RWLIST_TRAVERSE(&sccp_config->list_line, line_itr, list) {
		ast_cli(a->fd, "%-9s %-8s\n", line_itr->name, line_state_str(line_itr->state));
		line_cnt++;
	}
	AST_RWLIST_UNLOCK(&sccp_config->list_line);
	ast_cli(a->fd, "Total: %d line(s)\n", line_cnt);

	return CLI_SUCCESS;
}

static char *sccp_set_directmedia_on(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp set directmedia on";
		e->usage = "usage: sccp set directmedia on\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	sccp_config->directmedia = 1;

	return CLI_SUCCESS;
}

static char *sccp_set_directmedia_off(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp set directmedia off";
		e->usage = "usage: sccp set directmedia off\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	sccp_config->directmedia = 0;

	return CLI_SUCCESS;
}

static char *sccp_show_devices(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct sccp_device *device_itr = NULL;
	struct sccp_session *session = NULL;
	int dev_cnt = 0;
	int reg_cnt = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show devices";
		e->usage = "Usage: sccp show devices\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ast_cli(a->fd, "%-16s %-16s %-8s %-13s %s\n", "Device", "IP", "Type", "Reg.state", "Proto.Version");
	ast_cli(a->fd, "===============  ===============  ======   ==========    ==============\n");
	AST_RWLIST_RDLOCK(&sccp_config->list_device);
	AST_RWLIST_TRAVERSE(&sccp_config->list_device, device_itr, list) {
		session = device_itr->session;
		ast_cli(a->fd, "%-16s %-16s %-8s %-13s %d\n", device_itr->name,
							session && session->ipaddr ? session->ipaddr: "-",
							device_type_str(device_itr->type),
							device_regstate_str(device_itr->registered),
							device_itr->protoVersion);

		dev_cnt++;
		if (device_itr->registered == DEVICE_REGISTERED_TRUE)
			reg_cnt++;
	}
	AST_RWLIST_UNLOCK(&sccp_config->list_device);
	ast_cli(a->fd, "Total: %d device(s), %d registered\n", dev_cnt, reg_cnt);

	return CLI_SUCCESS;
}

static char *sccp_reset_device(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct sccp_device *device = NULL;
	int restart = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp reset";
		e->usage = "Usage: sccp reset <device> [restart]\n"
				"Cause a SCCP device to reset itself, optionally with a full restart\n";
		return NULL;

	case CLI_GENERATE:
		return NULL;
	}

	device = find_device_by_name(a->argv[2], &sccp_config->list_device);
	if (device == NULL)
		return CLI_FAILURE;

	if (a->argc == 4 && !strcasecmp(a->argv[3], "restart"))
		restart = 1;

	if (restart == 1)
		transmit_reset(device->session, 1);
	else
		transmit_reset(device->session, 2);

	device_unregister(device);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_sccp[] = {
	AST_CLI_DEFINE(sccp_show_lines, "Show the state of the lines"),
	AST_CLI_DEFINE(sccp_show_devices, "Show the state of the devices"),
	AST_CLI_DEFINE(sccp_reset_device, "Reset SCCP device"),
	AST_CLI_DEFINE(sccp_set_directmedia_on, "Turn on direct media"),
	AST_CLI_DEFINE(sccp_set_directmedia_off, "Turn off direct media"),
};

void sccp_server_fini()
{
	struct sccp_session *session_itr = NULL;

	AST_TEST_UNREGISTER(sccp_test_null_arguments);
	ast_cli_unregister_multiple(cli_sccp, ARRAY_LEN(cli_sccp));

	ast_channel_unregister(&sccp_tech);

	pthread_cancel(sccp_srv.thread_accept);
	pthread_kill(sccp_srv.thread_accept, SIGURG);
	pthread_join(sccp_srv.thread_accept, NULL);

	AST_LIST_TRAVERSE_SAFE_BEGIN(&list_session, session_itr, list) {
		if (session_itr != NULL) {

			ast_log(LOG_DEBUG, "Session del %s\n", session_itr->ipaddr);
			AST_LIST_REMOVE_CURRENT(list);

			pthread_cancel(session_itr->tid);
			pthread_kill(session_itr->tid, SIGURG);
			pthread_join(session_itr->tid, NULL);

			destroy_session(&session_itr);
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;

	freeaddrinfo(sccp_srv.res);
	shutdown(sccp_srv.sockfd, SHUT_RDWR);
}

void sccp_rtp_fini()
{
	ast_rtp_glue_unregister(&sccp_rtp_glue);
}

void sccp_rtp_init(const struct ast_module_info *module_info)
{
	ast_module_info = module_info;
	ast_rtp_glue_register(&sccp_rtp_glue);
}

int sccp_server_init(struct sccp_configs *sccp_cfg)
{
	int ret = 0;
	struct addrinfo hints = {0};
	const int flag_reuse = 1;

	AST_TEST_REGISTER(sccp_test_null_arguments);
	ast_cli_register_multiple(cli_sccp, ARRAY_LEN(cli_sccp));

	sccp_config = sccp_cfg;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST;

	ret = getaddrinfo(sccp_config->bindaddr, SCCP_PORT, &hints, &sccp_srv.res);
	if (ret != 0) {
		ast_log(LOG_ERROR, "getaddrinfo: %s: '%s'\n", gai_strerror(ret), sccp_config->bindaddr);
		return -1;
	}

	sccp_srv.sockfd = socket(sccp_srv.res->ai_family, sccp_srv.res->ai_socktype, sccp_srv.res->ai_protocol);
	setsockopt(sccp_srv.sockfd, SOL_SOCKET, SO_REUSEADDR, &flag_reuse, sizeof(flag_reuse));

	ret = bind(sccp_srv.sockfd, sccp_srv.res->ai_addr, sccp_srv.res->ai_addrlen);
	if (ret == -1) {
		ast_log(LOG_ERROR, "Failed to bind socket: %s\n", strerror(errno));
		return -1;
	}

	ret = listen(sccp_srv.sockfd, SCCP_BACKLOG);
	if (ret == -1) {
		ast_log(LOG_ERROR, "Failed to listen socket: %s\n", strerror(errno));
		return -1;
	}

	sched = sched_context_create();
	if (sched == NULL) {
		ast_log(LOG_ERROR, "Unable to create schedule context\n");
	}

	ast_channel_register(&sccp_tech);
	ast_pthread_create_background(&sccp_srv.thread_accept, NULL, thread_accept, NULL);

	return 0;
}
