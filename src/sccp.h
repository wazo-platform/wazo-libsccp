#ifndef SCCP_H
#define SCCP_H

#include <asterisk/linkedlists.h>
#include "device.h"

#define SCCP_DEFAULT_AUTH_TIMEOUT 30
#define SCCP_MAX_PACKET_SZ 2000

static struct sccp_server {

	int sockfd;
	struct addrinfo *res;
	pthread_t thread_accept;
	pthread_t thread_session;

} sccp_srv;

struct sccp_configs {

	char *bindaddr;
	char dateformat[6];
	int keepalive;
	int authtimeout;

	struct list_line list_line;
	struct list_device list_device;	
};

struct sccp_session {

	ast_mutex_t lock;
	pthread_t tid;
	time_t start_time;
	int sockfd;

	char *ipaddr;
	struct sccp_device *device;

	char inbuf[SCCP_MAX_PACKET_SZ];
	char outbuf[SCCP_MAX_PACKET_SZ];

	AST_LIST_ENTRY(sccp_session) list;
};

int codec_ast2sccp(format_t);
int sccp_server_init(struct sccp_configs *sccp_cfg);
void sccp_server_fini(void);

#endif /* SCCP */
