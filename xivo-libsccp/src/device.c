#include "device.h"
#include "sccp.h"

struct list_line list_line = AST_LIST_HEAD_INIT_VALUE;
struct list_device list_device = AST_LIST_HEAD_INIT_VALUE;

void device_unregister(struct sccp_device *device)
{
	struct sccp_line *line_itr = NULL;

	if (device == NULL) {
		ast_log(LOG_ERROR, "Invalid parameter\n");
		return;
	}

	device->registered = DEVICE_REGISTERED_FALSE;

	AST_LIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		if (line_itr->active_subchan != NULL)
			if (line_itr->active_subchan->channel != NULL)
				ast_queue_hangup(line_itr->active_subchan->channel);
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

	ast_mutex_lock(&device->lock);

	device->active_line = NULL;
	device->active_line_cnt = 0;

	while ((line_itr = TAILQ_FIRST(&device->qline))) {
		TAILQ_REMOVE(&device->qline, line_itr, qline);
	}

	device->exten[0] = '\0';

	ast_mutex_unlock(&device->lock);

	AST_LIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		set_line_state(line_itr, SCCP_ONHOOK);
	}

	return;
}

struct sccp_line *find_line_by_name(char *name, struct list_line *list_line)
{
	struct sccp_line *line_itr = NULL;

	if (name == NULL)
		return NULL;

	AST_LIST_TRAVERSE(list_line, line_itr, list) {
		ast_log(LOG_DEBUG, "line_itr->name [%s] name[%s]\n", line_itr->name, name);
		if (!strncmp(line_itr->name, name, sizeof(line_itr->name))) {
			break;
		}
	}

	return line_itr;
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
	int supported = 0;

	switch (device_type) {
	case SCCP_DEVICE_7940:
	case SCCP_DEVICE_7941:
		supported = 1;
		break;

	case SCCP_DEVICE_7960:
	case SCCP_DEVICE_7961:
		supported = 1;
		break;

	default:
		supported = 0;
		break;
	}

	return supported;
}

int device_get_button_template(struct sccp_device *device, struct button_definition_template *btl)
{
	int err = 0;
	int i = 0;

	ast_log(LOG_DEBUG, "Device type %d\n", device->type);

	switch (device->type) {
	case SCCP_DEVICE_7940:
	case SCCP_DEVICE_7941:
		for (i = 0; i < 2; i++) {
			(btl++)->buttonDefinition = BT_CUST_LINESPEEDDIAL;
		}
		break;

	case SCCP_DEVICE_7960:
	case SCCP_DEVICE_7961:
		for (i = 0; i < 6; i++) {
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

void line_select_subchan(struct sccp_line *line, struct sccp_subchannel *subchan)
{
	if (line->active_subchan)
		line->active_subchan->state = line->state;

	/* switch subchan */
	line->active_subchan = subchan;
}

void line_select_subchan_id(struct sccp_line *line, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan_itr;
	AST_LIST_TRAVERSE(&line->subchans, subchan_itr, list) {	
		if (subchan_itr->id == subchan_id) {
			line_select_subchan(line, subchan_itr);
			break;
		}
	}
}

void set_line_state(struct sccp_line *line, int state)
{
	ast_mutex_lock(&line->lock);
	line->state = state;
	ast_mutex_unlock(&line->lock);
}

void device_enqueue_line(struct sccp_device *device, struct sccp_line *line)
{
	if (device == NULL || line == NULL) {
		ast_log(LOG_WARNING, "Invalid parameter\n");
		return;
	}

	ast_mutex_lock(&device->lock);

	TAILQ_INSERT_TAIL(&device->qline, line, qline);
	device->active_line_cnt++;

	ast_mutex_unlock(&device->lock);
}

void device_release_line(struct sccp_device *device, struct sccp_line *line)
{
	if (device == NULL || line == NULL) {
		ast_log(LOG_WARNING, "Invalid parameter\n");
		return;
	}

	ast_mutex_lock(&device->lock);

	if (device->active_line == line) {
		device->active_line = NULL;
	} else {
		TAILQ_REMOVE(&device->qline, line, qline);
	}

	device->active_line_cnt--;

	ast_mutex_unlock(&device->lock);
}

struct sccp_line *device_get_active_line(struct sccp_device *device)
{
	if (device == NULL) {
		ast_log(LOG_WARNING, "Invalid parameter\n");
		return NULL;
	}

	ast_mutex_lock(&device->lock);

	if (device->active_line == NULL) {
		if (device->qline.tqh_first != NULL) {
			device->active_line = device->qline.tqh_first;
			TAILQ_REMOVE(&device->qline, device->active_line, qline);
		} else {
			device->active_line = device->default_line;
			device->active_line_cnt++;
		}
	}

	ast_mutex_unlock(&device->lock);

	return device->active_line;
}
