#include <errno.h>
#include <string.h>

#include <asterisk.h>
#include <asterisk/localtime.h>
#include <asterisk/time.h>
#include <asterisk/utils.h>

#include "sccp.h"
#include "sccp_debug.h"
#include "sccp_device.h"
#include "sccp_line.h"
#include "sccp_message.h"
#include "sccp_utils.h"

#include "../config.h"

static struct sccp_msg *msg_alloc(size_t data_length, uint32_t msg_id);
static int transmit_message(struct sccp_msg *msg, struct sccp_session *session);
static int transmit_empty_message(uint32_t msg_id, struct sccp_session *session);

const char *msg_id_str(uint32_t msg_id) {
	switch (msg_id) {
	case KEEP_ALIVE_MESSAGE:
		return "keep alive";
	case REGISTER_MESSAGE:
		return "register";
	case IP_PORT_MESSAGE:
		return "ip port";
	case ENBLOC_CALL_MESSAGE:
		return "enbloc call";
	case KEYPAD_BUTTON_MESSAGE:
		return "keypad button";
	case STIMULUS_MESSAGE:
		return "stimulus";
	case OFFHOOK_MESSAGE:
		return "offhook";
	case ONHOOK_MESSAGE:
		return "onhook";
	case FORWARD_STATUS_REQ_MESSAGE:
		return "forward status req";
	case SPEEDDIAL_STAT_REQ_MESSAGE:
		return "speeddial stat req";
	case LINE_STATUS_REQ_MESSAGE:
		return "line status req";
	case CONFIG_STATUS_REQ_MESSAGE:
		return "config status req";
	case TIME_DATE_REQ_MESSAGE:
		return "time date req";
	case BUTTON_TEMPLATE_REQ_MESSAGE:
		return "button template req";
	case VERSION_REQ_MESSAGE:
		return "version req";
	case CAPABILITIES_RES_MESSAGE:
		return "capabilities res";
	case ALARM_MESSAGE:
		return "alarm";
	case OPEN_RECEIVE_CHANNEL_ACK_MESSAGE:
		return "open receive channel ack";
	case SOFTKEY_SET_REQ_MESSAGE:
		return "softkey set req";
	case SOFTKEY_EVENT_MESSAGE:
		return "softkey event";
	case UNREGISTER_MESSAGE:
		return "unregister";
	case SOFTKEY_TEMPLATE_REQ_MESSAGE:
		return "softkey template req";
	case REGISTER_AVAILABLE_LINES_MESSAGE:
		return "register available lines";
	case FEATURE_STATUS_REQ_MESSAGE:
		return "feature status req";
	case ACCESSORY_STATUS_MESSAGE:
		return "accessory status";
	case REGISTER_ACK_MESSAGE:
		return "register ack";
	case START_TONE_MESSAGE:
		return "start tone";
	case STOP_TONE_MESSAGE:
		return "stop tone";
	case SET_RINGER_MESSAGE:
		return "set ringer";
	case SET_LAMP_MESSAGE:
		return "set lamp";
	case SET_SPEAKER_MESSAGE:
		return "set speaker";
	case STOP_MEDIA_TRANSMISSION_MESSAGE:
		return "stop media transmission";
	case START_MEDIA_TRANSMISSION_MESSAGE:
		return "start media transmission";
	case CALL_INFO_MESSAGE:
		return "call info";
	case FORWARD_STATUS_RES_MESSAGE:
		return "forward status res";
	case SPEEDDIAL_STAT_RES_MESSAGE:
		return "speeddial stat res";
	case LINE_STATUS_RES_MESSAGE:
		return "line status res";
	case CONFIG_STATUS_RES_MESSAGE:
		return "config status res";
	case DATE_TIME_RES_MESSAGE:
		return "date time res";
	case BUTTON_TEMPLATE_RES_MESSAGE:
		return "button template res";
	case VERSION_RES_MESSAGE:
		return "version res";
	case CAPABILITIES_REQ_MESSAGE:
		return "capabilities req";
	case REGISTER_REJ_MESSAGE:
		return "register rej";
	case RESET_MESSAGE:
		return "reset";
	case KEEP_ALIVE_ACK_MESSAGE:
		return "keep alive ack";
	case OPEN_RECEIVE_CHANNEL_MESSAGE:
		return "open receive channel";
	case CLOSE_RECEIVE_CHANNEL_MESSAGE:
		return "close receive channel";
	case SOFTKEY_TEMPLATE_RES_MESSAGE:
		return "softkey template res";
	case SOFTKEY_SET_RES_MESSAGE:
		return "softkey set res";
	case SELECT_SOFT_KEYS_MESSAGE:
		return "select soft keys";
	case CALL_STATE_MESSAGE:
		return "call state";
	case DISPLAY_NOTIFY_MESSAGE:
		return "display notify";
	case CLEAR_NOTIFY_MESSAGE:
		return "clear notify";
	case ACTIVATE_CALL_PLANE_MESSAGE:
		return "activate call plane";
	case DIALED_NUMBER_MESSAGE:
		return "dialed number";
	case FEATURE_STAT_MESSAGE:
		return "feature stat";
	case START_MEDIA_TRANSMISSION_ACK_MESSAGE:
		return "start media transmission ack";
	default:
		return "unknown";
	}
}

