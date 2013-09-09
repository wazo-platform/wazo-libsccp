#ifndef SCCP_LINE_H
#define SCCP_LINE_H

#include <asterisk/channel.h>

struct sccp_configs;

struct sccp_line {

	char name[80];
	char cid_num[80];
	char cid_name[80];

	char language[MAX_LANGUAGE];
	char context[AST_MAX_EXTENSION];
	struct ast_variable *chanvars;

	uint32_t serial_callid;
	uint32_t instance;
	uint32_t state;

	uint8_t dnd;
	uint8_t callfwd;
	uint32_t callfwd_id;
	char callfwd_exten[AST_MAX_EXTENSION];

	struct sccp_subchannel *active_subchan;
	AST_RWLIST_HEAD(, sccp_subchannel) subchans;

	struct ast_codec_pref codec_pref;
	struct sccp_device *device;

	AST_LIST_ENTRY(sccp_line) list;
	AST_LIST_ENTRY(sccp_line) list_per_device;
};

struct sccp_line *sccp_new_line(const char *name, struct sccp_configs *sccp_cfg);
void sccp_line_set_field(struct sccp_line *line, const char *name, const char *value);

#endif /* SCCP_LINE_H */
