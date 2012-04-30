#include "device.h"
#include "sccp.h"

void device_unregister(struct sccp_device *device)
{
	struct sccp_line *line_itr = NULL;
	struct sccp_subchannel *subchan = NULL;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	device->registered = DEVICE_REGISTERED_FALSE;

	AST_RWLIST_RDLOCK(&device->lines);
	AST_RWLIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		if (line_itr->active_subchan != NULL)
			if (line_itr->active_subchan->channel != NULL) {
				subchan = line_itr->active_subchan;
				break;
			}
	}
	AST_RWLIST_UNLOCK(&device->lines);

	if (subchan != NULL)
		ast_queue_hangup(subchan->channel);

	return;
}

void device_register(struct sccp_device *device,
			int8_t protoVersion,
			int type,
			void *session,
			struct sockaddr_in localip)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	device->registered = DEVICE_REGISTERED_TRUE;
	device->protoVersion = protoVersion;
	device->type = type;
	device->session = session;
	device->localip = localip;

	return;
}

void device_prepare(struct sccp_device *device)
{
	struct sccp_line *line_itr = NULL;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

//	ast_mutex_lock(&device->lock);

	device->active_line = NULL;
	device->active_line_cnt = 0;

	while ((line_itr = TAILQ_FIRST(&device->qline))) {
		TAILQ_REMOVE(&device->qline, line_itr, qline);
	}

	device->exten[0] = '\0';

//	ast_mutex_unlock(&device->lock);

	AST_RWLIST_RDLOCK(&device->lines);
	AST_RWLIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		set_line_state(line_itr, SCCP_ONHOOK);
	}
	AST_RWLIST_UNLOCK(&device->lines);

	return;
}

struct sccp_device *find_device_by_name(const char *name, struct list_device *list_device)
{
	struct sccp_device *device_itr = NULL;

	if (name == NULL) {
		ast_log(LOG_DEBUG, "name is NULL\n");
		return NULL;
	}

	if (list_device == NULL) {
		ast_log(LOG_DEBUG, "list_device is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(list_device);
	AST_RWLIST_TRAVERSE(list_device, device_itr, list) {
		if (!strncmp(device_itr->name, name, sizeof(device_itr->name)))
			break;
	}
	AST_RWLIST_UNLOCK(list_device);

	return device_itr;
}

struct sccp_subchannel *line_get_next_ringin_subchan(struct sccp_line *line)
{
	struct sccp_subchannel *subchan_itr = NULL;

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return NULL;
	}

	AST_LIST_LOCK(&line->subchans);
	AST_LIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr != NULL && subchan_itr->state == SCCP_RINGIN)
			break;
	}
	AST_LIST_UNLOCK(&line->subchans);

	return subchan_itr;
}

struct sccp_line *find_line_by_name(const char *name, struct list_line *list_line)
{
	struct sccp_line *line_itr = NULL;

	if (name == NULL) {
		ast_log(LOG_DEBUG, "name is NULL\n");
		return NULL;
	}

