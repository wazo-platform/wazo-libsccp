#ifndef SCCP_H
#define SCCP_H

#define SCCP_DEFAULT_AUTH_TIMEOUT 30
struct sccp_configs {

	char *bindaddr;
	char dateformat[6];
	int keepalive;
	int authtimeout;

};

int sccp_server_init(void);

#endif /* SCCP */
