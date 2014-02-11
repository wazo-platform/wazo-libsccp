#ifndef SCCP_H_
#define SCCP_H_

#define SCCP_LINE_PREFIX "SCCP"

#define SCCP_DEVICE_NAME_MAX 20
#define SCCP_LINE_NAME_MAX 40
#define SCCP_SPEEDDIAL_NAME_MAX 40

struct ast_channel_tech;

extern struct ast_channel_tech sccp_tech;
extern struct ast_sched_context *sccp_sched;

#endif /* SCCP_H_ */