static struct sccp_msg *msg_alloc(size_t data_length, uint32_t msg_id)
{
	struct sccp_msg *msg;

	msg = ast_calloc(1, 12 + 4 + data_length);
	if (!msg) {
		ast_log(LOG_ERROR, "msg allocation failed\n");
		return NULL;
	}

	msg->length = htolel(4 + data_length);
	msg->reserved = htolel(0);
	msg->id = htolel(msg_id);

	return msg;
}

static int transmit_message(struct sccp_msg *msg, struct sccp_session *session)
{
	ssize_t nbyte;

	if (!session) {
		ast_log(LOG_WARNING, "could not transmit message: session is NULL\n");
		goto end;
	}

	if (session->transmit_error) {
		ast_log(LOG_DEBUG, "not sending msg because last transmit failed\n");
		goto end;
	}

	if (!msg) {
		ast_log(LOG_WARNING, "could not transmit message: msg is NULL\n");
		session->transmit_error = -1;
		goto end;
	}

	if (sccp_debug) {
		if (*sccp_debug_addr == '\0' || !strcmp(sccp_debug_addr, session->ipaddr)) {
			sccp_dump_message_transmitting(session, msg);
		}
	}

	memcpy(session->outbuf, msg, 12);
	memcpy(session->outbuf+12, &msg->data, letohl(msg->length));

	nbyte = write(session->sockfd, session->outbuf, letohl(msg->length)+8);
	if (nbyte == -1) {
		session->transmit_error = -1;
		ast_log(LOG_WARNING, "message transmit failed: %s\n", strerror(errno));
	}

end:
	ast_free(msg);

	return session ? session->transmit_error : -1;
}

static int transmit_empty_message(uint32_t msg_id, struct sccp_session *session)
{
	return transmit_message(msg_alloc(0, msg_id), session);
}

