#ifndef SCCP_H_
#define SCCP_H_

#define SCCP_LINE_PREFIX "SCCP"

#define SCCP_DEVICE_NAME_MAX 20
#define SCCP_LINE_NAME_MAX 40
#define SCCP_SPEEDDIAL_NAME_MAX 40

extern struct ast_channel_tech sccp_tech;
extern struct ast_sched_context *sccp_sched;
extern const struct ast_module_info *sccp_module_info;

#endif /* SCCP_H_ */