	if (list_line == NULL) {
		ast_log(LOG_DEBUG, "list_line is NULL\n");
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

struct sccp_line *device_get_line(struct sccp_device *device, int instance)
{
	struct sccp_line *line_itr = NULL;

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return NULL;
	}

	AST_RWLIST_RDLOCK(&device->lines);
	AST_RWLIST_TRAVERSE(&device->lines, line_itr, list_per_device) {
		if (line_itr->instance == instance)
			break;
	}
	AST_RWLIST_UNLOCK(&device->lines);
	
	return line_itr;
}

char *device_type_str(int device_type)
{
	switch (device_type) {
	case SCCP_DEVICE_7905:
		return "7905";
	case SCCP_DEVICE_7906:
		return "7906";
	case SCCP_DEVICE_7911:
		return "7911";
	case SCCP_DEVICE_7912:
		return "7912";
	case SCCP_DEVICE_7940:
		return "7940";
	case SCCP_DEVICE_7941:
		return "7941";
	case SCCP_DEVICE_7941GE:
		return "7941GE";
	case SCCP_DEVICE_7960:
		return "7960";
	case SCCP_DEVICE_7961:
		return "7961";
	default:
		return "unknown";
	}
}

char *device_regstate_str(int device_state)
{
	switch (device_state) {
	case DEVICE_REGISTERED_TRUE:
		return "Registered";
	case DEVICE_REGISTERED_FALSE:
		return "Unregistered";
	default:
		return "unknown";
	}
}

int device_type_is_supported(int device_type)
{
	int supported = 0;

	switch (device_type) {
	case SCCP_DEVICE_7905:
	case SCCP_DEVICE_7906:
	case SCCP_DEVICE_7911:
	case SCCP_DEVICE_7912:
	case SCCP_DEVICE_7940:
	case SCCP_DEVICE_7941:
	case SCCP_DEVICE_7941GE:
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

	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return -1;
	}

	if (btl == NULL) {
		ast_log(LOG_DEBUG, "button definition template is NULL\n");
		return -1;
	}

	ast_log(LOG_DEBUG, "Device type %d\n", device->type);

	switch (device->type) {
	case SCCP_DEVICE_7905:
	case SCCP_DEVICE_7906:
	case SCCP_DEVICE_7911:
	case SCCP_DEVICE_7912:
		(btl++)->buttonDefinition = BT_CUST_LINESPEEDDIAL;
		break;

	case SCCP_DEVICE_7940:
	case SCCP_DEVICE_7941:
	case SCCP_DEVICE_7941GE:
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
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return;
	}

	if (subchan == NULL) {
		ast_log(LOG_DEBUG, "subchan is NULL\n");
		return;
	}

	if (line->active_subchan)
		line->active_subchan->state = line->state;

	line->active_subchan = subchan;
}

struct sccp_subchannel *line_get_subchan(struct sccp_line *line, uint32_t subchan_id)
{
	struct sccp_subchannel *subchan_itr = NULL;

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return NULL;
	}

	AST_LIST_TRAVERSE(&line->subchans, subchan_itr, list) {
		if (subchan_itr->id == subchan_id)
			break;
	}

	return subchan_itr;
}

void line_select_subchan_id(struct sccp_line *line, uint32_t subchan_id)
{
	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return;
	}

	struct sccp_subchannel *subchan_itr;
	AST_LIST_TRAVERSE(&line->subchans, subchan_itr, list) {	
		if (subchan_itr->id == subchan_id) {
			line_select_subchan(line, subchan_itr);
			break;
		}
	}
}

void subchan_set_state(struct sccp_subchannel *subchan, int state)
{
	subchan->state = state;
}

void set_line_state(struct sccp_line *line, int state)
{
	line->state = state;
}

void device_enqueue_line(struct sccp_device *device, struct sccp_line *line)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return;
	}

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return ;
	}

//	ast_mutex_lock(&device->lock);

	TAILQ_INSERT_TAIL(&device->qline, line, qline);
	device->active_line_cnt++;

//	ast_mutex_unlock(&device->lock);
}

void device_release_line(struct sccp_device *device, struct sccp_line *line)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL");
		return;
	}

	if (line == NULL) {
		ast_log(LOG_DEBUG, "line is NULL\n");
		return;
	}

//	ast_mutex_lock(&device->lock);

	if (device->active_line == line) {
		device->active_line = NULL;
	} else {
		TAILQ_REMOVE(&device->qline, line, qline);
	}

	device->active_line_cnt--;
//	ast_mutex_unlock(&device->lock);
}

struct sccp_line *device_get_active_line(struct sccp_device *device)
{
	if (device == NULL) {
		ast_log(LOG_DEBUG, "device is NULL\n");
		return NULL;
	}

//	ast_mutex_lock(&device->lock);

	if (device->active_line == NULL) {
		if (device->qline.tqh_first != NULL) {
			device->active_line = device->qline.tqh_first;
			TAILQ_REMOVE(&device->qline, device->active_line, qline);
		} else {
			device->active_line = device->default_line;
			device->active_line_cnt++;
		}
	}

//	ast_mutex_unlock(&device->lock);

	return device->active_line;
}
