#include <asterisk.h>
#include <asterisk/strings.h>

#include "sccp.h"
#include "sccp_debug.h"
#include "sccp_msg.h"
#include "sccp_utils.h"

static int sccp_debug;
static char sccp_debug_device_name[SCCP_DEVICE_NAME_MAX];
static char sccp_debug_ip[INET_ADDRSTRLEN];

static void dump_message(const struct sccp_msg *msg, const char *head1, const char *head2, const char *ipaddr, int port);

void sccp_debug_enable(void)
{
	sccp_debug_disable();
	sccp_debug = 1;
}

void sccp_debug_enable_device_name(const char *name)
{
	sccp_debug_disable();
	ast_copy_string(sccp_debug_device_name, name, sizeof(sccp_debug_device_name));
}

void sccp_debug_enable_ip(const char *ip)
{
	sccp_debug_disable();
	ast_copy_string(sccp_debug_ip, ip, sizeof(sccp_debug_ip));
}

void sccp_debug_disable(void)
{
	sccp_debug = 0;
	*sccp_debug_device_name = '\0';
	*sccp_debug_ip = '\0';
}

int sccp_debug_enabled(const char *device_name, const char *ip)
{
	return sccp_debug ||
		(device_name && !strcmp(device_name, sccp_debug_device_name)) ||
		(ip && !strcmp(ip, sccp_debug_ip));
}

void sccp_dump_message_received(const struct sccp_msg *msg, const char *ipaddr, int port)
{
	dump_message(msg, "Received message", "from", ipaddr, port);
}

void sccp_dump_message_transmitting(const struct sccp_msg *msg, const char *ipaddr, int port)
{
	dump_message(msg, "Transmitting message", "to", ipaddr, port);
}

static void dump_message(const struct sccp_msg *msg, const char *head1, const char *head2, const char *ipaddr, int port)
{
	char body[256];
	int pad = 1;
	uint32_t msg_id;

	if (!msg) {
		return;
	}

	msg_id = letohl(msg->id);

	/* don't dump these messages */
	if (msg_id == KEEP_ALIVE_MESSAGE || msg_id == KEEP_ALIVE_ACK_MESSAGE) {
		return;
	}

	*body = '\0';
	if (sccp_msg_dump(body, sizeof(body), msg)) {
		pad = 0;
	}

	ast_verbose(
		"\n<--- %s \"%s\" %s %s:%d -->\n"
		"Length: %4u   Reserved: 0x%08X   ID: 0x%04X\n%s"
		"%s"
		"\n<------------>\n",
		head1, sccp_msg_id_str(msg_id), head2, ipaddr, port, letohl(msg->length),
		letohl(msg->reserved), msg_id, pad ? "\n" : "", body
	);
}
