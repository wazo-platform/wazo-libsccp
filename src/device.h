#ifndef SCCP_DEVICE_H
#define SCCP_DEVICE_H

#include <asterisk.h>
#include <asterisk/linkedlists.h>

#include <stdint.h>

struct sccp_line {

	char name[80];
	struct sccp_device *device;
	AST_LIST_ENTRY(sccp_line) list;
	AST_LIST_ENTRY(sccp_line) list_per_device;
};

struct sccp_device {

	char name[80];
	uint8_t registered;
	int type;
	
	AST_LIST_HEAD(, sccp_line) lines;
	AST_LIST_ENTRY(sccp_device) list;
};

AST_LIST_HEAD(list_line, sccp_line);
AST_LIST_HEAD(list_device, sccp_device);

extern struct list_line list_line; /* global */
extern struct list_device list_device; /* global */

#endif /* SCCP_DEVICE_H */