int transmit_button_template_res(struct sccp_session *session)
{
	int i;
	int button_set;
	int active_button_count = 0;
	int device_button_count;
	uint32_t line_instance = 1;
	struct sccp_msg *msg;
	struct sccp_line *line_itr;
	struct sccp_speeddial *speeddial_itr;

	if (!session) {
		return -1;
	}

	msg = msg_alloc(sizeof(struct button_template_res_message), BUTTON_TEMPLATE_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	device_button_count = device_get_button_count(session->device);
	if (device_button_count == -1) {
		return -1;
	}

	for (i = 0; i < device_button_count; i++) {
		button_set = 0;

		msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
		msg->data.buttontemplate.definition[i].lineInstance = htolel(line_instance);

		AST_RWLIST_RDLOCK(&session->device->lines);
		AST_RWLIST_TRAVERSE(&session->device->lines, line_itr, list_per_device) {
			if (line_itr->instance == line_instance) {
				msg->data.buttontemplate.definition[i].buttonDefinition = BT_LINE;
				msg->data.buttontemplate.definition[i].lineInstance = htolel(line_instance);

				line_instance++;
				active_button_count++;
				button_set = 1;
			}
		}
		AST_RWLIST_UNLOCK(&session->device->lines);

		if (button_set != 1) {
			AST_RWLIST_RDLOCK(&session->device->speeddials);
			AST_RWLIST_TRAVERSE(&session->device->speeddials, speeddial_itr, list_per_device) {
				if (speeddial_itr->instance == line_instance) {
					msg->data.buttontemplate.definition[i].buttonDefinition = STIMULUS_FEATUREBUTTON;
					msg->data.buttontemplate.definition[i].lineInstance = htolel(line_instance);

					line_instance++;
					active_button_count++;
				}
			}
			AST_RWLIST_UNLOCK(&session->device->speeddials);
		}
	}

	for (; i < 42; i++) {
		msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
		msg->data.buttontemplate.definition[i].lineInstance = htolel(0);
	}

	msg->data.buttontemplate.buttonOffset = htolel(0);
	msg->data.buttontemplate.buttonCount = htolel(active_button_count);
	msg->data.buttontemplate.totalButtonCount = htolel(active_button_count);

	return transmit_message(msg, session);
}

int transmit_callinfo(struct sccp_session *session, const char *from_name, const char *from_num,
			const char *to_name, const char *to_num, int line_instance, int callid, enum sccp_direction direction)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct call_info_message), CALL_INFO_MESSAGE);
	if (!msg) {
		return -1;
	}

	if (from_name) {
		ast_copy_string(msg->data.callinfo.callingPartyName, from_name, sizeof(msg->data.callinfo.callingPartyName));
	}

	if (from_num) {
		ast_copy_string(msg->data.callinfo.callingParty, from_num, sizeof(msg->data.callinfo.callingParty));
	}

	if (to_name) {
		ast_copy_string(msg->data.callinfo.calledPartyName, to_name, sizeof(msg->data.callinfo.calledPartyName));
		ast_copy_string(msg->data.callinfo.originalCalledPartyName, to_name, sizeof(msg->data.callinfo.originalCalledPartyName));
	}

	if (to_num) {
		ast_copy_string(msg->data.callinfo.calledParty, to_num, sizeof(msg->data.callinfo.calledParty));
		ast_copy_string(msg->data.callinfo.originalCalledParty, to_num, sizeof(msg->data.callinfo.originalCalledParty));
	}

	msg->data.callinfo.lineInstance = htolel(line_instance);
	msg->data.callinfo.callInstance = htolel(callid);
	msg->data.callinfo.type = htolel(direction);

	return transmit_message(msg, session);
}

int transmit_callstate(struct sccp_session *session, int line_instance, enum sccp_state state, uint32_t callid)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct call_state_message), CALL_STATE_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.callstate.callState = htolel(state);
	msg->data.callstate.lineInstance = htolel(line_instance);
	msg->data.callstate.callReference = htolel(callid);
	msg->data.callstate.visibility = htolel(0);
	msg->data.callstate.priority = htolel(4);

	return transmit_message(msg, session);
}

int transmit_capabilities_req(struct sccp_session *session)
{
	return transmit_empty_message(CAPABILITIES_REQ_MESSAGE, session);
}

int transmit_clearmessage(struct sccp_session *session)
{
	return transmit_empty_message(CLEAR_NOTIFY_MESSAGE, session);
}

int transmit_close_receive_channel(struct sccp_session *session, uint32_t callid)
{
	struct sccp_msg *msg = msg_alloc(sizeof(struct close_receive_channel_message), CLOSE_RECEIVE_CHANNEL_MESSAGE);

	if (msg == NULL) {
		return -1;
	}

	msg->data.closereceivechannel.conferenceId = htolel(callid);
	msg->data.closereceivechannel.partyId = htolel(callid ^ 0xFFFFFFFF);
	msg->data.closereceivechannel.conferenceId1 = htolel(callid);

	return transmit_message(msg, session);
}

