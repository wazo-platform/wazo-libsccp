#ifndef SCCP_MESSAGE_H
#define SCCP_MESSAGE_H

#include "sccp_device.h"
#include "sccp.h"
#include "sccp_config.h"

const char *msg_id_str(uint32_t msg_id);

int transmit_button_template_res(struct sccp_session *session);
int transmit_callinfo(struct sccp_session *session, const char *from_name, const char *from_num, const char *to_name, const char *to_num, int lineInstance, int callInstance, int calltype);
int transmit_callstate(struct sccp_session *session, int lineInstance, int state, unsigned callInstance);
int transmit_capabilities_req(struct sccp_session *session);
int transmit_clearmessage(struct sccp_session *session);
int transmit_close_receive_channel(struct sccp_session *session, uint32_t callInstance);
int transmit_config_status_res(struct sccp_session *session);
int transmit_open_receive_channel(struct sccp_session *session, struct sccp_subchannel *subchan);
int transmit_dialed_number(struct sccp_session *session, const char *extension, int instance, int callid);
int transmit_displaymessage(struct sccp_session *session, const char *text);
int transmit_feature_status(struct sccp_session *session, int instance, int type, int status, const char *label);
int transmit_forward_status_message(struct sccp_session *session, int instance, const char *extension, int status);
int transmit_forward_status_res(struct sccp_session *session, int lineInstance);
int transmit_keep_alive_ack(struct sccp_session *session);
int transmit_lamp_state(struct sccp_session *session, int callInstance, int lineInstance, int state);
int transmit_line_status_res(struct sccp_session *session, int lineInstance, struct sccp_line *line);
int transmit_register_ack(struct sccp_session *session, uint8_t protoVersion, int keepalive, char* dateFormat);
int transmit_register_rej(struct sccp_session *session, const char *errorMessage);
int transmit_reset(struct sccp_session *session, uint32_t type);
int transmit_ringer_mode(struct sccp_session *session, int mode);
int transmit_selectsoftkeys(struct sccp_session *session, int lineInstance, int callInstance, int softkey);
int transmit_speeddial_stat_res(struct sccp_session *session, int index, struct sccp_speeddial *speeddial);
int transmit_softkey_set_res(struct sccp_session *session);
int transmit_softkey_template_res(struct sccp_session *session);
int transmit_speaker_mode(struct sccp_session *session, int mode);
int transmit_start_media_transmission(struct sccp_session *session, uint32_t callid, struct sockaddr_in endpoint, struct ast_format_list fmt);
int transmit_stop_media_transmission(struct sccp_session *session, uint32_t callid);
int transmit_stop_tone(struct sccp_session *session, int instance, int reference);
int transmit_time_date_res(struct sccp_session *session);
int transmit_tone(struct sccp_session *session, int tone, int lineInstance, int callInstance);

#define KEEP_ALIVE_MESSAGE 0x0000

#define REGISTER_MESSAGE 0x0001
struct register_message {
	char name[16];
	uint32_t userId;
	uint32_t lineInstance;
	uint32_t ip;
	uint32_t type;
	uint32_t maxStreams;
	uint32_t activeStreams;
	uint8_t protoVersion;
};

#define IP_PORT_MESSAGE 0x0002
struct ip_port_message {
	uint32_t stationIpPort;
};

#define ENBLOC_CALL_MESSAGE 0x0004
struct enbloc_call_message {
	char extension[24];
};

#define KEYPAD_BUTTON_MESSAGE 0x0003
struct keypad_button_message {
	uint32_t button;
	uint32_t lineInstance;
	uint32_t callInstance;
};

#define STIMULUS_MESSAGE 0x0005
struct stimulus_message {
	uint32_t stimulus;
	uint32_t lineInstance;
};

#define OFFHOOK_MESSAGE 0x0006
struct offhook_message {
	uint32_t lineInstance;
	uint32_t callInstance;
};

#define ONHOOK_MESSAGE 0x0007
struct onhook_message {
	uint32_t lineInstance;
	uint32_t callInstance;
};

#define FORWARD_STATUS_REQ_MESSAGE 0x0009
struct forward_status_req_message {
	uint32_t lineInstance;
};

#define CAPABILITIES_RES_MESSAGE 0x0010
struct station_capabilities {
	uint32_t codec;
	uint32_t frames;
	union {
		char res[8];
		uint32_t g723bitRate;	/* g723 Bit Rate (1=5.3 Kbps, 2=6.4 Kbps) */
	} payloads;
};

