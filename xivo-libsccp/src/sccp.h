#ifndef SCCP_H
#define SCCP_H

#include <asterisk.h>
#include <asterisk/linkedlists.h>
#include <asterisk/module.h>

#include "sccp_device.h"
#include "sccp_config.h"
#include "sccp_task.h"

#define SCCP_MAX_PACKET_SZ 2048

struct sccp_server {

	int sockfd;
	struct addrinfo *res;
	pthread_t thread_accept;

};

struct sccp_session {
	pthread_t tid;
	int sockfd;
	int transmit_error;
	int destroy;

	char *ipaddr;
	int port;
	struct sccp_task_runner *task_runner;

	struct sccp_device *device;

	char inbuf[SCCP_MAX_PACKET_SZ];
	char outbuf[SCCP_MAX_PACKET_SZ];

	AST_LIST_ENTRY(sccp_session) list;
};

enum sccp_codecs codec_ast2sccp(struct ast_format *format);
int do_hangup(uint32_t line_instance, uint32_t subchan_id, struct sccp_session *session);
char *utf8_to_iso88591(char *to_convert);

#endif /* SCCP */
