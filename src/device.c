#include "device.h"
#include "sccp.h"

struct list_line list_line = AST_LIST_HEAD_INIT_VALUE;
struct list_device list_device = AST_LIST_HEAD_INIT_VALUE;

void device_unregister(struct sccp_device *device)
{
	struct sccp_line *line_itr = NULL;

	if (device == NULL) {
		ast_log(LOG_ERROR, "invalid parameter\n");
		return;
	}

	device->registered = DEVICE_REGISTERED_FALSE;

	AST_LIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		if (line_itr->channel != NULL)
			ast_queue_hangup(line_itr->channel);
	}

	return;
}

void device_register(struct sccp_device *device,
			int8_t protoVersion,
			int type,
			void *session)
{
	device->registered = DEVICE_REGISTERED_TRUE;
	device->protoVersion = protoVersion;
	device->type = type;
	device->session = session;

	return;
}

void device_prepare(struct sccp_device *device)
{
	struct sccp_line *line_itr = NULL;

	device->active_line = NULL;
	device->active_line_cnt = 0;

	while (line_itr = TAILQ_FIRST(&device->qline)) {
		TAILQ_REMOVE(&device->qline, line_itr, qline);
	}

	device->exten[0] = '\0';

	AST_LIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		set_line_state(line_itr, SCCP_ONHOOK);
	}

	return;
}

struct sccp_line *find_line_by_name(char *name)
{
	struct sccp_line *line_itr;
	AST_LIST_TRAVERSE(&list_line, line_itr, list) {
		if (!strncmp(line_itr->name, name, sizeof(line_itr->name)))
			return line_itr;
	}

	return NULL;
}

struct sccp_line *device_get_line(struct sccp_device *device, int instance)
{
	struct sccp_line *line_itr;
	AST_LIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		if (line_itr->instance == instance)
			return line_itr;
	}
	
	return NULL;
}

int device_type_is_supported(int device_type)
{
	int is_supported = 0;

	switch (device_type) {
		case SCCP_DEVICE_7940:
		case SCCP_DEVICE_7941:
			is_supported = 1;
			break;

		default:
			is_supported = 0;
			break;
	}

	return is_supported;
}

int device_get_button_template(struct sccp_device *device, struct button_definition_template *btl)
{
	int err = 0;
	int i = 0;

	ast_log(LOG_NOTICE, "device type %d\n", device->type);

	switch (device->type) {
		case SCCP_DEVICE_7940:
		case SCCP_DEVICE_7941:
			for (i = 0; i < 2; i++) {
				(btl++)->buttonDefinition = BT_CUST_LINESPEEDDIAL;
			}
			break;

		default:
			ast_log(LOG_WARNING, "Unknown device type '%d'\n", device->type);
			err = -1;
			break;
	}

	return err;
}

void set_line_state(struct sccp_line *line, int state)
{
	line->state = state;
}

void device_enqueue_line(struct sccp_device *device, struct sccp_line *line)
{
	if (device == NULL || line == NULL) {
		ast_log(LOG_WARNING, "Invalid parameter\n");
		return;
	}

	TAILQ_INSERT_TAIL(&device->qline, line, qline);

	device->active_line_cnt++;
}

void device_release_line(struct sccp_device *device, struct sccp_line *line)
{
	if (device == NULL || line == NULL) {
		ast_log(LOG_WARNING, "Invalid parameter\n");
		return;
	}

	if (device->active_line == line) {
		device->active_line = NULL;
	} else {
		TAILQ_REMOVE(&device->qline, line, qline);
	}

	device->active_line_cnt--;
}

int device_get_active_line(struct sccp_device *device)
{
	if (device == NULL) {
		ast_log(LOG_WARNING, "Invalid parameter\n");
		return;
	}

	if (device->active_line == NULL) {
		if (device->qline.tqh_first != NULL) {
			device->active_line = device->qline.tqh_first;
			TAILQ_REMOVE(&device->qline, device->active_line, qline);
		} else {
			device->active_line = device->default_line;
			device->active_line_cnt++;
		}
	}

	return device->active_line;
}