#define SCCP_MAX_CAPABILITIES 18
struct capabilities_res_message {
	uint32_t count;
	struct station_capabilities caps[SCCP_MAX_CAPABILITIES];
};

#define SPEEDDIAL_STAT_REQ_MESSAGE 0x000A
struct speeddial_stat_req_message {
	uint32_t instance;
};

#define LINE_STATUS_REQ_MESSAGE 0x000B
struct line_status_req_message {
	uint32_t lineInstance;
};

#define CONFIG_STATUS_REQ_MESSAGE 0x000C
#define TIME_DATE_REQ_MESSAGE 0x000D
#define BUTTON_TEMPLATE_REQ_MESSAGE 0x000E

#define ALARM_MESSAGE 0x0020
struct alarm_message {
	uint32_t alarmSeverity;
	char displayMessage[80];
	uint32_t alarmParam1;
	uint32_t alarmParam2;
};

#define OPEN_RECEIVE_CHANNEL_ACK_MESSAGE 0x0022
struct open_receive_channel_ack_message {
	uint32_t status;
	uint32_t ipAddr;
	uint32_t port;
	uint32_t passThruId;
};

#define SOFTKEY_SET_REQ_MESSAGE 0x0025
#define SOFTKEY_EVENT_MESSAGE 0x0026
struct softkey_event_message {
	uint32_t softKeyEvent;
	uint32_t lineInstance;
	uint32_t callInstance;
};

#define SOFTKEY_TEMPLATE_REQ_MESSAGE 0x0028
#define REGISTER_AVAILABLE_LINES_MESSAGE 0x002D

#define FEATURE_STATUS_REQ_MESSAGE 0X0034
struct feature_status_req_message {
	uint32_t instance;
	uint32_t unknown;
};

#define ACCESSORY_STATUS_MESSAGE 0x0049
#define REGISTER_ACK_MESSAGE 0x0081
struct register_ack_message {
	uint32_t keepAlive;
	char dateTemplate[6];
	char res[2];
	uint32_t secondaryKeepAlive;
	uint8_t protoVersion;
	uint8_t unknown1;
	uint8_t unknown2;
	uint8_t unknown3;
};

#define START_TONE_MESSAGE 0x00082
struct start_tone_message {
	uint32_t tone;
	uint32_t space;
	uint32_t lineInstance;
	uint32_t callInstance;
};

#define STOP_TONE_MESSAGE 0x0083
struct stop_tone_message {
	uint32_t lineInstance;
	uint32_t callInstance;
};

#define SET_RINGER_MESSAGE 0x0085
struct set_ringer_message {
	uint32_t ringerMode;
	uint32_t unknown1;
	uint32_t unknown2;
	uint32_t space[2];
};

#define SET_LAMP_MESSAGE 0x0086
struct set_lamp_message {
	uint32_t stimulus;
	uint32_t lineInstance;
	uint32_t state;
};

#define SET_SPEAKER_MESSAGE 0X0088
struct set_speaker_message {
	uint32_t mode;
};

#define STOP_MEDIA_TRANSMISSION_MESSAGE 0x008B
struct stop_media_transmission_message {
	uint32_t conferenceId;
	uint32_t partyId;
	uint32_t conferenceId1;
	uint32_t unknown1;
};

#define START_MEDIA_TRANSMISSION_MESSAGE 0x008A
struct media_qualifier {
	uint32_t precedence;
	uint32_t vad;
	uint16_t packets;
	uint32_t bitRate;
};

struct start_media_transmission_message {
	uint32_t conferenceId;
	uint32_t passThruPartyId;
	uint32_t remoteIp;
	uint32_t remotePort;
	uint32_t packetSize;
	uint32_t payloadType;
	struct media_qualifier qualifier;
	uint32_t conferenceId1;
	uint32_t space[14];
	uint32_t rtpDtmfPayload;
	uint32_t rtpTimeout;
	uint32_t mixingMode;
	uint32_t mixingParty;
};

#define CALL_INFO_MESSAGE 0x008F
struct call_info_message {
	char callingPartyName[40];
	char callingParty[24];
	char calledPartyName[40];
	char calledParty[24];
	uint32_t lineInstance;
	uint32_t callInstance;
	uint32_t type;
	char originalCalledPartyName[40];
	char originalCalledParty[24];
	char lastRedirectingPartyName[40];
	char lastRedirectingParty[24];
	uint32_t originalCalledPartyRedirectReason;
	uint32_t lastRedirectingReason;
	char callingPartyVoiceMailbox[24];
	char calledPartyVoiceMailbox[24];
	char originalCalledPartyVoiceMailbox[24];
	char lastRedirectingVoiceMailbox[24];
	uint32_t space[3];
};