int transmit_config_status_res(struct sccp_session *session)
{
	struct sccp_msg *msg;

	if (!session) {
		return -1;
	}

	msg = msg_alloc(sizeof(struct config_status_res_message), CONFIG_STATUS_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	memcpy(msg->data.configstatus.deviceName, session->device->name, sizeof(msg->data.configstatus.deviceName));
	msg->data.configstatus.stationUserId = htolel(0);
	msg->data.configstatus.stationInstance = htolel(1);
	msg->data.configstatus.numberLines = htolel(session->device->line_count);
	msg->data.configstatus.numberSpeedDials = htolel(session->device->speeddial_count);

	return transmit_message(msg, session);
}

int transmit_open_receive_channel(struct sccp_session *session, struct sccp_subchannel *subchan)
{
	struct ast_format_list fmt;
	struct sccp_msg *msg;
	struct sccp_device *device;

	if (!subchan) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return -1;
	}

	device = subchan->line->device;
	if (!device) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}
	if (device->open_receive_channel_pending == 1) {
		ast_debug(1, "open_receive_channel already sent\n");
		return 0;
	}
	device->open_receive_channel_pending = 1;

	fmt = ast_codec_pref_getsize(&subchan->line->codec_pref, &subchan->fmt);

	msg = msg_alloc(sizeof(struct open_receive_channel_message), OPEN_RECEIVE_CHANNEL_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.openreceivechannel.conferenceId = htolel(subchan->id);
	msg->data.openreceivechannel.partyId = htolel(subchan->id ^ 0xFFFFFFFF);
	msg->data.openreceivechannel.packets = htolel(fmt.cur_ms);
	msg->data.openreceivechannel.capability = htolel(codec_ast2sccp(&fmt.format));
	msg->data.openreceivechannel.echo = htolel(0);
	msg->data.openreceivechannel.bitrate = htolel(0);
	msg->data.openreceivechannel.conferenceId1 = htolel(subchan->id);
	msg->data.openreceivechannel.rtpTimeout = htolel(10);

	return transmit_message(msg, session);
}

int transmit_dialed_number(struct sccp_session *session, const char *extension, int line_instance, int callid)
{
	struct sccp_msg *msg;

	if (!extension) {
		ast_log(LOG_DEBUG, "extension is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct dialed_number_message), DIALED_NUMBER_MESSAGE);
	if (!msg) {
		return -1;
	}

	ast_copy_string(msg->data.dialednumber.calledParty, extension, sizeof(msg->data.dialednumber.calledParty));
	msg->data.dialednumber.lineInstance = htolel(line_instance);
	msg->data.dialednumber.callInstance = htolel(callid);

	return transmit_message(msg, session);
}

int transmit_displaymessage(struct sccp_session *session, const char *text)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct display_notify_message), DISPLAY_NOTIFY_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.notify.displayTimeout = htolel(0);
	ast_copy_string(msg->data.notify.displayMessage, text, sizeof(msg->data.notify.displayMessage));

	return transmit_message(msg, session);
}

int transmit_feature_status(struct sccp_session *session, int instance, enum sccp_button_type type, enum sccp_blf_status status, const char *label)
{
	struct sccp_msg *msg;

	if (!label) {
		ast_log(LOG_DEBUG, "label is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct feature_stat_message), FEATURE_STAT_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.featurestatus.bt_instance = htolel(instance);
	msg->data.featurestatus.type = htolel(type);
	msg->data.featurestatus.status = htolel(status);
	ast_copy_string(msg->data.featurestatus.label, label, sizeof(msg->data.featurestatus.label));

	return transmit_message(msg, session);
}

int transmit_forward_status_message(struct sccp_session *session, int line_instance, const char *extension, int status)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.forwardstatus.status = htolel(status);
	msg->data.forwardstatus.lineInstance = htolel(line_instance);
	msg->data.forwardstatus.cfwdAllStatus = htolel(status);
	ast_copy_string(msg->data.forwardstatus.cfwdAllNumber, extension,
				sizeof(msg->data.forwardstatus.cfwdAllNumber));

	return transmit_message(msg, session);
}

int transmit_forward_status_res(struct sccp_session *session, int lineInstance)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.forwardstatus.status = htolel(0);
	msg->data.forwardstatus.lineInstance = htolel(lineInstance);

	return transmit_message(msg, session);
}

