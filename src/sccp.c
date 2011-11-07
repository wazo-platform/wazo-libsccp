#include <asterisk.h>
#include <asterisk/causes.h>
#include <asterisk/channel.h>
#include <asterisk/linkedlists.h>
#include <asterisk/poll-compat.h>
#include <asterisk/rtp.h>
#include <asterisk/utils.h>

#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
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

extern struct sccp_configs sccp_cfg; /* global */
static AST_LIST_HEAD_STATIC(list_session, sccp_session);

static struct ast_channel *sccp_request(const char *type, int format, void *data, int *cause);
static int sccp_call(struct ast_channel *ast, char *dest, int timeout);
static int sccp_hangup(struct ast_channel *ast);
static int sccp_answer(struct ast_channel *ast);
static struct ast_frame *sccp_read(struct ast_channel *ast);
static int sccp_write(struct ast_channel *ast, struct ast_frame *frame);
static int sccp_indicate(struct ast_channel *ast, int ind, const void *data, size_t datalen);
static int sccp_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
static int sccp_senddigit_begin(struct ast_channel *ast, char digit);
static int sccp_senddigit_end(struct ast_channel *ast, char digit, unsigned int duration);

static const struct ast_channel_tech sccp_tech = {
	.type = "sccp",
	.description = "Skinny Client Control Protocol",
	.capabilities = ((AST_FORMAT_MAX_AUDIO << 1) - 1),
	.properties = AST_CHAN_TP_WANTSJITTER | AST_CHAN_TP_CREATESJITTER,
	.requester = sccp_request,
	.call = sccp_call,
	.hangup = sccp_hangup,
	.answer = sccp_answer,
	.read = sccp_read,
	.write = sccp_write,
	.indicate = sccp_indicate,
	.fixup = sccp_fixup,
	.send_digit_begin = sccp_senddigit_begin,
	.send_digit_end = sccp_senddigit_end,
};


