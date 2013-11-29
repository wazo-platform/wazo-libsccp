#include <asterisk.h>

#include "sccp_line.h"
#include "sccp_config.h"

static struct ast_variable *add_var(const char *buf, struct ast_variable *list);

struct sccp_line *sccp_line_create(const char *name, struct sccp_configs *sccp_cfg)
{
	struct sccp_line* line = ast_calloc(1, sizeof(*line));

	if (!line) {
		return NULL;
	}

	ast_copy_string(line->name, name, sizeof(line->name));
	ast_copy_string(line->context, sccp_cfg->context, sizeof(line->context));
	ast_copy_string(line->language, sccp_cfg->language, sizeof(line->language));
	line->state = SCCP_ONHOOK;
	line->device = NULL;
	line->active_subchan = NULL;
	line->serial_callid = 1;
	line->callfwd = SCCP_CFWD_INACTIVE;
	line->codec_pref = sccp_cfg->codec_pref;
	line->caps = ast_format_cap_alloc_nolock();
	if (!line->caps) {
		ast_free(line);
		return NULL;
	}
	ast_format_cap_copy(line->caps, sccp_cfg->caps);

	AST_RWLIST_HEAD_INIT(&line->subchans);

	return line;
}

void sccp_line_destroy(struct sccp_line *line)
{
	ast_format_cap_destroy(line->caps);
	ast_variables_destroy(line->chanvars);
	AST_RWLIST_HEAD_DESTROY(&line->subchans);

	ast_free(line);
}

struct sccp_line *sccp_line_find_by_name(const char *name, struct list_line *list_line)
{
	struct sccp_line *line_itr;

	if (!name) {
		ast_log(LOG_ERROR, "name is NULL\n");
		return NULL;
	}

	if (!list_line) {
		ast_log(LOG_ERROR, "list_line is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(list_line);
	AST_RWLIST_TRAVERSE(list_line, line_itr, list) {
		if (!strncmp(line_itr->name, name, sizeof(line_itr->name)))
			break;
	}
	AST_RWLIST_UNLOCK(list_line);

	return line_itr;
}

struct sccp_subchannel *sccp_line_get_next_offhook_subchan(struct sccp_line *line)
{
	struct sccp_subchannel *subchan_itr;

	AST_RWLIST_RDLOCK(&line->subchans);
	AST_RWLIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr->state == SCCP_OFFHOOK)
			break;
	}
	AST_RWLIST_UNLOCK(&line->subchans);

	return subchan_itr;
}

struct sccp_subchannel *sccp_line_get_next_ringin_subchan(struct sccp_line *line)
{
	struct sccp_subchannel *subchan_itr;

	AST_RWLIST_RDLOCK(&line->subchans);
	AST_RWLIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr->state == SCCP_RINGIN)
			break;
	}
	AST_RWLIST_UNLOCK(&line->subchans);

	return subchan_itr;
}

struct sccp_subchannel *sccp_line_get_subchan(struct sccp_line *line, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan_itr;

	AST_RWLIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr->id == subchan_id)
			break;
	}

	return subchan_itr;
}

void sccp_line_select_subchan(struct sccp_line *line, struct sccp_subchannel *subchan)
{
	if (!subchan) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return;
	}

	if (line->active_subchan) {
		line->active_subchan->state = line->state;
	}

	line->active_subchan = subchan;
}

void sccp_line_select_subchan_id(struct sccp_line *line, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan_itr;

	AST_RWLIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr->id == subchan_id) {
			sccp_line_select_subchan(line, subchan_itr);
			break;
		}
	}
}

void sccp_line_set_field(struct sccp_line *line, const char *name, const char *value)
{
	if (!strcasecmp(name, "cid_num")) {
		ast_copy_string(line->cid_num, value, sizeof(line->cid_num));
	} else if (!strcasecmp(name, "cid_name")) {
		ast_copy_string(line->cid_name, value, sizeof(line->cid_name));
	} else if (!strcasecmp(name, "setvar")) {
		line->chanvars = add_var(value, line->chanvars);
	} else if (!strcasecmp(name, "language")) {
		ast_copy_string(line->language, value, sizeof(line->language));
	} else if (!strcasecmp(name, "context")) {
		ast_copy_string(line->context, value, sizeof(line->language));
	} else if (!strcasecmp(name, "allow")) {
		ast_parse_allow_disallow(&line->codec_pref, line->caps, value, 1);
	} else if (!strcasecmp(name, "disallow")) {
		ast_parse_allow_disallow(&line->codec_pref, line->caps, value, 0);
	} else {
		ast_log(LOG_WARNING, "Unknown line configuration option: %s\n", name);
	}
}

static struct ast_variable *add_var(const char *buf, struct ast_variable *list)
{
	struct ast_variable *tmpvar = NULL;
	char *varname = ast_strdupa(buf), *varval = NULL;

	if ((varval = strchr(varname, '='))) {
		*varval++ = '\0';
		if ((tmpvar = ast_variable_new(varname, varval, ""))) {
			tmpvar->next = list;
			list = tmpvar;
		}
	}
	return list;
}