#define FORWARD_STATUS_RES_MESSAGE 0x0090
struct forward_status_res_message {
	uint32_t status;
	uint32_t lineInstance;
	uint32_t cfwdAllStatus;
	char cfwdAllNumber[24];
	uint32_t cfwdBusyStatus;
	char cfwdBusyNumber[24];
	uint32_t cfwdNoAnswerStatus;
	char cfwdNoAnswerNumber[24];
};

#define SPEEDDIAL_STAT_RES_MESSAGE 0x0091
struct speeddial_stat_res_message {
	uint32_t instance;
	char extension[24];
	char label[40];
};

#define LINE_STATUS_RES_MESSAGE 0x0092
struct line_status_res_message {
	uint32_t lineNumber;
	char lineDirNumber[24];
	char lineDisplayName[40];
	char lineDisplayAlias[44];
};

#define CONFIG_STATUS_RES_MESSAGE 0x0093
struct config_status_res_message {
	char deviceName[16];
	uint32_t stationUserId;
	uint32_t stationInstance;
	char userName[40];
	char serverName[40];
	uint32_t numberLines;
	uint32_t numberSpeedDials;
};

#define DATE_TIME_RES_MESSAGE 0x0094
struct time_date_res_message {
	uint32_t year;
	uint32_t month;
	uint32_t dayOfWeek;
	uint32_t day;
	uint32_t hour;
	uint32_t minute;
	uint32_t seconds;
	uint32_t milliseconds;
	uint32_t systemTime;
};

#define BUTTON_TEMPLATE_RES_MESSAGE 0x0097
struct button_definition {
	uint8_t lineInstance;
	uint8_t buttonDefinition;
};

struct button_template_res_message {
	uint32_t buttonOffset;
	uint32_t buttonCount;
	uint32_t totalButtonCount;
	struct button_definition definition[42];
};

#define CAPABILITIES_REQ_MESSAGE 0x009B

#define REGISTER_REJ_MESSAGE 0x009D
struct register_rej_message {
	char errMsg[33];
};

#define RESET_MESSAGE 0x009F
struct reset_message {
	uint32_t type;
};

#define KEEP_ALIVE_ACK_MESSAGE 0x0100

#define OPEN_RECEIVE_CHANNEL_MESSAGE 0x0105
struct open_receive_channel_message {
	uint32_t conferenceId;
	uint32_t partyId;
	uint32_t packets;
	uint32_t capability;
	uint32_t echo;
	uint32_t bitrate;

	uint32_t conferenceId1;

	uint32_t unknown1;
	uint32_t unknown2;
	uint32_t unknown3;
	uint32_t unknown4;
	uint32_t unknown5;
	uint32_t unknown6;
	uint32_t unknown7;
	uint32_t unknown8;
	uint32_t unknown9;
	uint32_t unknown10;

	uint32_t unknown11;
	uint32_t unknown12;
	uint32_t unknown13;
	uint32_t unknown14;
	uint32_t rtpDtmfPayload;
	uint32_t rtpTimeout;

	uint32_t mixingMode;
	uint32_t mixingParty;
	char IpAddr[16];
	uint32_t unknown17;
};

#define CLOSE_RECEIVE_CHANNEL_MESSAGE 0x0106
struct close_receive_channel_message {
	uint32_t conferenceId;
	uint32_t partyId;
	uint32_t conferenceId1;
};

#define SOFTKEY_TEMPLATE_RES_MESSAGE 0x0108
struct softkey_template_definition {
	char softKeyLabel[16];
	uint32_t softKeyEvent;
};

#define SOFTKEY_SET_RES_MESSAGE 0x0109
struct softkey_set_definition {
	uint8_t softKeyTemplateIndex[16];
	uint16_t softKeyInfoIndex[16];
};

#define SELECT_SOFT_KEYS_MESSAGE 0x0110
struct select_soft_keys_message {
	uint32_t lineInstance;
	uint32_t callInstance;
	uint32_t softKeySetIndex;
	uint32_t validKeyMask;
};

#define CALL_STATE_MESSAGE 0x0111
struct call_state_message {
	uint32_t callState;
	uint32_t lineInstance;
	uint32_t callReference;
	uint32_t visibility;
	uint32_t priority;
	uint32_t unknown;
};

