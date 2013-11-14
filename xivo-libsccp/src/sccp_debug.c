#include "sccp_debug.h"
#include "sccp_device.h"
#include "sccp_message.h"
#include "sccp_utils.h"

int sccp_debug;
char sccp_debug_addr[16];

static void dump_message(const struct sccp_session *session, const struct sccp_msg *msg, const char *head1, const char *head2);

static void dump_call_info(char *str, size_t size, const struct call_info_message *m);
static void dump_call_state(char *str, size_t size, const struct call_state_message *m);
static void dump_close_receive_channel(char *str, size_t size, const struct close_receive_channel_message *m);
static void dump_keypad_button(char *str, size_t size, const struct keypad_button_message *m);
static void dump_offhook(char *str, size_t size, const struct offhook_message *m);
static void dump_onhook(char *str, size_t size, const struct onhook_message *m);
static void dump_open_receive_channel_ack(char *str, size_t size, const struct open_receive_channel_ack_message *m);
static void dump_select_soft_keys(char *str, size_t size, const struct select_soft_keys_message *m);
static void dump_set_ringer(char *str, size_t size, const struct set_ringer_message *m);
static void dump_softkey_event(char *str, size_t size, const struct softkey_event_message *m);
static void dump_start_media_transmission(char *str, size_t size, const struct start_media_transmission_message *m);
static void dump_stop_media_transmission(char *str, size_t size, const struct stop_media_transmission_message *m);

static const char *ringer_mode_str(enum sccp_ringer_mode v);
static const char *softkey_status_str(enum sccp_softkey_status v);
static const char *softkey_str(enum sccp_softkey_type v);

void sccp_enable_debug(void)
{
	sccp_debug = 1;
	*sccp_debug_addr = '\0';
}

void sccp_enable_debug_ip(const char *ip)
{
	sccp_debug = 1;
	ast_copy_string(sccp_debug_addr, ip, sizeof(sccp_debug_addr));
}

void sccp_disable_debug(void)
{
	sccp_debug = 0;
}

void sccp_dump_message_received(const struct sccp_session *session, const struct sccp_msg *msg)
{
	dump_message(session, msg, "Received message", "from");
}

void sccp_dump_message_transmitting(const struct sccp_session *session, const struct sccp_msg *msg)
{
	dump_message(session, msg, "Transmitting message", "to");
}

static void dump_message(const struct sccp_session *session, const struct sccp_msg *msg, const char *head1, const char *head2)
{
	char body[256];
	int pad = 1;
	uint32_t msg_id;

	if (session == NULL || msg == NULL) {
		return;
	}

	*body = '\0';
	msg_id = letohl(msg->id);

	if (msg_id == KEEP_ALIVE_MESSAGE || msg_id == KEEP_ALIVE_ACK_MESSAGE) {
		// don't dump these messages
		return;
	}

	switch (msg_id) {
	case CALL_INFO_MESSAGE:
		dump_call_info(body, sizeof(body), &msg->data.callinfo);
		break;
	case CALL_STATE_MESSAGE:
		dump_call_state(body, sizeof(body), &msg->data.callstate);
		break;
	case CLOSE_RECEIVE_CHANNEL_MESSAGE:
		dump_close_receive_channel(body, sizeof(body), &msg->data.closereceivechannel);
		break;
	case KEYPAD_BUTTON_MESSAGE:
		dump_keypad_button(body, sizeof(body), &msg->data.keypad);
		break;
	case OFFHOOK_MESSAGE:
		dump_offhook(body, sizeof(body), &msg->data.offhook);
		break;
	case ONHOOK_MESSAGE:
		dump_onhook(body, sizeof(body), &msg->data.onhook);
		break;
	case OPEN_RECEIVE_CHANNEL_ACK_MESSAGE:
		dump_open_receive_channel_ack(body, sizeof(body), &msg->data.openreceivechannelack);
		break;
	case SELECT_SOFT_KEYS_MESSAGE:
		dump_select_soft_keys(body, sizeof(body), &msg->data.selectsoftkey);
		break;
	case SET_RINGER_MESSAGE:
		dump_set_ringer(body, sizeof(body), &msg->data.setringer);
		break;
	case SOFTKEY_EVENT_MESSAGE:
		dump_softkey_event(body, sizeof(body), &msg->data.softkeyevent);
		break;
	case START_MEDIA_TRANSMISSION_MESSAGE:
		dump_start_media_transmission(body, sizeof(body), &msg->data.startmedia);
		break;
	case STOP_MEDIA_TRANSMISSION_MESSAGE:
		dump_stop_media_transmission(body, sizeof(body), &msg->data.stopmedia);
		break;
	default:
		pad = 0;
		break;
	}

	ast_verbose(
		"\n<--- %s \"%s\" %s %s:%d -->\n"
		"Length: %4u   Reserved: 0x%08X   ID: 0x%04X\n%s"
		"%s"
		"\n<------------>\n",
		head1, msg_id_str(msg_id), head2, session->ipaddr, session->port, letohl(msg->length),
		letohl(msg->reserved), msg_id, pad ? "\n" : "", body
	);
}

