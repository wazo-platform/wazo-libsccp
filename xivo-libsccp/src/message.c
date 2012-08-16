#include <errno.h>
#include <string.h>

#include <asterisk.h>
#include <asterisk/utils.h>
#include <asterisk/rtp_engine.h>

#include "device.h"
#include "message.h"
#include "sccp.h"
#include "utils.h"

struct sccp_msg *msg_alloc(size_t data_length, int message_id)
{
	struct sccp_msg *msg = NULL;

	msg = ast_calloc(1, 12 + 4 + data_length);	
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return NULL;
	}

	msg->length = htolel(4 + data_length);
	msg->id = message_id;

	return msg;
}

int transmit_message(struct sccp_msg *msg, struct sccp_session *session)
{
	ssize_t nbyte = 0;

	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg is NULL\n");
		return -1;
	}

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	memcpy(session->outbuf, msg, 12);
	memcpy(session->outbuf+12, &msg->data, letohl(msg->length));

	nbyte = write(session->sockfd, session->outbuf, letohl(msg->length)+8);	
	if (nbyte == -1) {
		ast_log(LOG_WARNING, "message transmit failed: %s\n", strerror(errno));
	}

	ast_free(msg);

	return nbyte;
}

int transmit_speaker_mode(struct sccp_session *session, int mode)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct set_speaker_message), SET_SPEAKER_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.setspeaker.mode = htolel(mode);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_activatecallplane(struct sccp_line *line)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	if (line->device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	if (line->device->session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct activate_call_plane_message), ACTIVATE_CALL_PLANE_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.activatecallplane.lineInstance = htolel(line->instance);

	ret = transmit_message(msg, (struct sccp_session *)line->device->session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_close_receive_channel(struct sccp_line *line, uint32_t callid)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	if (line->device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	if (line->device->session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct close_receive_channel_message), CLOSE_RECEIVE_CHANNEL_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.closereceivechannel.conferenceId = htolel(0);
	msg->data.closereceivechannel.partyId = htolel(callid ^ 0xFFFFFFFF);
	msg->data.closereceivechannel.conferenceId1 = htolel(0);

	ret = transmit_message(msg, (struct sccp_session *)line->device->session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_stop_media_transmission(struct sccp_line *line, uint32_t callid)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;	

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	if (line->device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	if (line->device->session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct stop_media_transmission_message), STOP_MEDIA_TRANSMISSION_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation faile\n");
		return -1;
	}

	msg->data.stopmedia.conferenceId = htolel(0);
	msg->data.stopmedia.partyId = htolel(callid ^ 0xFFFFFFFF);
	msg->data.stopmedia.conferenceId1 = htolel(0); 
 
	ret = transmit_message(msg, (struct sccp_session *)line->device->session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_start_media_transmission(struct sccp_line *line, uint32_t callid, struct sockaddr_in endpoint, struct ast_format_list fmt)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	msg = msg_alloc(sizeof(struct start_media_transmission_message), START_MEDIA_TRANSMISSION_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.startmedia.conferenceId = htolel(0);
	msg->data.startmedia.passThruPartyId = htolel(callid ^ 0xFFFFFFFF);
	msg->data.startmedia.remoteIp = htolel(endpoint.sin_addr.s_addr);
	msg->data.startmedia.remotePort = htolel(ntohs(endpoint.sin_port));
	msg->data.startmedia.packetSize = htolel(fmt.cur_ms);
	msg->data.startmedia.payloadType = htolel(codec_ast2sccp(fmt.bits));
	msg->data.startmedia.qualifier.precedence = htolel(127);
	msg->data.startmedia.qualifier.vad = htolel(0);
	msg->data.startmedia.qualifier.packets = htolel(0);
	msg->data.startmedia.qualifier.bitRate = htolel(0);
	msg->data.startmedia.conferenceId1 = htolel(0);
	msg->data.startmedia.rtpTimeout = htolel(10);

	ret = transmit_message(msg, line->device->session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_connect(struct sccp_line *line, uint32_t callid)
{
	struct ast_format_list fmt = {0};
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return -1;
	}

	if (line->device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	if (line->device->session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	fmt = ast_codec_pref_getsize(&line->codec_pref, ast_best_codec(line->device->codecs));

	msg = msg_alloc(sizeof(struct open_receive_channel_message), OPEN_RECEIVE_CHANNEL_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.openreceivechannel.conferenceId = htolel(0);
	msg->data.openreceivechannel.partyId = htolel(callid ^ 0xFFFFFFFF);
	msg->data.openreceivechannel.packets = htolel(fmt.cur_ms);
	msg->data.openreceivechannel.capability = htolel(codec_ast2sccp(fmt.bits));
	msg->data.openreceivechannel.echo = htolel(0);
	msg->data.openreceivechannel.bitrate = htolel(0);
	msg->data.openreceivechannel.conferenceId1 = htolel(0);
	msg->data.openreceivechannel.rtpTimeout = htolel(10);

	ret = transmit_message(msg, (struct sccp_session *)line->device->session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_callinfo(struct sccp_session *session, const char *from_name, const char *from_num,
			const char *to_name, const char *to_num, int line_instance, int callid, int calltype)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct call_info_message), CALL_INFO_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	ast_copy_string(msg->data.callinfo.callingPartyName, from_name ? from_name: "", sizeof(msg->data.callinfo.callingPartyName));
	ast_copy_string(msg->data.callinfo.callingParty, from_num ? from_num: "", sizeof(msg->data.callinfo.callingParty));
	ast_copy_string(msg->data.callinfo.calledPartyName, to_name ? to_name: "", sizeof(msg->data.callinfo.calledPartyName));
	ast_copy_string(msg->data.callinfo.calledParty, to_num ? to_num: "", sizeof(msg->data.callinfo.calledParty));

	msg->data.callinfo.lineInstance = htolel(line_instance);
	msg->data.callinfo.callInstance = htolel(callid);
	msg->data.callinfo.type = htolel(calltype);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_forward_status_message(struct sccp_session *session , int line_instance, const char *extension, int status)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.forwardstatus.status = htolel(status);
	msg->data.forwardstatus.lineInstance = htolel(line_instance);
	msg->data.forwardstatus.cfwdAllStatus = htolel(status);

	ast_copy_string(msg->data.forwardstatus.cfwdAllNumber, extension,
				sizeof(msg->data.forwardstatus.cfwdAllNumber));

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_callstate(struct sccp_session *session, int line_instance, int state, uint32_t callid)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct call_state_message), CALL_STATE_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.callstate.callState = htolel(state);
	msg->data.callstate.lineInstance = htolel(line_instance);
	msg->data.callstate.callReference = htolel(callid);
	msg->data.callstate.visibility = htolel(0);
	msg->data.callstate.priority = htolel(4);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_displaymessage(struct sccp_session *session, const char *text)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct display_notify_message), DISPLAY_NOTIFY_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	} 

	msg->data.notify.displayTimeout = htolel(0);
        ast_copy_string(msg->data.notify.displayMessage, text,
				sizeof(msg->data.notify.displayMessage));

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_stop_tone(struct sccp_session *session, int line_instance, int callid)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct stop_tone_message), STOP_TONE_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.stop_tone.lineInstance = htolel(line_instance);
	msg->data.stop_tone.callInstance = htolel(callid);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_tone(struct sccp_session *session, int tone, int line_instance, int callid)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct start_tone_message), START_TONE_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.starttone.tone = htolel(tone);
	msg->data.starttone.lineInstance = htolel(line_instance);
	msg->data.starttone.callInstance = htolel(callid);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_lamp_state(struct sccp_session *session, int stimulus, int line_instance, int indication)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct set_lamp_message), SET_LAMP_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.setlamp.stimulus = htolel(stimulus);
	msg->data.setlamp.lineInstance = htolel(line_instance);
	msg->data.setlamp.state = htolel(indication);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_reset(struct sccp_session *session, uint32_t type)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	/* 2 => hard restart
	 * 1 => soft reset */
	if (type != 1 && type != 2) {
		ast_log(LOG_DEBUG, "reset type is unknown (%d)\n", type);
		type = 1;
	}

	msg = msg_alloc(sizeof(struct reset_message), RESET_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.reset.type = htolel(type);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_ringer_mode(struct sccp_session *session, int mode)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct set_ringer_message), SET_RINGER_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.setringer.ringerMode = htolel(mode);
	msg->data.setringer.unknown1 = htolel(1);
	msg->data.setringer.unknown2 = htolel(1);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_dialed_number(struct sccp_session *session, const char *extension, int line_instance, int callid)
{
	int ret = 0;
	struct sccp_msg *msg = NULL;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	if (extension == NULL) {
		ast_log(LOG_DEBUG, "extension is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct dialed_number_message), DIALED_NUMBER_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	ast_copy_string(msg->data.dialednumber.calledParty, extension, sizeof(msg->data.dialednumber.calledParty));

	msg->data.dialednumber.lineInstance = htolel(line_instance);
	msg->data.dialednumber.callInstance = htolel(callid);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}

int transmit_selectsoftkeys(struct sccp_session *session, int line_instance, int callid, int softkey)
{
	struct sccp_msg *msg = NULL;
	int ret = 0;

	if (session == NULL) {
		ast_log(LOG_DEBUG, "session is NULL\n");
		return -1;
	}

	msg = msg_alloc(sizeof(struct select_soft_keys_message), SELECT_SOFT_KEYS_MESSAGE);
	if (msg == NULL) {
		ast_log(LOG_DEBUG, "msg allocation failed\n");
		return -1;
	}

	msg->data.selectsoftkey.lineInstance = htolel(line_instance);
	msg->data.selectsoftkey.callInstance = htolel(callid);
	msg->data.selectsoftkey.softKeySetIndex = htolel(softkey);
	msg->data.selectsoftkey.validKeyMask = htolel(0xFFFFFFFF);

	ret = transmit_message(msg, session);
	if (ret == -1)
		return -1;

	return 0;
}