static int handle_softkey_template_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	msg = msg_alloc(sizeof(struct softkey_template_res_message), SOFTKEY_TEMPLATE_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.softkeytemplate.softKeyOffset = htolel(0);
	msg->data.softkeytemplate.softKeyCount = htolel(sizeof(softkey_template_default) / sizeof(struct softkey_template_definition));
	msg->data.softkeytemplate.totalSoftKeyCount = htolel(sizeof(softkey_template_default) / sizeof(struct softkey_template_definition));
	memcpy(msg->data.softkeytemplate.softKeyTemplateDefinition, softkey_template_default, sizeof(softkey_template_default));

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_config_status_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	msg = msg_alloc(sizeof(struct config_status_res_message), CONFIG_STATUS_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	memcpy(msg->data.configstatus.deviceName, session->device->name, sizeof(msg->data.configstatus.deviceName));
	msg->data.configstatus.stationUserId = htolel(0);
	msg->data.configstatus.stationInstance = htolel(1);
	/*
	memcpy(msg->data.configstatus.userName, "userName", sizeof(msg->data.configstatus.userName));
	memcpy(msg->data.configstatus.serverName, "serverName", sizeof(msg->data.configstatus.serverName));
	*/
	msg->data.configstatus.numberLines = htolel(session->device->line_count);
	msg->data.configstatus.numberSpeedDials = htolel(session->device->speeddial_count);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_time_date_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	time_t now = 0;
	struct tm *cmtime = NULL;
	int ret = 0;

	msg = msg_alloc(sizeof(struct time_date_res_message), DATE_TIME_RES_MESSAGE);
	if (msg == NULL)
		return -1;

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
	msg->data.timedate.systemTime = htolel(0);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_button_template_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct button_definition_template btl[42] = {0};
	int button_count = 0;
	int line_instance = 1;
	int speeddial_instance = 0;
	struct sccp_line *line_itr = NULL;
	int ret = 0;
	int i = 0;

	msg = msg_alloc(sizeof(struct button_template_res_message), BUTTON_TEMPLATE_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	ret = device_get_button_template(session->device, btl);
	if (ret == -1)
		return -1;

	for (i = 0; i < 42; i++) {
		switch (btl[i].buttonDefinition) {
			case BT_CUST_LINESPEEDDIAL:

				msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
				msg->data.buttontemplate.definition[i].instanceNumber = htolel(0);

				AST_LIST_TRAVERSE(&session->device->lines, line_itr, list_per_device) {
					if (line_itr->instance == line_instance) {
						msg->data.buttontemplate.definition[i].buttonDefinition = BT_LINE;
						msg->data.buttontemplate.definition[i].instanceNumber = htolel(line_instance);

						line_instance++;
						button_count++;
					}
				}

				break;

			case BT_NONE:
			default:
				msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
				msg->data.buttontemplate.definition[i].instanceNumber = htolel(0);
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

static int handle_keep_alive_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	msg = msg_alloc(0, KEEP_ALIVE_ACK_MESSAGE);
	if (msg == NULL)
		return -1;

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int register_device(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_device *device_itr = NULL;
	int device_found = 0;

	AST_LIST_TRAVERSE(&list_device, device_itr, list) {

		if (!strcasecmp(device_itr->name, msg->data.reg.name)) {

			if (device_itr->registered == DEVICE_REGISTERED_TRUE) {

				ast_log(LOG_NOTICE, "Device already registered [%s]\n", device_itr->name);
				device_found = -1;

			} else {

				ast_log(LOG_DEBUG, "Device found [%s]\n", device_itr->name);

				device_prepare(device_itr);
				device_register(device_itr,
						letohl(msg->data.reg.protoVersion),
						letohl(msg->data.reg.type),
						session);

				session->device = device_itr;
				device_found = 1;
			}
			break;
		}
	}

	if (device_found == 0)
		ast_log(LOG_DEBUG, "Device not found [%s]\n", device_itr->name);

	return device_found;
}

static struct ast_channel *sccp_new_channel(struct sccp_line *line)
{
	struct ast_channel *channel = NULL;
	int audio_format = 0;

	if (line == NULL)
		return NULL;

	/* FIXME replace hardcoded values */
	channel = ast_channel_alloc(	1,			/* needqueue */
					AST_STATE_DOWN,		/* state */
					line->cid_num,		/* cid_num */
					line->cid_name,		/* cid_name */
					"code",			/* acctcode */
					line->device->exten,	/* exten */
					"default",		/* context */
					0,			/* amaflag */
					"sccp/%s@%s-%d",	/* format */
					line->name,		/* name */
					line->device->name,	/* name */
					1);			/* callnums */

	if (channel == NULL)
		return NULL;

	channel->tech = &sccp_tech;
	channel->tech_pvt = line;
	line->channel = channel;

	/* FIXME ast_codec get mangled */
	channel->nativeformats = 0x10c; //line->device->ast_codec;
	audio_format = ast_best_codec(channel->nativeformats);

	channel->writeformat = audio_format;
	channel->rawwriteformat = audio_format;
	channel->readformat = audio_format;
	channel->rawreadformat = audio_format;

	return channel;
}

/* FIXME doesn't belong here */
static struct sched_context *sched = NULL;
static struct io_conext *io;

static void start_rtp(struct sccp_line *line)
{
	struct sockaddr_in *addr = sccp_srv.res->ai_addr;
	struct ast_codec_pref default_prefs = {0};
	struct sccp_session *session = NULL;
	
	session = line->device->session;
	/* FIXME add option in conf */
	ast_parse_allow_disallow(&default_prefs, &line->device->ast_codec, "", 1);
	line->rtp = ast_rtp_new_with_bindaddr(sched, io, 1, 0, addr->sin_addr);

	line->channel->fds[0] = ast_rtp_fd(line->rtp);
	line->channel->fds[1] = ast_rtcp_fd(line->rtp);

	ast_rtp_setnat(line->rtp, 0);
	ast_rtp_codec_setpref(line->rtp, &default_prefs);

	transmit_connect(line);
}

static void *sccp_newcall(void *data)
{
	struct ast_channel *channel = data;
	struct sccp_line *line = channel->tech_pvt;

	ast_set_callerid(channel,
			line->cid_num,
			line->cid_name,
			NULL);

	channel->lid.lid_num = ast_strdup(channel->exten);
	channel->lid.lid_name = NULL;

	ast_setstate(channel, AST_STATE_RING);

	start_rtp(line);
	ast_pbx_run(channel);

	return NULL;
}

static void *sccp_lookup_exten(void *data)
{
	struct ast_channel *channel = data;
	struct sccp_line *line = channel->tech_pvt;

	size_t len = 0;
	int ret = 0;

	len = strlen(line->device->exten);
	while (line->device->registered == DEVICE_REGISTERED_TRUE &&
		line->state == SCCP_OFFHOOK && len < AST_MAX_EXTENSION-1) {

		//ret = ast_ignore_pattern(channel->context, line->device->exten);
		//ast_log(LOG_NOTICE, "ast ignore pattern : %d\n", ret);

		if (ast_exists_extension(channel, channel->context, line->device->exten, 1, line->cid_num)) {
			if (!ast_matchmore_extension(channel, channel->context, line->device->exten, 1, line->cid_num)) {

				set_line_state(line, SCCP_RINGOUT);
				transmit_callstate(line->device->session, line->instance, SCCP_RINGOUT, line->callid);
				transmit_tone(line->device->session, SCCP_TONE_ALERT, line->instance, 0);
				transmit_callinfo(line->device->session, "", "", line->cid_name, line->cid_num, line->instance, line->callid, 2);
				ast_copy_string(channel->exten, line->device->exten, sizeof(channel->exten));
				sccp_newcall(channel);
				return NULL;
			}
		}
/*
		ret = ast_matchmore_extension(channel, channel->context, line->device->exten, 1, line->cid_num);
		ast_log(LOG_NOTICE, "ast matchmore extension: %d\n", ret);

		ret = ast_canmatch_extension(channel, channel->context, line->device->exten, 1, line->cid_num);
		ast_log(LOG_NOTICE, "ast canmatch extension: %d\n", ret);
*/
		ast_safe_sleep(channel, 500);
		len = strlen(line->device->exten);
	}

	if (channel)
		ast_hangup(channel);

	return NULL;
}

static int handle_offhook_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_device *device = NULL;
	struct sccp_line *line = NULL;
	struct ast_channel *channel = NULL;
	int ret = 0;

	device = session->device;
	line = device_get_active_line(device);

	ast_log(LOG_NOTICE, "handle offhook message line [%d] state [%d] SCCP_RINGIN [%d] ONHOOK [%d]\n", line, line->state, SCCP_RINGIN, SCCP_ONHOOK);

	if (line && line->state == SCCP_RINGIN) {

		ret = transmit_ringer_mode(session, SCCP_RING_OFF);
		ast_queue_control(line->channel, AST_CONTROL_ANSWER);
		transmit_callstate(session, line->instance, SCCP_OFFHOOK, 0);
		transmit_tone(session, SCCP_TONE_NONE, line->instance, 0);
		transmit_callstate(session, line->instance, SCCP_CONNECTED, 0);
		start_rtp(line);
		ast_setstate(line->channel, AST_STATE_UP);
	
		set_line_state(line, SCCP_CONNECTED);

	} else if (line->state == SCCP_ONHOOK) {

		set_line_state(line, SCCP_OFFHOOK);
		channel = sccp_new_channel(line);

		ast_log(LOG_NOTICE, "channel state %d\n", channel->_state);
		ast_setstate(line->channel, AST_STATE_DOWN);

		ret = transmit_lamp_indication(session, 1, line->instance, SCCP_LAMP_ON);
		if (ret == -1)
			return -1;

		ret = transmit_callstate(session, line->instance, SCCP_OFFHOOK, 0);
		if (ret == -1)
			return -1;

		ret = transmit_tone(session, SCCP_TONE_DIAL, line->instance, 0);
		if (ret == -1)
			return -1;
/*
		ret = transmit_displaymessage(session, NULL, line->instance, 0);
		if (ret == -1)
			return -1;
*/
		ret = transmit_selectsoftkeys(session, line->instance, 0, KEYDEF_OFFHOOK);
		if (ret == -1)
			return -1;

		if (ast_pthread_create(&device->lookup_thread, NULL, sccp_lookup_exten, channel)) {
			ast_log(LOG_WARNING, "Unable to create switch thread: %s\n", strerror(errno));
			ast_hangup(channel);
		}
	}
	
	return 0;
}

static int handle_onhook_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_line *line = NULL;
	void **value_ptr;
	int ret = 0;

	line = device_get_active_line(session->device);

	device_release_line(line->device, line);
	set_line_state(line, SCCP_ONHOOK);

	/* reset dialed number */
	line->device->exten[0] = '\0';
	pthread_join(session->device->lookup_thread, value_ptr);

	ret = transmit_callstate(session, line->instance, SCCP_ONHOOK, 0);
	if (ret == -1)
		return -1;

	ret = transmit_selectsoftkeys(session, line->instance, 0, KEYDEF_ONHOOK);
	if (ret == -1)
		return -1;

	if (line->channel != NULL) {

		transmit_close_receive_channel(line);
		transmit_stop_media_transmission(line);

		if (line->rtp) {
			ast_rtp_destroy(line->rtp);
			line->rtp = NULL;
		}

		ast_queue_hangup(line->channel);
		line->channel->tech_pvt = NULL;
		line->channel = NULL;
	}

	return 0;
}

static int handle_softkey_set_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	const struct softkey_definitions *softkeymode = softkey_default_definitions;
	int keyset_count = 0;
	int i = 0;
	int j = 0;
	int ret = 0;

	msg = msg_alloc(sizeof(struct softkey_set_res_message), SOFTKEY_SET_RES_MESSAGE);
	if (msg == NULL)
		return -1;

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
	uint32_t instance = 0;
	int ret = 0;

	instance = letohl(msg->data.forward.lineNumber);
	ast_log(LOG_DEBUG, "Forward status line %d\n", instance);

	msg = msg_alloc(sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.forwardstatus.status = 0;
	msg->data.forwardstatus.lineNumber = htolel(instance);
	
	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

enum sccp_codecs {
	SCCP_CODEC_ALAW = 2,
	SCCP_CODEC_ULAW = 4,
	SCCP_CODEC_G723_1 = 9,
	SCCP_CODEC_G729A = 12,
	SCCP_CODEC_G726_32 = 82,
	SCCP_CODEC_H261 = 100,
	SCCP_CODEC_H263 = 101
};

int codec_ast2sccp(int astcodec)
{
        switch (astcodec) {
        case AST_FORMAT_ALAW:
                return SCCP_CODEC_ALAW;
        case AST_FORMAT_ULAW:
                return SCCP_CODEC_ULAW;
        case AST_FORMAT_G723_1:
                return SCCP_CODEC_G723_1;
        case AST_FORMAT_G729A:
                return SCCP_CODEC_G729A;
        case AST_FORMAT_G726_AAL2: /* XXX Is this right? */
                return SCCP_CODEC_G726_32;
        case AST_FORMAT_H261:
                return SCCP_CODEC_H261;
        case AST_FORMAT_H263:
                return SCCP_CODEC_H263;
        default:
                return 0;
        }
}

static int codec_sccp2ast(enum sccp_codecs sccp_codec)
{
	switch (sccp_codec) {
	case SCCP_CODEC_ALAW:
		return AST_FORMAT_ALAW;
	case SCCP_CODEC_ULAW:
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
		return 0;
	}
}

static int handle_capabilities_res_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int count = 0;
	int ast_codec = 0;
	int sccp_codec = 0;
	int i = 0;

	count = letohl(msg->data.caps.count);
	ast_log(LOG_DEBUG, "Received %d capabilities\n", count);

	if (count > SCCP_MAX_CAPABILITIES) {
		count = SCCP_MAX_CAPABILITIES;
		ast_log(LOG_WARNING, "Received more capabilities (%d) than we can handle (%d)\n", count, SCCP_MAX_CAPABILITIES);
	}

	for (i = 0; i < count; i++) {
		sccp_codec = letohl(msg->data.caps.caps[i].codec);
		ast_codec |= codec_sccp2ast(sccp_codec);
	}

	session->device->ast_codec = ast_codec;

	return 0;
}

static int handle_open_receive_channel_ack_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_line *line = NULL;
	struct ast_format_list fmt = {0};
	struct sockaddr_in sin = {0};
	struct sockaddr_in us = {0};
	uint32_t addr = 0;
	uint32_t port = 0;
	uint32_t passthruid = 0;
	int ret = 0;

	line = device_get_active_line(session->device);
/*
	if (line->eevice->protoVersion >= 17) {
		addr = letohl(msg->data.openreceivechannelack_v17.ipAddr[0]);
		port = letohl(msg->data.openreceivechannelack_v17.port);
		passthruid = letohl(msg->data.openreceivechannelack_v17.passThruId);

	} else {
*/
		addr = letohl(msg->data.openreceivechannelack.ipAddr);
		port = letohl(msg->data.openreceivechannelack.port);
		passthruid = letohl(msg->data.openreceivechannelack.passThruId);
//	}

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = addr;
	sin.sin_port = htons(port);

	ast_rtp_set_peer(line->rtp, &sin);
	ast_rtp_get_us(line->rtp, &us);

	/* FIXME `fmt' per device */
	struct ast_codec_pref default_prefs;
	fmt = ast_codec_pref_getsize(&default_prefs, ast_best_codec(line->device->ast_codec));
/*
	if (line->device->protoVersion >= 17) {

		msg = msg_alloc(sizeof(struct start_media_transmission_message_v17), START_MEDIA_TRANSMISSION_MESSAGE);
		if (msg == NULL)
			return -1;

		msg->data.startmedia_v17.conferenceId = htolel(0);
		msg->data.startmedia_v17.passThruPartyId = htolel(0);
		msg->data.startmedia_v17.conferenceId1 = htolel(0);
		msg->data.startmedia_v17.remoteIp[0] = htolel(us.sin_addr.s_addr);
		msg->data.startmedia_v17.remotePort = htolel(ntohs(us.sin_port));
		msg->data.startmedia_v17.packetSize = htolel(fmt.cur_ms);
		msg->data.startmedia_v17.payloadType = htolel(codec_ast2sccp(fmt.bits));
		msg->data.startmedia_v17.qualifier.precedence = htolel(127);
		msg->data.startmedia_v17.qualifier.vad = htolel(0);
		msg->data.startmedia_v17.qualifier.packets = htolel(0);
		msg->data.startmedia_v17.qualifier.bitRate = htolel(0);

	} else {
*/
		msg = msg_alloc(sizeof(struct start_media_transmission_message), START_MEDIA_TRANSMISSION_MESSAGE);
		if (msg == NULL)
			return -1;

		msg->data.startmedia.conferenceId = htolel(0);
		msg->data.startmedia.passThruPartyId = htolel(line->callid ^ 0xFFFFFFFF);
		msg->data.startmedia.remoteIp = htolel(us.sin_addr.s_addr);
		msg->data.startmedia.remotePort = htolel(ntohs(us.sin_port));
		msg->data.startmedia.packetSize = htolel(fmt.cur_ms);
		msg->data.startmedia.payloadType = htolel(codec_ast2sccp(fmt.bits));
		msg->data.startmedia.qualifier.precedence = htolel(127);
		msg->data.startmedia.qualifier.vad = htolel(0);
		msg->data.startmedia.qualifier.packets = htolel(0);
		msg->data.startmedia.qualifier.bitRate = htolel(0);
		msg->data.startmedia.conferenceId1 = htolel(0);
		msg->data.startmedia.rtpTimeout = htolel(10);
//	}

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

static int handle_line_status_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int line_instance;
	struct sccp_line *line;
	int ret = 0;

	line_instance = letohl(msg->data.line.lineNumber);

	line = device_get_line(session->device, line_instance);
	if (line == NULL) {
		ast_log(LOG_ERROR, "Line instance [%d] is not attached to device [%s]\n", line_instance, session->device->name);
		return -1;
	}

	msg = msg_alloc(sizeof(struct line_status_res_message), LINE_STATUS_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.linestatus.lineNumber = letohl(line_instance);

	memcpy(msg->data.linestatus.lineDirNumber, line->name, sizeof(msg->data.linestatus.lineDirNumber));
	memcpy(msg->data.linestatus.lineDisplayName, session->device->name, sizeof(msg->data.linestatus.lineDisplayName));
	memcpy(msg->data.linestatus.lineDisplayAlias, line->name, sizeof(msg->data.linestatus.lineDisplayAlias));

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	/* foward stat */
	msg = msg_alloc(sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);
	if (msg == NULL)
		return -1;

	msg->data.forwardstatus.status = 0;
	msg->data.forwardstatus.lineNumber = htolel(line_instance);
	
	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0; 
}

static int handle_register_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	ast_log(LOG_NOTICE, "name %s\n", msg->data.reg.name);
	ast_log(LOG_NOTICE, "userId %d\n", msg->data.reg.userId);
	ast_log(LOG_NOTICE, "instance %d\n", msg->data.reg.instance);
	ast_log(LOG_NOTICE, "ip %d\n", msg->data.reg.ip);
	ast_log(LOG_NOTICE, "type %d\n", msg->data.reg.type);
	ast_log(LOG_NOTICE, "maxStreams %d\n", msg->data.reg.maxStreams);
	ast_log(LOG_NOTICE, "protoVersion %d\n", msg->data.reg.protoVersion);

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
			return -1;
		}

		snprintf(msg->data.regrej.errMsg, sizeof(msg->data.regrej.errMsg), "Access denied: %s\n", msg->data.reg.name);
		ret = transmit_message(msg, session);
		if (ret == -1)
			return -1;

		return 0;
	}

	msg = msg_alloc(sizeof(struct register_ack_message), REGISTER_ACK_MESSAGE);
	if (msg == NULL) {
		return -1;
	}

        msg->data.regack.keepAlive = htolel(sccp_cfg.keepalive);
        memcpy(msg->data.regack.dateTemplate, sccp_cfg.dateformat, sizeof(msg->data.regack.dateTemplate));

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

        msg->data.regack.secondaryKeepAlive = htolel(sccp_cfg.keepalive);

        ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	msg = msg_alloc(0, CAPABILITIES_REQ_MESSAGE);
	if (msg == NULL) {
		return -1;
	}

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1; 

	return 0;
}