int transmit_keep_alive_ack(struct sccp_session *session)
{
	return transmit_empty_message(KEEP_ALIVE_ACK_MESSAGE, session);
}

int transmit_lamp_state(struct sccp_session *session, enum sccp_stimulus_type stimulus, int line_instance, enum sccp_lamp_state indication)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct set_lamp_message), SET_LAMP_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.setlamp.stimulus = htolel(stimulus);
	msg->data.setlamp.lineInstance = htolel(line_instance);
	msg->data.setlamp.state = htolel(indication);

	return transmit_message(msg, session);
}

int transmit_line_status_res(struct sccp_session *session, int lineInstance, struct sccp_line *line)
{
	struct sccp_msg *msg;
	char *displayname = NULL;

	if (!session) {
		return -1;
	}

	if (!line) {
		ast_log(LOG_ERROR, "Line instance [%d] is not attached to device [%s]\n", lineInstance, session->device->name);
		return -1;
	}

	msg = msg_alloc(sizeof(struct line_status_res_message), LINE_STATUS_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.linestatus.lineNumber = htolel(lineInstance);

	if (line->device->proto_version <= 11) {
		displayname = utf8_to_iso88591(line->cid_name);
	}

	ast_copy_string(msg->data.linestatus.lineDirNumber, line->cid_num, sizeof(msg->data.linestatus.lineDirNumber));
	if (displayname)
		ast_copy_string(msg->data.linestatus.lineDisplayName, displayname, sizeof(msg->data.linestatus.lineDisplayName));
	else
		ast_copy_string(msg->data.linestatus.lineDisplayName, line->cid_name, sizeof(msg->data.linestatus.lineDisplayName));
	ast_copy_string(msg->data.linestatus.lineDisplayAlias, line->cid_num, sizeof(msg->data.linestatus.lineDisplayAlias));

	ast_free(displayname);

	return transmit_message(msg, session);
}

int transmit_register_ack(struct sccp_session *session, uint8_t protoVersion, int keepalive, char *dateFormat)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct register_ack_message), REGISTER_ACK_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.regack.keepAlive = htolel(keepalive);
	msg->data.regack.secondaryKeepAlive = htolel(keepalive);
	ast_copy_string(msg->data.regack.dateTemplate, dateFormat, sizeof(msg->data.regack.dateTemplate));

	if (protoVersion <= 3) {
		msg->data.regack.protoVersion = htolel(3);
		msg->data.regack.unknown1 = htolel(0x00);
		msg->data.regack.unknown2 = htolel(0x00);
		msg->data.regack.unknown3 = htolel(0x00);
	} else if (protoVersion <= 10) {
		msg->data.regack.protoVersion = htolel(protoVersion);
		msg->data.regack.unknown1 = htolel(0x20);
		msg->data.regack.unknown2 = htolel(0x00);
		msg->data.regack.unknown3 = htolel(0xFE);
	} else {
		msg->data.regack.protoVersion = htolel(11);
		msg->data.regack.unknown1 = htolel(0x20);
		msg->data.regack.unknown2 = htolel(0xF1);
		msg->data.regack.unknown3 = htolel(0xFF);
	}

	return transmit_message(msg, session);
}

int transmit_register_rej(struct sccp_session *session, const char *errorMessage)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct register_rej_message), REGISTER_REJ_MESSAGE);
	if (!msg) {
		return -1;
	}

	ast_copy_string(msg->data.regrej.errMsg, errorMessage, sizeof(msg->data.regrej.errMsg));

	return transmit_message(msg, session);
}

