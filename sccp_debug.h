#ifndef SCCP_DEBUG_H_
#define SCCP_DEBUG_H_

struct sccp_msg;

void sccp_debug_enable(void);

void sccp_debug_enable_device_name(const char *name);

void sccp_debug_enable_ip(const char *ip);

void sccp_debug_disable(void);

int sccp_debug_enabled(const char *device_name, const char *ip);

void sccp_dump_message_received(const struct sccp_msg *msg, const char *ipaddr, int port);

void sccp_dump_message_transmitting(const struct sccp_msg *msg, const char *ipaddr, int port);

#endif /* SCCP_DEBUG_H_ */
