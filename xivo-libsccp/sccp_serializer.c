#include <iconv.h>
#include <string.h>

#include <asterisk.h>
#include <asterisk/logger.h>
#include <asterisk/utils.h>

#include "sccp_serializer.h"
#include "sccp_utils.h"

#define MIN_TOTAL_LENGTH 12
#define MAX_TOTAL_LENGTH sizeof(struct sccp_msg)
#define SCCP_MSG_LENGTH_OFFSET 8

void sccp_deserializer_init(struct sccp_deserializer *deserializer, int fd)
{
	deserializer->start = 0;
	deserializer->end = 0;
	deserializer->fd = fd;
}

int sccp_deserializer_read(struct sccp_deserializer *deserializer)
{
	ssize_t n;
	size_t bytes_left;

	bytes_left = sizeof(deserializer->buf) - deserializer->end;
	if (!bytes_left) {
		ast_log(LOG_WARNING, "sccp deserializer read failed: buffer is full\n");
		return SCCP_DESERIALIZER_FULL;
	}

	n = read(deserializer->fd, &deserializer->buf[deserializer->end], bytes_left);
	if (n == -1) {
		ast_log(LOG_ERROR, "sccp deserializer read failed: read: %s\n", strerror(errno));
		return SCCP_DESERIALIZER_ERROR;
	} else if (n == 0) {
		return SCCP_DESERIALIZER_EOF;
	}

	deserializer->end += (size_t) n;

	return 0;
}

int sccp_deserializer_pop(struct sccp_deserializer *deserializer, struct sccp_msg **msg)
{
	size_t avail_bytes;
	size_t new_start;
	size_t total_length;
	uint32_t msg_length;

	avail_bytes = deserializer->end - deserializer->start;
	if (avail_bytes < MIN_TOTAL_LENGTH) {
		return SCCP_DESERIALIZER_NOMSG;
	}

	memcpy(&msg_length, &deserializer->buf[deserializer->start], sizeof(msg_length));
	total_length = letohl(msg_length) + SCCP_MSG_LENGTH_OFFSET;
	if (total_length < MIN_TOTAL_LENGTH || total_length > MAX_TOTAL_LENGTH) {
		return SCCP_DESERIALIZER_MALFORMED;
	} else if (avail_bytes < total_length) {
		return SCCP_DESERIALIZER_NOMSG;
	}

	memcpy(&deserializer->msg, &deserializer->buf[deserializer->start], total_length);
	*msg = &deserializer->msg;

	new_start = deserializer->start + total_length;
	if (new_start == deserializer->end) {
		deserializer->start = 0;
		deserializer->end = 0;
	} else {
		deserializer->start = new_start;
	}

	return 0;
}

static const uint8_t softkey_default_onhook[] = {
	SOFTKEY_REDIAL,
	SOFTKEY_NEWCALL,
	SOFTKEY_CFWDALL,
	SOFTKEY_DND,
};

static const uint8_t softkey_default_connected[] = {
	SOFTKEY_HOLD,
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
};

static const uint8_t softkey_default_onhold[] = {
	SOFTKEY_RESUME,
	SOFTKEY_NEWCALL,
};

