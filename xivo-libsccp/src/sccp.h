#ifndef SCCP_H
#define SCCP_H

#include <asterisk.h>
#include <asterisk/linkedlists.h>
#include <asterisk/module.h>

#include "sccp_device.h"
#include "sccp_config.h"

#define SCCP_MAX_PACKET_SZ 2000

struct sccp_server {

	int sockfd;
	struct addrinfo *res;
	pthread_t thread_accept;

};

struct sccp_session {

	pthread_t tid;
	time_t start_time;
	int sockfd;
	int transmit_error;
	int destroy;

	char *ipaddr;
	struct sccp_device *device;

	char inbuf[SCCP_MAX_PACKET_SZ];
	char outbuf[SCCP_MAX_PACKET_SZ];

	AST_LIST_ENTRY(sccp_session) list;
};

int codec_ast2sccp(struct ast_format *astcodec);
int sccp_server_init(struct sccp_configs *sccp_cfg);
void sccp_server_fini(void);
void sccp_rtp_fini(void);
void sccp_rtp_init(const struct ast_module_info *module_info);
int do_hangup(uint32_t line_instance, uint32_t subchan_id, struct sccp_session *session);
char *utf8_to_iso88591(char *to_convert);

#endif /* SCCP */