int transmit_reset(struct sccp_session *session, enum sccp_reset_type type)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct reset_message), RESET_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.reset.type = htolel(type);

	return transmit_message(msg, session);
}

int transmit_ringer_mode(struct sccp_session *session, enum sccp_ringer_mode mode)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct set_ringer_message), SET_RINGER_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.setringer.ringerMode = htolel(mode);
	msg->data.setringer.unknown1 = htolel(1);
	msg->data.setringer.unknown2 = htolel(1);

	return transmit_message(msg, session);
}

int transmit_selectsoftkeys(struct sccp_session *session, int line_instance, int callid, enum sccp_softkey_status softkey)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct select_soft_keys_message), SELECT_SOFT_KEYS_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.selectsoftkey.lineInstance = htolel(line_instance);
	msg->data.selectsoftkey.callInstance = htolel(callid);
	msg->data.selectsoftkey.softKeySetIndex = htolel(softkey);
	msg->data.selectsoftkey.validKeyMask = htolel(0xFFFFFFFF);

	return transmit_message(msg, session);
}

int transmit_speeddial_stat_res(struct sccp_session *session, int index, struct sccp_speeddial *speeddial)
{
	struct sccp_msg *msg;

	if (!speeddial) {
		return 0;
	}

	msg = msg_alloc(sizeof(struct speeddial_stat_res_message), SPEEDDIAL_STAT_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.speeddialstatus.instance = htolel(index);
	memcpy(msg->data.speeddialstatus.extension, speeddial->extension, sizeof(msg->data.speeddialstatus.extension));
	memcpy(msg->data.speeddialstatus.label, speeddial->label, sizeof(msg->data.speeddialstatus.label));

	return transmit_message(msg, session);
}

int transmit_softkey_set_res(struct sccp_session *session)
{
	int keyset_count = ARRAY_LEN(softkey_default_definitions);
	int i;
	int j;
	struct sccp_msg *msg;
	const struct softkey_definitions *softkeymode;

	msg = msg_alloc(sizeof(struct softkey_set_res_message), SOFTKEY_SET_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.softkeysets.softKeySetOffset = htolel(0);
	msg->data.softkeysets.softKeySetCount = htolel(keyset_count);
	msg->data.softkeysets.totalSoftKeySetCount = htolel(keyset_count);

	for (i = 0; i < keyset_count; i++) {
		softkeymode = &softkey_default_definitions[i];
		for (j = 0; j < softkeymode->count; j++) {
			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyTemplateIndex[j]
				= htolel(softkeymode->defaults[j]);

			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyInfoIndex[j]
				= htolel(softkeymode->defaults[j]);
		}
	}

	return transmit_message(msg, session);
}

int transmit_softkey_template_res(struct sccp_session *session)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct softkey_template_res_message), SOFTKEY_TEMPLATE_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.softkeytemplate.softKeyOffset = htolel(0);
	msg->data.softkeytemplate.softKeyCount = htolel(ARRAY_LEN(softkey_template_default));
	msg->data.softkeytemplate.totalSoftKeyCount = htolel(ARRAY_LEN(softkey_template_default));
	memcpy(msg->data.softkeytemplate.softKeyTemplateDefinition, softkey_template_default, sizeof(softkey_template_default));

	return transmit_message(msg, session);
}

int transmit_speaker_mode(struct sccp_session *session, enum sccp_speaker_mode mode)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct set_speaker_message), SET_SPEAKER_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.setspeaker.mode = htolel(mode);

	return transmit_message(msg, session);
}