static void dump_call_info(char *str, size_t size, const struct call_info_message *m)
{
	snprintf(str, size,
			"Calling name: %s\n"
			"Calling: %s\n"
			"Called name: %s\n"
			"Called: %s\n"
			"Line instance: %u\n"
			"Call ID: %u\n"
			"Type: %u\n",
			m->callingPartyName, m->callingParty, m->calledPartyName, m->calledParty,
			letohl(m->lineInstance), letohl(m->callInstance), letohl(m->type));
}

static void dump_call_state(char *str, size_t size, const struct call_state_message *m)
{
	snprintf(str, size,
			"State: %s\n"
			"Line instance: %u\n"
			"Call ID: %u\n",
			line_state_str(letohl(m->callState)), letohl(m->lineInstance), letohl(m->callReference));
}

static void dump_close_receive_channel(char *str, size_t size, const struct close_receive_channel_message *m)
{
	snprintf(str, size,
			"Conference ID: %u\n",
			letohl(m->conferenceId));
}

static void dump_keypad_button(char *str, size_t size, const struct keypad_button_message *m)
{
	snprintf(str, size,
			"Button: %u\n"
			"Line instance: %u\n"
			"Call ID: %u\n",
			letohl(m->button), letohl(m->lineInstance), letohl(m->callInstance));
}

static void dump_offhook(char *str, size_t size, const struct offhook_message *m)
{
	snprintf(str, size,
			"Line instance: %u\n"
			"Call ID: %u\n",
			letohl(m->lineInstance), letohl(m->callInstance));
}

static void dump_onhook(char *str, size_t size, const struct onhook_message *m)
{
	snprintf(str, size,
			"Line instance: %u\n"
			"Call ID: %u\n",
			letohl(m->lineInstance), letohl(m->callInstance));
}

static void dump_open_receive_channel_ack(char *str, size_t size, const struct open_receive_channel_ack_message *m)
{
	char buf[INET_ADDRSTRLEN];
	struct in_addr addr;

	addr.s_addr = m->ipAddr;

	if (!inet_ntop(AF_INET, &addr, buf, sizeof(buf))) {
		return;
	}

	snprintf(str, size,
			"Status: %u\n"
			"IP: %s\n"
			"Port: %u\n",
			letohl(m->status), buf, letohl(m->port));
}

static void dump_select_soft_keys(char *str, size_t size, const struct select_soft_keys_message *m)
{
	snprintf(str, size,
			"Softkey status: %s\n"
			"Line instance: %u\n"
			"Call ID: %u\n",
			softkey_status_str(letohl(m->softKeySetIndex)), letohl(m->lineInstance), letohl(m->callInstance));
}

