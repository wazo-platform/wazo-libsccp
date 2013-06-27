#ifndef SCCP_CONFIG_H_
#define SCCP_CONFIG_H_

#include "sccp.h"

struct sccp_configs *sccp_config; /* global settings */

char *sccp_show_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
char *sccp_update_config(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);

int config_load(char *config_file, struct sccp_configs *sccp_cfg);
void sccp_config_unload(struct sccp_configs *sccp_cfg);
void destroy_device_config(struct sccp_device *device, struct sccp_configs *sccp_cfg);

#endif /* SCCP_CONFIG_H_ */
