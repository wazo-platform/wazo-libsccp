#ifndef SCCP_DEBUG_H_
#define SCCP_DEBUG_H_

struct sccp_msg;

extern int sccp_debug;
extern char sccp_debug_addr[16];

#define sccp_debug_enabled(ipaddr) (sccp_debug && (*sccp_debug_addr == '\0' || !strcmp(sccp_debug_addr, ipaddr)))

void sccp_enable_debug(void);

void sccp_enable_debug_ip(const char *ip);

void sccp_disable_debug(void);

void sccp_dump_message_received(const struct sccp_msg *msg, const char *ipaddr, int port);

void sccp_dump_message_transmitting(const struct sccp_msg *msg, const char *ipaddr, int port);

#endif /* SCCP_DEBUG_H_ */
