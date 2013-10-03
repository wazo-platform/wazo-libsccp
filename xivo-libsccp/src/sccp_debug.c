#include "sccp_debug.h"
#include "sccp_message.h"
#include "sccp_utils.h"

int sccp_debug;
char sccp_debug_addr[16];

static void dump_message(struct sccp_session *session, struct sccp_msg *msg, const char *head);
static void dump_open_receive_channel_ack(char *str, size_t size, struct open_receive_channel_ack_message *m);

void dump_message_received(struct sccp_session *session, struct sccp_msg *msg) {
	dump_message(session, msg, "Received message from");
}

void dump_message_transmitting(struct sccp_session *session, struct sccp_msg *msg) {
	dump_message(session, msg, "Transmitting message to");
}

static void dump_message(struct sccp_session *session, struct sccp_msg *msg, const char *head) {
	char body[256];
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
	case OFFHOOK_MESSAGE:
		snprintf(body, sizeof(body), "Line instance: %d\nCall instance: %d\n\n",
			letohl(msg->data.offhook.lineInstance),
			letohl(msg->data.offhook.callInstance));
		break;
	case ONHOOK_MESSAGE:
		snprintf(body, sizeof(body), "Line instance: %d\nCall instance: %d\n\n",
			letohl(msg->data.offhook.lineInstance),
			letohl(msg->data.offhook.callInstance));
		break;
	case OPEN_RECEIVE_CHANNEL_ACK_MESSAGE:
		dump_open_receive_channel_ack(body, sizeof(body), &msg->data.openreceivechannelack);
		break;
	}

	ast_verbose(
		"\n<--- %s %s:%d -->\n"
		"Length: %4u   Reserved: 0x%08X   ID: 0x%04X (%s)\n\n"
		"%s"
		"\n<------------>\n",
		head, session->ipaddr, session->port, letohl(msg->length), letohl(msg->reserved),
		msg_id, msg_id_str(msg_id), body
	);
}

static void dump_open_receive_channel_ack(char *str, size_t size, struct open_receive_channel_ack_message *m) {
	char buf[INET_ADDRSTRLEN];
	struct in_addr addr;

	addr.s_addr = m->ipAddr;

	if (!inet_ntop(AF_INET, &addr, buf, sizeof(buf))) {
		ast_log(LOG_WARNING, "could not dump msg: error calling inet_ntop: %s\n", strerror(errno));
		return;
	}

	snprintf(str, size, "Status: %d\nIP: %s\nPort: %u\n\n", letohl(m->status), buf, letohl(m->port));
}
