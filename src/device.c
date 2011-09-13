#include "device.h"
#include "sccp.h"

struct list_line list_line = AST_LIST_HEAD_INIT_VALUE;
struct list_device list_device = AST_LIST_HEAD_INIT_VALUE;

void *device_get_button_template(struct sccp_device *device, struct button_definition_template *btl)
{
	int i;

	switch (device->type) {
		case SCCP_DEVICE_7940:
			for (i = 0; i < 2; i++) {
				(btl++)->buttonDefinition = BT_CUST_LINESPEEDDIAL;
			}
			break;

		default:
			ast_log(LOG_WARNING, "Unknown device type '%d'\n", device->type);
			break;
	}

	return btl;
}