static int handle_ipport_message(struct sccp_msg *msg, struct sccp_session *session)
{
	session->device->station_port = msg->data.ipport.stationIpPort;
	return 0;
}

static int handle_keypad_button_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_line *line = NULL;
	struct ast_frame frame = { AST_FRAME_DTMF, };

	char digit;
	int button;
	int instance;
	int callId;

	size_t len;

	button = letohl(msg->data.keypad.button);
	instance = letohl(msg->data.keypad.instance);
	callId = letohl(msg->data.keypad.callId);

	line = device_get_line(session->device, instance);

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

		frame.subclass = digit;
		frame.src = "sccp";
		frame.len = 100;
		frame.offset = 0;
		frame.datalen = 0;
		frame.data = NULL;

		ast_queue_frame(line->channel, &frame);

	} else if (line->state == SCCP_OFFHOOK) {

		len = strlen(line->device->exten);
		if (len < sizeof(line->device->exten) - 1) {
			line->device->exten[len] = digit;
			line->device->exten[len+1] = '\0';
		}

		transmit_tone(session, SCCP_TONE_NONE, line->instance, 0);
	}

	return 0;
}

static void destroy_session(struct sccp_session **session)
{
	ast_mutex_destroy(&(*session)->lock);
	ast_free((*session)->ipaddr);
	close((*session)->sockfd);
	ast_free(*session);
}