#define DISPLAY_NOTIFY_MESSAGE 0x0114
struct display_notify_message {
	uint32_t displayTimeout;
	char displayMessage[100];
};

#define CLEAR_NOTIFY_MESSAGE 0x0115

#define ACTIVATE_CALL_PLANE_MESSAGE 0x0116
struct activate_call_plane_message {
	uint32_t lineInstance;
};

#define DIALED_NUMBER_MESSAGE 0x011d
struct dialed_number_message {
	char calledParty[24];
	uint32_t lineInstance;
	uint32_t callInstance;
};

#define FEATURE_STAT_MESSAGE 0x0146
struct feature_stat_message {
	uint32_t bt_instance;
	uint32_t type;
	uint32_t status;
	char label[40];
};

#define START_MEDIA_TRANSMISSION_ACK_MESSAGE 0x0159

struct softkey_set_res_message {
	uint32_t softKeySetOffset;
	uint32_t softKeySetCount;
	uint32_t totalSoftKeySetCount;
	struct softkey_set_definition softKeySetDefinition[16];
	uint32_t res;
};

struct softkey_template_res_message {
	uint32_t softKeyOffset;
	uint32_t softKeyCount;
	uint32_t totalSoftKeyCount;
	struct softkey_template_definition softKeyTemplateDefinition[32];
};

union sccp_data {
	struct alarm_message alarm;
	struct register_message reg;
	struct register_ack_message regack;
	struct register_rej_message regrej;
	struct stimulus_message stimulus;
	struct offhook_message offhook;
	struct onhook_message onhook;
	struct start_tone_message starttone;
	struct forward_status_req_message forward;
	struct forward_status_res_message forwardstatus;
	struct capabilities_res_message caps;
	struct enbloc_call_message enbloc;
	struct ip_port_message ipport;
	struct button_template_res_message buttontemplate;
	struct speeddial_stat_req_message speeddial;
	struct feature_status_req_message feature;
	struct line_status_req_message line;
	struct speeddial_stat_res_message speeddialstatus;
	struct line_status_res_message linestatus;
	struct time_date_res_message timedate;
	struct config_status_res_message configstatus;
	struct reset_message reset;
	struct set_lamp_message setlamp;
	struct stop_tone_message stop_tone;
	struct set_ringer_message setringer;
	struct display_notify_message notify;
	struct call_state_message callstate;
	struct keypad_button_message keypad;
	struct softkey_event_message softkeyevent;
	struct activate_call_plane_message activatecallplane;
	struct softkey_set_res_message softkeysets;
	struct softkey_template_res_message softkeytemplate;
	struct select_soft_keys_message selectsoftkey;
	struct dialed_number_message dialednumber;
	struct feature_stat_message featurestatus;
	struct stop_media_transmission_message stopmedia;
	struct start_media_transmission_message startmedia;
	struct set_speaker_message setspeaker;
	struct call_info_message callinfo;
	struct close_receive_channel_message closereceivechannel;
	struct open_receive_channel_message openreceivechannel;
	struct open_receive_channel_ack_message openreceivechannelack;
};

/* message composition */
struct sccp_msg {
	uint32_t length;
	uint32_t reserved;
	uint32_t id;
	union sccp_data data;
};

struct softkey_template_definition softkey_template_default[] = {
	{"\x80\x01", 0x01}, // Redial
	{"\x80\x02", 0x02}, // NewCall
	{"\x80\x03", 0x03}, // Hold
	{"\x80\x04", 0x04}, // Trnsfer
	{"\x80\x05", 0x05}, // CFwdAll
	{"\x80\x06", 0x06}, // CFwdBusy
	{"\x80\x07", 0x07}, // CFwdNoAnswer
	{"\x80\x08", 0x08}, // Cancel
	{"\x80\x09", 0x09}, // EndCall
	{"\x80\x0A", 0x0A}, // Resume
	{"\x80\x0B", 0x0B}, // Answer
	{"\x80\x0C", 0x0C}, // Info
	{"\x80\x0D", 0x0D}, // Confrn
	{"\x80\x0E", 0x0E}, // Park
	{"\x80\x0F", 0x0F}, // Join
	{"\x80\x10", 0x10}, // MeetMe
	{"\x80\x11", 0x11}, // PickUp
	{"\x80\x12", 0x12}, // GPickUp
	{"Dial", 0x13}, // Dial
	{"\200\77", 0x14}, // DND
};

#endif /* SCCP_MESSAGE_H */