static const uint8_t softkey_default_ringin[] = {
	SOFTKEY_ANSWER,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_ringout[] = {
	SOFTKEY_NONE,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_offhook[] = {
	SOFTKEY_REDIAL,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_dialintransfer[] = {
	SOFTKEY_REDIAL,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_connintransfer[] = {
	SOFTKEY_NONE,
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
};

static const uint8_t softkey_default_callfwd[] = {
	SOFTKEY_BKSPC,
	SOFTKEY_CFWDALL,
};

struct softkey_definitions {
	const uint8_t mode;
	const uint8_t *defaults;
	const int count;
};

static const struct softkey_definitions softkey_default_definitions[] = {
	{KEYDEF_ONHOOK, softkey_default_onhook, ARRAY_LEN(softkey_default_onhook)},
	{KEYDEF_CONNECTED, softkey_default_connected, ARRAY_LEN(softkey_default_connected)},
	{KEYDEF_ONHOLD, softkey_default_onhold, ARRAY_LEN(softkey_default_onhold)},
	{KEYDEF_RINGIN, softkey_default_ringin, ARRAY_LEN(softkey_default_ringin)},
	{KEYDEF_RINGOUT, softkey_default_ringout, ARRAY_LEN(softkey_default_ringout)},
	{KEYDEF_OFFHOOK, softkey_default_offhook, ARRAY_LEN(softkey_default_offhook)},
	{KEYDEF_CONNINTRANSFER, softkey_default_connintransfer, ARRAY_LEN(softkey_default_connintransfer)},
	{KEYDEF_DIALINTRANSFER, softkey_default_dialintransfer, ARRAY_LEN(softkey_default_dialintransfer)},
	{KEYDEF_CALLFWD, softkey_default_callfwd, ARRAY_LEN(softkey_default_callfwd)},
};

static struct softkey_template_definition softkey_template_default[] = {
	{"\x80\x01", SOFTKEY_REDIAL},
	{"\x80\x02", SOFTKEY_NEWCALL},
	{"\x80\x03", SOFTKEY_HOLD},
	{"\x80\x04", SOFTKEY_TRNSFER},
	{"\x80\x05", SOFTKEY_CFWDALL},
	{"\x80\x06", SOFTKEY_CFWDBUSY},
	{"\x80\x07", SOFTKEY_CFWDNOANSWER},
	{"\x80\x08", SOFTKEY_BKSPC},
	{"\x80\x09", SOFTKEY_ENDCALL},
	{"\x80\x0A", SOFTKEY_RESUME},
	{"\x80\x0B", SOFTKEY_ANSWER},
	{"\x80\x0C", SOFTKEY_INFO},
	{"\x80\x0D", SOFTKEY_CONFRN},
	{"\x80\x0E", SOFTKEY_PARK},
	{"\x80\x0F", SOFTKEY_JOIN},
	{"\x80\x10", SOFTKEY_MEETME},
	{"\x80\x11", SOFTKEY_PICKUP},
	{"\x80\x12", SOFTKEY_GPICKUP},
	{"Dial", 0x13}, // Dial
	{"\200\77", SOFTKEY_DND},
};

static int utf8_to_iso88591(char *out, const char *in, size_t n)
{
	iconv_t cd;
	char *inbuf = (char *) in;
	char *outbuf = out;
	size_t outbytesleft;
	size_t inbytesleft;
	size_t iconv_value;
	int ret;

	/* A: n > 0 */

	cd = iconv_open("ISO-8859-1//TRANSLIT", "UTF-8");
	if (cd == (iconv_t) -1) {
		ast_log(LOG_ERROR, "utf8_to_iso88591 failed: iconv_open: %s\n", strerror(errno));
		return -1;
	}

	inbytesleft = strlen(in);
	outbytesleft = n - 1;

	iconv_value = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
	if (iconv_value == (size_t) -1) {
		ast_log(LOG_ERROR, "utf8_to_iso88591 failed: iconv: %s\n", strerror(errno));
		ret = -1;
	} else {
		*outbuf = '\0';
		ret = 0;
	}

	iconv_close(cd);

	return ret;
}

static void set_msg_header(struct sccp_msg *msg, uint32_t data_length, uint32_t msg_id)
{
	/* XXX we should probably zero out the memory here so that we don't leak any information
	 *	   (and since this function is called by every push_xxx functions
	 */
	msg->length = htolel(4 + data_length);
	msg->reserved = htolel(0);
	msg->id = htolel(msg_id);
}

void sccp_serializer_init(struct sccp_serializer *szer, int fd)
{
	szer->fd = fd;
	szer->error = 0;
	szer->proto_version = 0;
}

void sccp_serializer_set_proto_version(struct sccp_serializer *szer, uint8_t proto_version)
{
	szer->proto_version = proto_version;
}

int sccp_serializer_push_button_template_res(struct sccp_serializer *szer, struct button_definition *definition, size_t n)
{
	struct sccp_msg *msg = &szer->msg;
	size_t i;

	set_msg_header(msg, sizeof(struct button_template_res_message), BUTTON_TEMPLATE_RES_MESSAGE);

	for (i = 0; i < n; i++) {
		msg->data.buttontemplate.definition[i] = definition[i];
	}

	for (; i < MAX_BUTTON_DEFINITION; i++) {
		msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
		msg->data.buttontemplate.definition[i].lineInstance = htolel(0);
	}

	msg->data.buttontemplate.buttonOffset = htolel(0);
	msg->data.buttontemplate.buttonCount = htolel(n);
	msg->data.buttontemplate.totalButtonCount = htolel(n);

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_capabilities_req(struct sccp_serializer *szer)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, 0, CAPABILITIES_REQ_MESSAGE);

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_config_status_res(struct sccp_serializer *szer, const char *name, uint32_t line_count, uint32_t speeddial_count)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct config_status_res_message), CONFIG_STATUS_RES_MESSAGE);

	ast_copy_string(msg->data.configstatus.deviceName, name, sizeof(msg->data.configstatus.deviceName));
	msg->data.configstatus.stationUserId = htolel(0);
	msg->data.configstatus.stationInstance = htolel(1);
	msg->data.configstatus.numberLines = htolel(line_count);
	msg->data.configstatus.numberSpeedDials = htolel(speeddial_count);

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_clear_message(struct sccp_serializer *szer)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, 0, CLEAR_NOTIFY_MESSAGE);

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_feature_status(struct sccp_serializer *szer, uint32_t instance, enum sccp_button_type type, enum sccp_blf_status status, const char *label)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct feature_stat_message), FEATURE_STAT_MESSAGE);

	msg->data.featurestatus.bt_instance = htolel(instance);
	msg->data.featurestatus.type = htolel(type);
	msg->data.featurestatus.status = htolel(status);
	ast_copy_string(msg->data.featurestatus.label, label, sizeof(msg->data.featurestatus.label));

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_forward_status_res(struct sccp_serializer *szer, uint32_t line_instance, const char *extension, uint32_t status)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct forward_status_res_message), FORWARD_STATUS_RES_MESSAGE);

	msg->data.forwardstatus.status = htolel(status);
	msg->data.forwardstatus.lineInstance = htolel(line_instance);
	msg->data.forwardstatus.cfwdAllStatus = htolel(status);
	ast_copy_string(msg->data.forwardstatus.cfwdAllNumber, extension, sizeof(msg->data.forwardstatus.cfwdAllNumber));
	msg->data.forwardstatus.cfwdBusyStatus = htolel(0);
	msg->data.forwardstatus.cfwdBusyNumber[0] = '\0';
	msg->data.forwardstatus.cfwdNoAnswerStatus = htolel(0);
	msg->data.forwardstatus.cfwdNoAnswerNumber[0] = '\0';

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_keep_alive_ack(struct sccp_serializer *szer)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, 0, KEEP_ALIVE_ACK_MESSAGE);

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_line_status_res(struct sccp_serializer *szer, uint32_t line_instance, const char *cid_name, const char *cid_num)
{
	struct sccp_msg *msg = &szer->msg;
	char *display_name;

	set_msg_header(msg, sizeof(struct line_status_res_message), LINE_STATUS_RES_MESSAGE);

	/* XXX would be nicer to use a "version driver" for function where treatment differs
	 *     from one version to the other
	 */
	msg->data.linestatus.lineNumber = htolel(line_instance);
	ast_copy_string(msg->data.linestatus.lineDirNumber, cid_num, sizeof(msg->data.linestatus.lineDirNumber));
	if (szer->proto_version <= 11) {
		display_name = ast_alloca(sizeof(msg->data.linestatus.lineDisplayName));
		utf8_to_iso88591(display_name, cid_name, sizeof(msg->data.linestatus.lineDisplayName));
		ast_copy_string(msg->data.linestatus.lineDisplayName, display_name, sizeof(msg->data.linestatus.lineDisplayName));
	} else {
		ast_copy_string(msg->data.linestatus.lineDisplayName, cid_name, sizeof(msg->data.linestatus.lineDisplayName));
	}

	ast_copy_string(msg->data.linestatus.lineDisplayAlias, cid_num, sizeof(msg->data.linestatus.lineDisplayAlias));

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_register_ack(struct sccp_serializer *szer, uint8_t proto_version, uint32_t keepalive, const char *datefmt)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct register_ack_message), REGISTER_ACK_MESSAGE);

	msg->data.regack.keepAlive = htolel(keepalive);
	msg->data.regack.secondaryKeepAlive = htolel(keepalive);
	ast_copy_string(msg->data.regack.dateTemplate, datefmt, sizeof(msg->data.regack.dateTemplate));

	/* XXX hum, this feels a bit weird, we don't use our protoversion here because,
	 *     well, let's face it, we don't know exactly what should be done
	 */
	if (proto_version <= 3) {
		msg->data.regack.protoVersion = htolel(3);
		msg->data.regack.unknown1 = htolel(0x00);
		msg->data.regack.unknown2 = htolel(0x00);
		msg->data.regack.unknown3 = htolel(0x00);
	} else if (proto_version <= 10) {
		msg->data.regack.protoVersion = htolel(proto_version);
		msg->data.regack.unknown1 = htolel(0x20);
		msg->data.regack.unknown2 = htolel(0x00);
		msg->data.regack.unknown3 = htolel(0xFE);
	} else {
		msg->data.regack.protoVersion = htolel(11);
		msg->data.regack.unknown1 = htolel(0x20);
		msg->data.regack.unknown2 = htolel(0xF1);
		msg->data.regack.unknown3 = htolel(0xFF);
	}

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_register_rej(struct sccp_serializer *szer)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct register_rej_message), REGISTER_REJ_MESSAGE);

	/* never seen where the phone display the errmsg, so don't use it */
	msg->data.regrej.errMsg[0] = '\0';

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_select_softkeys(struct sccp_serializer *szer, uint32_t line_instance, uint32_t callid, enum sccp_softkey_status softkey)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct select_soft_keys_message), SELECT_SOFT_KEYS_MESSAGE);

	msg->data.selectsoftkey.lineInstance = htolel(line_instance);
	msg->data.selectsoftkey.callInstance = htolel(callid);
	msg->data.selectsoftkey.softKeySetIndex = htolel(softkey);
	msg->data.selectsoftkey.validKeyMask = htolel(0xFFFFFFFF);

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_softkey_set_res(struct sccp_serializer *szer)
{
	struct sccp_msg *msg = &szer->msg;
	const struct softkey_definitions *softkeymode;
	int keyset_count = ARRAY_LEN(softkey_default_definitions);
	int i;
	int j;

	set_msg_header(msg, sizeof(struct softkey_set_res_message), SOFTKEY_SET_RES_MESSAGE);

	msg->data.softkeysets.softKeySetOffset = htolel(0);
	msg->data.softkeysets.softKeySetCount = htolel(keyset_count);
	msg->data.softkeysets.totalSoftKeySetCount = htolel(keyset_count);

	for (i = 0; i < keyset_count; i++) {
		softkeymode = &softkey_default_definitions[i];
		for (j = 0; j < softkeymode->count; j++) {
			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyTemplateIndex[j]
				= htolel(softkeymode->defaults[j]);

			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyInfoIndex[j]
				= htolel(softkeymode->defaults[j]);
		}
	}

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_softkey_template_res(struct sccp_serializer *szer)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct softkey_template_res_message), SOFTKEY_TEMPLATE_RES_MESSAGE);

	msg->data.softkeytemplate.softKeyOffset = htolel(0);
	msg->data.softkeytemplate.softKeyCount = htolel(ARRAY_LEN(softkey_template_default));
	msg->data.softkeytemplate.totalSoftKeyCount = htolel(ARRAY_LEN(softkey_template_default));
	memcpy(msg->data.softkeytemplate.softKeyTemplateDefinition, softkey_template_default, sizeof(softkey_template_default));

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_speeddial_stat_res(struct sccp_serializer *szer, uint32_t index, const char *extension, const char *label)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct speeddial_stat_res_message), SPEEDDIAL_STAT_RES_MESSAGE);

	msg->data.speeddialstatus.instance = htolel(index);
	memcpy(msg->data.speeddialstatus.extension, extension, sizeof(msg->data.speeddialstatus.extension));
	memcpy(msg->data.speeddialstatus.label, label, sizeof(msg->data.speeddialstatus.label));

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_time_date_res(struct sccp_serializer *szer)
{
	struct sccp_msg *msg = &szer->msg;
	struct timeval now;
	struct ast_tm cmtime;

	set_msg_header(msg, sizeof(struct time_date_res_message), DATE_TIME_RES_MESSAGE);

	now = ast_tvnow();
	ast_localtime(&now, &cmtime, NULL);

	msg->data.timedate.year = htolel(cmtime.tm_year + 1900);
	msg->data.timedate.month = htolel(cmtime.tm_mon + 1);
	msg->data.timedate.dayOfWeek = htolel(cmtime.tm_wday);
	msg->data.timedate.day = htolel(cmtime.tm_mday);
	msg->data.timedate.hour = htolel(cmtime.tm_hour);
	msg->data.timedate.minute = htolel(cmtime.tm_min);
	msg->data.timedate.seconds = htolel(cmtime.tm_sec);
	msg->data.timedate.milliseconds = htolel(0);
	msg->data.timedate.systemTime = htolel(now.tv_sec);

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push_reset(struct sccp_serializer *szer, enum sccp_reset_type type)
{
	struct sccp_msg *msg = &szer->msg;

	set_msg_header(msg, sizeof(struct reset_message), RESET_MESSAGE);

	msg->data.reset.type = htolel(type);

	return sccp_serializer_push(szer, msg);
}

int sccp_serializer_push(struct sccp_serializer *szer, struct sccp_msg *msg)
{
	size_t count = letohl(msg->length) + SCCP_MSG_LENGTH_OFFSET;
	ssize_t n;

	if (szer->error) {
		ast_log(LOG_DEBUG, "sccp serializer push failed: in error state\n");
		return -1;
	}

	n = write(szer->fd, msg, count);
	if (n == (ssize_t) count) {
		return 0;
	}

	szer->error = 1;
	if (n == -1) {
		ast_log(LOG_WARNING, "sccp serializer push failed: write: %s\n", strerror(errno));
	} else {
		ast_log(LOG_WARNING, "sccp serializer push failed: write wrote less bytes than expected\n");
	}

	return -1;
}