static int handle_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret = 0;

	/* Prevent unregistered phone of sending non-registering messages */
	if ((session->device == NULL ||
		(session->device != NULL && session->device->registered == DEVICE_REGISTERED_FALSE)) &&
		(msg->id != REGISTER_MESSAGE && msg->id != ALARM_MESSAGE)) {

			ast_log(LOG_ERROR, "session from [%s::%d] sending non-registering messages\n",
						session->ipaddr, session->sockfd);
			return -1;
	}

	switch (msg->id) {

		case KEEP_ALIVE_MESSAGE:
			ast_log(LOG_NOTICE, "Keep alive message\n");
			ret = handle_keep_alive_message(msg, session);
			break;

		case REGISTER_MESSAGE:
			ast_log(LOG_NOTICE, "Register message\n");
			ret = handle_register_message(msg, session);
			break;

		case IP_PORT_MESSAGE:
			ast_log(LOG_NOTICE, "Ip port message\n");
			ret = handle_ipport_message(msg, session);
			break;

		case KEYPAD_BUTTON_MESSAGE:
			ast_log(LOG_NOTICE, "keypad button message\n");
			ret = handle_keypad_button_message(msg, session);
			break;

		case OFFHOOK_MESSAGE:
			ast_log(LOG_NOTICE, "Offhook message\n");
			ret = handle_offhook_message(msg, session);
			break;

		case ONHOOK_MESSAGE:
			ast_log(LOG_NOTICE, "Onhook message\n");
			ret = handle_onhook_message(msg, session);
			break;

		case FORWARD_STATUS_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Forward status message\n");
			ret = handle_forward_status_req_message(msg, session);
			break;

		case CAPABILITIES_RES_MESSAGE:
			ast_log(LOG_NOTICE, "Capabilities message\n");
			ret = handle_capabilities_res_message(msg, session);
			break;

		case LINE_STATUS_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Line status message\n");
			ret = handle_line_status_req_message(msg, session);
			break;

		case CONFIG_STATUS_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Config status message\n");
			ret = handle_config_status_req_message(msg, session);
			break;

		case TIME_DATE_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Time date message\n");
			ret = handle_time_date_req_message(msg, session);
			break;

		case BUTTON_TEMPLATE_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Button template request message\n");
			ret = handle_button_template_req_message(msg, session);
			break;

		case SOFTKEY_TEMPLATE_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Softkey template request message\n");
			ret = handle_softkey_template_req_message(msg, session);
			break;

		case ALARM_MESSAGE:
			ast_log(LOG_NOTICE, "Alarm message: %s\n", msg->data.alarm.displayMessage);
			break;

		case OPEN_RECEIVE_CHANNEL_ACK_MESSAGE:
			ast_log(LOG_NOTICE, "Open receive channel ack message\n");
			ret = handle_open_receive_channel_ack_message(msg, session);
			break;

		case SOFTKEY_SET_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Softkey set request message\n");
			ret = handle_softkey_set_req_message(msg, session);
			break;

		case REGISTER_AVAILABLE_LINES_MESSAGE:
			ast_log(LOG_NOTICE, "Register available lines message\n");
			break;

		case START_MEDIA_TRANSMISSION_ACK_MESSAGE:
			ast_log(LOG_NOTICE, "Start media transmission ack message\n");
			break;

		case ACCESSORY_STATUS_MESSAGE:
			break;

		default:
			ast_log(LOG_NOTICE, "Unknown message %x\n", msg->id);
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
	
	time(&now);
	/* if no device or device is not registered and time has elapsed */
	if ((session->device == NULL || (session->device != NULL && session->device->registered == DEVICE_REGISTERED_FALSE))
		&& now > session->start_time + sccp_cfg.authtimeout) {
		ast_log(LOG_WARNING, "Time has elapsed [%d]\n", sccp_cfg.authtimeout);
		return -1;
	}

	fds[0].fd = session->sockfd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	/* wait N times the keepalive frequence */
	nfds = ast_poll(fds, 1, sccp_cfg.keepalive * 1000 * 2);
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
		ast_log(LOG_DEBUG, "nbyte %d\n", nbyte);
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
		ast_log(LOG_DEBUG, "msg_len %d\n", msg_len);
		if (msg_len > SCCP_MAX_PACKET_SZ || msg_len < 0) {
			ast_log(LOG_WARNING, "Packet length is out of bounds: 0 > %d > %d\n", msg_len, SCCP_MAX_PACKET_SZ);
			return -1;
		}

		/* bypass the length field and fetch the payload */
		nbyte = read(session->sockfd, session->inbuf+4, msg_len+4);
		ast_log(LOG_DEBUG, "nbyte %d\n", nbyte);
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
	struct sccp_session *session = data;
	struct sccp_msg *msg = NULL;
	int connected = 1;
	int ret = 0;

	while (connected) {

		ret = fetch_data(session);
		if (ret > 0) {
			msg = (struct sccp_msg *)session->inbuf;
			ret = handle_message(msg, session);
			/* prevent flooding */
			usleep(100000);
		}

		if (ret == -1) {
			AST_LIST_LOCK(&list_session);
			session = AST_LIST_REMOVE(&list_session, session, list);
			AST_LIST_UNLOCK(&list_session);	

			if (session->device) {
				ast_log(LOG_ERROR, "Disconnecting device [%s]\n", session->device->name);
				device_unregister(session->device);
			}

			connected = 0;
		}
	}

	if (session)
		destroy_session(&session);

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
			close(new_sockfd);
			continue;
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
}