int transmit_start_media_transmission(struct sccp_session *session, struct sccp_subchannel *subchan, struct sockaddr_in endpoint)
{
	struct sccp_msg *msg;
	struct ast_format_list fmt;
	uint32_t callid;

	if (!subchan) {
		return -1;
	}

	if (!session) {
		return -1;
	}

	msg = msg_alloc(sizeof(struct start_media_transmission_message), START_MEDIA_TRANSMISSION_MESSAGE);
	if (!msg) {
		return -1;
	}

	callid = subchan->id;
	fmt = ast_codec_pref_getsize(&subchan->line->codec_pref, &subchan->fmt);

	ast_debug(2, "Sending start media transmission to %s: %s %d\n",
			session->ipaddr, ast_inet_ntoa(endpoint.sin_addr), ntohs(endpoint.sin_port));

	msg->data.startmedia.conferenceId = htolel(callid);
	msg->data.startmedia.passThruPartyId = htolel(callid ^ 0xFFFFFFFF);
	msg->data.startmedia.remoteIp = htolel(endpoint.sin_addr.s_addr);
	msg->data.startmedia.remotePort = htolel(ntohs(endpoint.sin_port));
	msg->data.startmedia.packetSize = htolel(fmt.cur_ms);
	msg->data.startmedia.payloadType = htolel(codec_ast2sccp(&fmt.format));
	msg->data.startmedia.qualifier.precedence = htolel(127);
	msg->data.startmedia.qualifier.vad = htolel(0);
	msg->data.startmedia.qualifier.packets = htolel(0);
	msg->data.startmedia.qualifier.bitRate = htolel(0);
	msg->data.startmedia.conferenceId1 = htolel(callid);
	msg->data.startmedia.rtpTimeout = htolel(10);

	return transmit_message(msg, session);
}

int transmit_stop_media_transmission(struct sccp_session *session, uint32_t callid)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct stop_media_transmission_message), STOP_MEDIA_TRANSMISSION_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.stopmedia.conferenceId = htolel(callid);
	msg->data.stopmedia.partyId = htolel(callid ^ 0xFFFFFFFF);
	msg->data.stopmedia.conferenceId1 = htolel(callid);

	return transmit_message(msg, session);
}

int transmit_stop_tone(struct sccp_session *session, int line_instance, int callid)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct stop_tone_message), STOP_TONE_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.stop_tone.lineInstance = htolel(line_instance);
	msg->data.stop_tone.callInstance = htolel(callid);

	return transmit_message(msg, session);
}

int transmit_time_date_res(struct sccp_session *session)
{
	struct sccp_msg *msg;
	struct timeval now;
	struct ast_tm cmtime;

	msg = msg_alloc(sizeof(struct time_date_res_message), DATE_TIME_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	now = ast_tvnow();
	ast_localtime(&now, &cmtime, NULL);

	msg->data.timedate.year = htolel(cmtime.tm_year + 1900);
	msg->data.timedate.month = htolel(cmtime.tm_mon + 1);
	msg->data.timedate.dayOfWeek = htolel(cmtime.tm_wday);
	msg->data.timedate.day = htolel(cmtime.tm_mday);
	msg->data.timedate.hour = htolel(cmtime.tm_hour);
	msg->data.timedate.minute = htolel(cmtime.tm_min);
	msg->data.timedate.seconds = htolel(cmtime.tm_sec);
	msg->data.timedate.milliseconds = htolel(0);
	msg->data.timedate.systemTime = htolel(now.tv_sec);

	return transmit_message(msg, session);
}

int transmit_tone(struct sccp_session *session, enum sccp_tone tone, int line_instance, int callid)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct start_tone_message), START_TONE_MESSAGE);
	if (!msg) {
		return -1;
	}

	msg->data.starttone.tone = htolel(tone);
	msg->data.starttone.lineInstance = htolel(line_instance);
	msg->data.starttone.callInstance = htolel(callid);

	return transmit_message(msg, session);
}

int transmit_version_res(struct sccp_session *session, const char *version)
{
	struct sccp_msg *msg;

	msg = msg_alloc(sizeof(struct version_res_message), VERSION_RES_MESSAGE);
	if (!msg) {
		return -1;
	}

	ast_copy_string(msg->data.version.version, version, sizeof(msg->data.version.version));

	return transmit_message(msg, session);
}
