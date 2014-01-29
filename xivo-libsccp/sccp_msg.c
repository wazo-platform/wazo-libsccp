#include "sccp_msg.h"

const char *sccp_msg_id_str(uint32_t msg_id) {
	switch (msg_id) {
	case KEEP_ALIVE_MESSAGE:
		return "keep alive";
	case REGISTER_MESSAGE:
		return "register";
	case IP_PORT_MESSAGE:
		return "ip port";
	case ENBLOC_CALL_MESSAGE:
		return "enbloc call";
	case KEYPAD_BUTTON_MESSAGE:
		return "keypad button";
	case STIMULUS_MESSAGE:
		return "stimulus";
	case OFFHOOK_MESSAGE:
		return "offhook";
	case ONHOOK_MESSAGE:
		return "onhook";
	case FORWARD_STATUS_REQ_MESSAGE:
		return "forward status req";
	case SPEEDDIAL_STAT_REQ_MESSAGE:
		return "speeddial stat req";
	case LINE_STATUS_REQ_MESSAGE:
		return "line status req";
	case CONFIG_STATUS_REQ_MESSAGE:
		return "config status req";
	case TIME_DATE_REQ_MESSAGE:
		return "time date req";
	case BUTTON_TEMPLATE_REQ_MESSAGE:
		return "button template req";
	case VERSION_REQ_MESSAGE:
		return "version req";
	case CAPABILITIES_RES_MESSAGE:
		return "capabilities res";
	case ALARM_MESSAGE:
		return "alarm";
	case OPEN_RECEIVE_CHANNEL_ACK_MESSAGE:
		return "open receive channel ack";
	case SOFTKEY_SET_REQ_MESSAGE:
		return "softkey set req";
	case SOFTKEY_EVENT_MESSAGE:
		return "softkey event";
	case UNREGISTER_MESSAGE:
		return "unregister";
	case SOFTKEY_TEMPLATE_REQ_MESSAGE:
		return "softkey template req";
	case REGISTER_AVAILABLE_LINES_MESSAGE:
		return "register available lines";
	case FEATURE_STATUS_REQ_MESSAGE:
		return "feature status req";
	case ACCESSORY_STATUS_MESSAGE:
		return "accessory status";
	case REGISTER_ACK_MESSAGE:
		return "register ack";
	case START_TONE_MESSAGE:
		return "start tone";
	case STOP_TONE_MESSAGE:
		return "stop tone";
	case SET_RINGER_MESSAGE:
		return "set ringer";
	case SET_LAMP_MESSAGE:
		return "set lamp";
	case SET_SPEAKER_MESSAGE:
		return "set speaker";
	case STOP_MEDIA_TRANSMISSION_MESSAGE:
		return "stop media transmission";
	case START_MEDIA_TRANSMISSION_MESSAGE:
		return "start media transmission";
	case CALL_INFO_MESSAGE:
		return "call info";
	case FORWARD_STATUS_RES_MESSAGE:
		return "forward status res";
	case SPEEDDIAL_STAT_RES_MESSAGE:
		return "speeddial stat res";
	case LINE_STATUS_RES_MESSAGE:
		return "line status res";
	case CONFIG_STATUS_RES_MESSAGE:
		return "config status res";
	case DATE_TIME_RES_MESSAGE:
		return "date time res";
	case BUTTON_TEMPLATE_RES_MESSAGE:
		return "button template res";
	case VERSION_RES_MESSAGE:
		return "version res";
	case CAPABILITIES_REQ_MESSAGE:
		return "capabilities req";
	case REGISTER_REJ_MESSAGE:
		return "register rej";
	case RESET_MESSAGE:
		return "reset";
	case KEEP_ALIVE_ACK_MESSAGE:
		return "keep alive ack";
	case OPEN_RECEIVE_CHANNEL_MESSAGE:
		return "open receive channel";
	case CLOSE_RECEIVE_CHANNEL_MESSAGE:
		return "close receive channel";
	case SOFTKEY_TEMPLATE_RES_MESSAGE:
		return "softkey template res";
	case SOFTKEY_SET_RES_MESSAGE:
		return "softkey set res";
	case SELECT_SOFT_KEYS_MESSAGE:
		return "select soft keys";
	case CALL_STATE_MESSAGE:
		return "call state";
	case DISPLAY_NOTIFY_MESSAGE:
		return "display notify";
	case CLEAR_NOTIFY_MESSAGE:
		return "clear notify";
	case ACTIVATE_CALL_PLANE_MESSAGE:
		return "activate call plane";
	case DIALED_NUMBER_MESSAGE:
		return "dialed number";
	case FEATURE_STAT_MESSAGE:
		return "feature stat";
	case START_MEDIA_TRANSMISSION_ACK_MESSAGE:
		return "start media transmission ack";
	}

	return "unknown";
}

const char *sccp_device_type_str(enum sccp_device_type device_type)
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
	case SCCP_DEVICE_7920:
		return "7920";
	case SCCP_DEVICE_7921:
		return "7921";
	case SCCP_DEVICE_7931:
		return "7931";
	case SCCP_DEVICE_7937:
		return "7937";
	case SCCP_DEVICE_7940:
		return "7940";
	case SCCP_DEVICE_7941:
		return "7941";
	case SCCP_DEVICE_7941GE:
		return "7941GE";
	case SCCP_DEVICE_7942:
		return "7942";
	case SCCP_DEVICE_7960:
		return "7960";
	case SCCP_DEVICE_7961:
		return "7961";
	case SCCP_DEVICE_7962:
		return "7962";
	case SCCP_DEVICE_7970:
		return "7970";
	case SCCP_DEVICE_CIPC:
		return "CIPC";
	}

	return "unknown";
}

const char *sccp_state_str(enum sccp_state state)
{
	switch (state) {
	case SCCP_OFFHOOK:
		return "Offhook";
	case SCCP_ONHOOK:
		return "Onhook";
	case SCCP_RINGOUT:
		return "Ringout";
	case SCCP_RINGIN:
		return "Ringin";
	case SCCP_CONNECTED:
		return "Connected";
	case SCCP_BUSY:
		return "Busy";
	case SCCP_CONGESTION:
		return "Congestion";
	case SCCP_HOLD:
		return "Hold";
	case SCCP_CALLWAIT:
		return "Callwait";
	case SCCP_TRANSFER:
		return "Transfer";
	case SCCP_PARK:
		return "Park";
	case SCCP_PROGRESS:
		return "Progress";
	case SCCP_INVALID:
		return "Invalid";
	}

	return "Unknown";
}