static struct ast_channel *sccp_request(const char *type, int format, void *destination, int *cause)
{
	ast_log(LOG_NOTICE, "sccp request\n");

	struct sccp_line *line = NULL;
	struct ast_channel *channel = NULL;

	ast_log(LOG_NOTICE, "type: %s\n"
				"format: %d\n"
				"destination: %s\n"
				"cause: %d\n",
				type, format, (char *)destination, *cause);

	line = find_line_by_name((char *)destination);

	if (line == NULL) {
		ast_log(LOG_NOTICE, "This line doesn't exist: %s\n", (char *)destination);
		*cause = AST_CAUSE_UNREGISTERED;
		return NULL;
	}

	if (line->state != SCCP_ONHOOK) {
		*cause = AST_CAUSE_BUSY;
		return NULL;
	}

	channel = sccp_new_channel(line);
	ast_setstate(line->channel, AST_STATE_DOWN);

	return channel;
}

static int sccp_call(struct ast_channel *channel, char *dest, int timeout)
{
	ast_log(LOG_NOTICE, "sccp call\n");

	struct sccp_line *line = NULL;
	struct sccp_device *device = NULL;
	struct sccp_session *session = NULL;

	ast_log(LOG_NOTICE, "channel->lid.lid_name: %s\n", channel->lid.lid_name);
	ast_log(LOG_NOTICE, "channel->lid.lid_num: %s\n", channel->lid.lid_num);

	line = channel->tech_pvt;
	if (line == NULL)
		return -1;

	device = line->device;
	session = device->session;

	if (line->state != SCCP_ONHOOK) {
		channel->hangupcause = AST_CONTROL_BUSY;
		ast_setstate(channel, AST_CONTROL_BUSY);
		ast_queue_control(channel, AST_CONTROL_BUSY);
		return 0;
	}

	device_enqueue_line(device, line);
	line->channel = channel;

	transmit_callstate(session, line->instance, SCCP_RINGIN, line->callid);
	transmit_selectsoftkeys(session, line->instance, line->callid, KEYDEF_RINGIN);
//	transmit_displaypromptstatus(session, "Ring-In", 0, line->instance, line->callid);
	transmit_callinfo(session, channel->lid.lid_name, channel->lid.lid_num, line->cid_name, line->cid_num, line->instance, line->callid, 1);
	transmit_lamp_indication(session, STIMULUS_LINE, line->instance, SCCP_LAMP_BLINK);

	if (device->active_line == NULL)
		transmit_ringer_mode(session, SCCP_RING_INSIDE);

	set_line_state(line, SCCP_RINGIN);
	ast_setstate(channel, AST_STATE_RINGING);
	ast_queue_control(channel, AST_CONTROL_RINGING);

	return 0;
}