static void dump_set_ringer(char *str, size_t size, const struct set_ringer_message *m)
{
	snprintf(str, size,
			"Mode: %s\n",
			ringer_mode_str(letohl(m->ringerMode)));
}

static void dump_softkey_event(char *str, size_t size, const struct softkey_event_message *m)
{
	snprintf(str, size,
			"Event: %s\n"
			"Line instance: %u\n"
			"Call ID: %u\n",
			softkey_str(letohl(m->softKeyEvent)), letohl(m->lineInstance), letohl(m->callInstance));
}

static void dump_start_media_transmission(char *str, size_t size, const struct start_media_transmission_message *m)
{
	char buf[INET_ADDRSTRLEN];
	struct in_addr addr;

	addr.s_addr = m->remoteIp;

	if (!inet_ntop(AF_INET, &addr, buf, sizeof(buf))) {
		return;
	}

	snprintf(str, size,
			"Call ID: %u\n"
			"IP: %s\n"
			"Port: %u\n"
			"Packet size: %u\n",
			letohl(m->conferenceId), buf, letohl(m->remotePort), letohl(m->packetSize));
}

static void dump_stop_media_transmission(char *str, size_t size, const struct stop_media_transmission_message *m)
{
	snprintf(str, size,
			"Conference ID: %u\n",
			letohl(m->conferenceId));
}

static const char *ringer_mode_str(enum sccp_ringer_mode v)
{
	switch (v) {
	case SCCP_RING_OFF:
		return "ring off";
	case SCCP_RING_INSIDE:
		return "ring inside";
	case SCCP_RING_OUTSIDE:
		return "ring outside";
	case SCCP_RING_FEATURE:
		return "ring feature";
	}

	return "unknown";
}

static const char *softkey_status_str(enum sccp_softkey_status v)
{
	switch (v) {
	case KEYDEF_ONHOOK:
		return "onhook";
	case KEYDEF_CONNECTED:
		return "connected";
	case KEYDEF_ONHOLD:
		return "onhold";
	case KEYDEF_RINGIN:
		return "ringin";
	case KEYDEF_OFFHOOK:
		return "offhook";
	case KEYDEF_CONNINTRANSFER:
		return "connintransfer";
	case KEYDEF_CALLFWD:
		return "callfwd";
	case KEYDEF_DIALINTRANSFER:
		return "dialintransfer";
	case KEYDEF_RINGOUT:
		return "ringout";
	case KEYDEF_AUTOANSWER:
		return "autoanswer";
	}

	return "unknown";
}

static const char *softkey_str(enum sccp_softkey_type v)
{
	switch (v) {
	case SOFTKEY_NONE:
		return "none";
	case SOFTKEY_REDIAL:
		return "redial";
	case SOFTKEY_NEWCALL:
		return "newcall";
	case SOFTKEY_HOLD:
		return "hold";
	case SOFTKEY_TRNSFER:
		return "transfer";
	case SOFTKEY_CFWDALL:
		return "cfwdall";
	case SOFTKEY_CFWDBUSY:
		return "cfwdbusy";
	case SOFTKEY_CFWDNOANSWER:
		return "cfwdnoanswer";
	case SOFTKEY_BKSPC:
		return "bkspc";
	case SOFTKEY_ENDCALL:
		return "endcall";
	case SOFTKEY_RESUME:
		return "resume";
	case SOFTKEY_ANSWER:
		return "answer";
	case SOFTKEY_INFO:
		return "info";
	case SOFTKEY_CONFRN:
		return "confrn";
	case SOFTKEY_PARK:
		return "park";
	case SOFTKEY_JOIN:
		return "join";
	case SOFTKEY_MEETME:
		return "meetme";
	case SOFTKEY_PICKUP:
		return "pickup";
	case SOFTKEY_GPICKUP:
		return "gpickup";
	case SOFTKEY_DND:
		return "dnd";
	}

	return "unknown";
}
