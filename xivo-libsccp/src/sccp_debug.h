#ifndef SCCP_DEBUG_H
#define SCCP_DEBUG_H

struct sccp_session;
struct sccp_msg;

extern int sccp_debug;
extern char sccp_debug_addr[16];

void sccp_enable_debug(void);
void sccp_enable_debug_ip(const char *ip);
void sccp_disable_debug(void);

void dump_message_received(struct sccp_session *session, struct sccp_msg *msg);
void dump_message_transmitting(struct sccp_session *session, struct sccp_msg *msg);

#endif /* SCCP_DEBUG_H */