static int sccp_hangup(struct ast_channel *channel)
{
	ast_log(LOG_NOTICE, "sccp hangup\n");

	struct sccp_line *line = NULL;
	int ret = 0;

	line = channel->tech_pvt;
	if (line == NULL)
		return -1;

	if (line->state == SCCP_RINGIN) {

		ret = transmit_lamp_indication(line->device->session, 1, line->instance, SCCP_LAMP_OFF);
		if (ret == -1)
			return -1;

		ret = transmit_callstate(line->device->session, line->instance, SCCP_ONHOOK, line->callid);
		if (ret == -1)
			return -1;

		ret = transmit_tone(line->device->session, SCCP_TONE_NONE, line->instance, 0);
		if (ret == -1)
			return -1;

		ret = transmit_selectsoftkeys(line->device->session, line->instance, 0, KEYDEF_ONHOOK);
		if (ret == -1)
			return -1;
	}

	if (line->device->active_line_cnt <= 1)
		ret = transmit_ringer_mode(line->device->session, SCCP_RING_OFF);

	if (line->channel != NULL) {

		transmit_close_receive_channel(line);
		transmit_stop_media_transmission(line);

		if (line->rtp) {
			ast_rtp_destroy(line->rtp);
			line->rtp = NULL;
		}

		ast_queue_hangup(line->channel);
		line->channel = NULL;
		channel->tech_pvt = NULL;
	}

	if (line->state == SCCP_RINGIN) {
		device_release_line(line->device, line);
		set_line_state(line, SCCP_ONHOOK);
	} else if (line->state == SCCP_CONNECTED) {
		set_line_state(line, SCCP_INVALID);
	}

	return 0;
}

static int sccp_answer(struct ast_channel *channel)
{
	ast_log(LOG_NOTICE, "sccp answer\n");

	struct sccp_line *line = NULL;
	line = channel->tech_pvt;

	if (line->rtp == NULL)
		start_rtp(line);

	transmit_tone(line->device->session, SCCP_TONE_NONE, line->instance, 0);
	ast_setstate(channel, AST_STATE_UP);
	set_line_state(line, SCCP_CONNECTED);

	return 0;
}

static struct ast_frame *sccp_read(struct ast_channel *channel)
{
	struct ast_frame *frame = NULL;
	struct sccp_line *line = NULL;

	/* XXX check all frame type */
	line = channel->tech_pvt;
	
	switch (channel->fdno) {
	case 0:
		frame = ast_rtp_read(line->rtp);
		break;

	case 1:
		frame = ast_rtcp_read(line->rtp);
		break;

	default:
		frame = &ast_null_frame;
	}

	if (frame->frametype == AST_FRAME_VOICE) {
		if (frame->subclass != channel->nativeformats) {
			channel->nativeformats = frame->subclass;
			ast_set_read_format(channel, channel->readformat);
			ast_set_write_format(channel, channel->writeformat);
		}
	}

	return frame;
}

static int sccp_write(struct ast_channel *channel, struct ast_frame *frame)
{
	int res = 0;
	struct sccp_line *line = channel->tech_pvt;

	if (line->state == SCCP_CONNECTED) {
		res = ast_rtp_write(line->rtp, frame);
	}

	return res;
}

static int sccp_indicate(struct ast_channel *ast, int ind, const void *data, size_t datalen)
{
	ast_log(LOG_NOTICE, "sccp indicate\n");
	return 0;
}

static int sccp_fixup(struct ast_channel *oldchannel, struct ast_channel *newchannel)
{
	ast_log(LOG_NOTICE, "sccp fixup\n");
	struct sccp_line *line = newchannel->tech_pvt;
	line->channel = newchannel;
	return 0;
}

static int sccp_senddigit_begin(struct ast_channel *ast, char digit)
{
	ast_log(LOG_NOTICE, "sccp senddigit begin\n");
	return 0;
}

static int sccp_senddigit_end(struct ast_channel *ast, char digit, unsigned int duration)
{
	ast_log(LOG_NOTICE, "sccp senddigit end\n");
	return 0;
}

void sccp_server_fini()
{
	struct sccp_session *session_itr = NULL;
	int ret;

	/* FIXME doesn't belong here */
	ast_channel_unregister(&sccp_tech);

	ret = pthread_cancel(sccp_srv.thread_accept);
	ret = pthread_kill(sccp_srv.thread_accept, SIGURG);
	ret = pthread_join(sccp_srv.thread_accept, NULL);

	AST_LIST_TRAVERSE_SAFE_BEGIN(&list_session, session_itr, list) {
		if (session_itr != NULL) {

			ast_log(LOG_NOTICE, "Session del %s\n", session_itr->ipaddr);
			AST_LIST_REMOVE_CURRENT(&list_session, list);

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

int sccp_server_init(void)
{
	struct addrinfo hints = {0};
	const int flag_reuse = 1;
	int ret = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST;

	getaddrinfo(sccp_cfg.bindaddr, SCCP_PORT, &hints, &sccp_srv.res);

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

	/* FIXME doesn't belong here */
	ast_channel_register(&sccp_tech);

	ast_pthread_create_background(&sccp_srv.thread_accept, NULL, thread_accept, NULL);

	return 0;
}
