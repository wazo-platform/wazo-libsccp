#ifndef SCCP_DEVICE_H
#define SCCP_DEVICE_H

#include <asterisk.h>
#include <asterisk/channel.h>
#include <asterisk/event.h>
#include <asterisk/lock.h>
#include <asterisk/linkedlists.h>
#include <asterisk/netsock2.h>
#include <asterisk/pbx.h>

#include <stdint.h>

enum sccp_device_type {
	SCCP_DEVICE_7960 = 7,
	SCCP_DEVICE_7940 = 8,
	SCCP_DEVICE_7941 = 115,
	SCCP_DEVICE_7911 = 307,
	SCCP_DEVICE_7941GE = 309,
	SCCP_DEVICE_7931 = 348,
	SCCP_DEVICE_7921 = 365,
	SCCP_DEVICE_7906 = 369,
	SCCP_DEVICE_7962 = 404,
	SCCP_DEVICE_7937 = 431,
	SCCP_DEVICE_7942 = 434,
	SCCP_DEVICE_7905 = 20000,
	SCCP_DEVICE_7920 = 30002,
	SCCP_DEVICE_7970 = 30006,
	SCCP_DEVICE_7912 = 30007,
	SCCP_DEVICE_CIPC = 30016,
	SCCP_DEVICE_7961 = 30018,
};

enum sccp_speaker_mode {
	SCCP_SPEAKERON = 1,
	SCCP_SPEAKEROFF = 2,
};

enum sccp_call_forward_status {
	SCCP_CFWD_INACTIVE = 1,
	SCCP_CFWD_INPUTEXTEN = 2,
	SCCP_CFWD_ACTIVE = 3,
};

enum sccp_blf_status {
	SCCP_BLF_STATUS_UNKNOWN = 0,
	SCCP_BLF_STATUS_IDLE = 1,
	SCCP_BLF_STATUS_INUSE = 2,
	SCCP_BLF_STATUS_DND = 3,
	SCCP_BLF_STATUS_ALERTING = 4,
};

enum sccp_state {
	SCCP_OFFHOOK = 1,
	SCCP_ONHOOK = 2,
	SCCP_RINGOUT = 3,
	SCCP_RINGIN = 4,
	SCCP_CONNECTED = 5,
	SCCP_BUSY = 6,
	SCCP_CONGESTION = 7,
	SCCP_HOLD = 8,
	SCCP_CALLWAIT = 9,
	SCCP_TRANSFER = 10,
	SCCP_PARK = 11,
	SCCP_PROGRESS = 12,
	SCCP_INVALID = 14,
};

enum sccp_tone {
	SCCP_TONE_SILENCE = 0x00,
	SCCP_TONE_DIAL = 0x21,
	SCCP_TONE_BUSY = 0x23,
	SCCP_TONE_ALERT = 0x24,
	SCCP_TONE_REORDER = 0x25,
	SCCP_TONE_CALLWAIT = 0x2D,
	SCCP_TONE_NONE = 0x7F,
};

enum sccp_lamp_state {
	SCCP_LAMP_OFF = 1,
	SCCP_LAMP_ON = 2,
	SCCP_LAMP_WINK = 3,
	SCCP_LAMP_FLASH = 4,
	SCCP_LAMP_BLINK = 5,
};

enum sccp_ringer_mode {
	SCCP_RING_OFF = 1,
	SCCP_RING_INSIDE = 2,
	SCCP_RING_OUTSIDE = 3,
	SCCP_RING_FEATURE = 4,
};

enum sccp_stimulus_type {
	STIMULUS_REDIAL = 0x01,
	STIMULUS_SPEEDDIAL = 0x02,
	STIMULUS_HOLD = 0x03,
	STIMULUS_TRANSFER = 0x04,
	STIMULUS_FORWARDALL = 0x05,
	STIMULUS_FORWARDBUSY = 0x06,
	STIMULUS_FORWARDNOANSWER = 0x07,
	STIMULUS_DISPLAY = 0x08,
	STIMULUS_LINE = 0x09,
	STIMULUS_VOICEMAIL = 0x0F,
	STIMULUS_AUTOANSWER = 0x11,
	STIMULUS_DND = 0x3F,
	STIMULUS_FEATUREBUTTON = 0x15,
	STIMULUS_CONFERENCE = 0x7D,
	STIMULUS_CALLPARK = 0x7E,
	STIMULUS_CALLPICKUP = 0x7F,
	STIMULUS_NONE = 0xFF,
};

enum sccp_button_type {
	BT_REDIAL = STIMULUS_REDIAL,
	BT_SPEEDDIAL = STIMULUS_SPEEDDIAL,
	BT_HOLD = STIMULUS_HOLD,
	BT_TRANSFER = STIMULUS_TRANSFER,
	BT_FORWARDALL = STIMULUS_FORWARDALL,
	BT_FORWARDBUSY = STIMULUS_FORWARDBUSY,
	BT_FORWARDNOANSWER = STIMULUS_FORWARDNOANSWER,
	BT_DISPLAY = STIMULUS_DISPLAY,
	BT_LINE = STIMULUS_LINE,
	BT_VOICEMAIL = STIMULUS_VOICEMAIL,
	BT_AUTOANSWER = STIMULUS_AUTOANSWER,
	BT_FEATUREBUTTON = STIMULUS_FEATUREBUTTON,
	BT_CONFERENCE = STIMULUS_CONFERENCE,
	BT_CALLPARK = STIMULUS_CALLPARK,
	BT_CALLPICKUP = STIMULUS_CALLPICKUP,
	BT_NONE = STIMULUS_NONE,
	BT_CUST_LINESPEEDDIAL = 0xB0,	/* line or speeddial */
};

enum sccp_softkey_status {
	KEYDEF_ONHOOK = 0,
	KEYDEF_CONNECTED = 1,
	KEYDEF_ONHOLD = 2,
	KEYDEF_RINGIN = 3,
	KEYDEF_OFFHOOK = 4,
	KEYDEF_CONNINTRANSFER = 5,
	KEYDEF_CALLFWD = 6,
	// KEYDEF_CONNWITHCONF = 7,
	// KEYDEF_RINGOUT = 8,
	KEYDEF_AUTOANSWER = 9,
	// KEYDEF_UNKNOWN = 10,
};

enum sccp_softkey_type {
	SOFTKEY_NONE = 0x00,
	SOFTKEY_REDIAL = 0x01,
	SOFTKEY_NEWCALL = 0x02,
	SOFTKEY_HOLD = 0x03,
	SOFTKEY_TRNSFER = 0x04,
	SOFTKEY_CFWDALL = 0x05,
	SOFTKEY_CFWDBUSY = 0x06,
	SOFTKEY_CFWDNOANSWER = 0x07,
	SOFTKEY_BKSPC = 0x08,
	SOFTKEY_ENDCALL = 0x09,
	SOFTKEY_RESUME = 0x0A,
	SOFTKEY_ANSWER = 0x0B,
	SOFTKEY_INFO = 0x0C,
	SOFTKEY_CONFRN = 0x0D,
	SOFTKEY_PARK = 0x0E,
	SOFTKEY_JOIN = 0x0F,
	SOFTKEY_MEETME = 0x10,
	SOFTKEY_PICKUP = 0x11,
	SOFTKEY_GPICKUP = 0x12,
	SOFTKEY_CANCEL = 0x08,
	SOFTKEY_DND = 0x14,
};

enum sccp_codecs {
	SCCP_CODEC_G711_ALAW = 2,
	SCCP_CODEC_G711_ULAW = 4,
	SCCP_CODEC_G723_1 = 9,
	SCCP_CODEC_G729A = 12,
	SCCP_CODEC_G726_32 = 82,
	SCCP_CODEC_H261 = 100,
	SCCP_CODEC_H263 = 101
};

struct softkey_definitions {
	const uint8_t mode;
	const uint8_t *defaults;
	const int count;
};

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
	SOFTKEY_NEWCALL,
};

static const uint8_t softkey_default_onhold[] = {
	SOFTKEY_NEWCALL,
	SOFTKEY_RESUME,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_ringin[] = {
	SOFTKEY_ANSWER,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_offhook[] = {
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_connintransfer[] = {
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
};

static const uint8_t softkey_default_callfwd[] = {
	SOFTKEY_CANCEL,
	SOFTKEY_CFWDALL,
};

static const uint8_t softkey_default_autoanswer[] = {
	SOFTKEY_NONE,
};

static const struct softkey_definitions softkey_default_definitions[] = {
	{KEYDEF_ONHOOK, softkey_default_onhook, sizeof(softkey_default_onhook) / sizeof(uint8_t)},
	{KEYDEF_CONNECTED, softkey_default_connected, sizeof(softkey_default_connected) / sizeof(uint8_t)},
	{KEYDEF_ONHOLD, softkey_default_onhold, sizeof(softkey_default_onhold) / sizeof(uint8_t)},
	{KEYDEF_RINGIN, softkey_default_ringin, sizeof(softkey_default_ringin) / sizeof(uint8_t)},
	{KEYDEF_OFFHOOK, softkey_default_offhook, sizeof(softkey_default_offhook) / sizeof(uint8_t)},
	{KEYDEF_CONNINTRANSFER, softkey_default_connintransfer, sizeof(softkey_default_connintransfer) / sizeof(uint8_t)},
	{KEYDEF_CALLFWD, softkey_default_callfwd, sizeof(softkey_default_callfwd) / sizeof(uint8_t)},
	{KEYDEF_AUTOANSWER, softkey_default_autoanswer, sizeof(softkey_default_autoanswer) / sizeof(uint8_t)},
};

struct sccp_subchannel {

	uint32_t id;
	enum sccp_state state;
	uint8_t on_hold;
	struct ast_sockaddr direct_media_addr;
	struct ast_rtp_instance *rtp;
	struct sccp_line *line;
	struct ast_channel *channel;
	struct sccp_subchannel *related;
	AST_LIST_ENTRY(sccp_subchannel) list;
};

struct sccp_line {

	char name[80];
	char cid_num[80];
	char cid_name[80];

	char language[MAX_LANGUAGE];
	char context[AST_MAX_EXTENSION];
	struct ast_variable *chanvars;

	uint32_t serial_callid;
	uint32_t instance;
	enum sccp_state state;

	uint8_t dnd;
	enum sccp_call_forward_status callfwd;
	uint32_t callfwd_id;
	char callfwd_exten[AST_MAX_EXTENSION];

	struct sccp_subchannel *active_subchan;
	AST_RWLIST_HEAD(, sccp_subchannel) subchans;

	struct ast_codec_pref codec_pref;
	struct sccp_device *device;

	AST_LIST_ENTRY(sccp_line) list;
	AST_LIST_ENTRY(sccp_line) list_per_device;
};

struct sccp_speeddial {

	char name[80];
	char label[80];
	char extension[AST_MAX_EXTENSION];
	uint32_t instance;
	uint32_t index;
	uint8_t blf;
	int state_id;
	int state;
	struct sccp_device *device;
	AST_LIST_ENTRY(sccp_speeddial) list;
	AST_LIST_ENTRY(sccp_speeddial) list_per_device;
};

#define DEVICE_REGISTERED_TRUE	0x1
#define DEVICE_REGISTERED_FALSE	0x2

struct sccp_device {

	ast_mutex_t lock;
	uint8_t destroy;

	char name[80];
	enum sccp_device_type type;
	enum sccp_state state;
	uint8_t proto_version;
	uint32_t station_port;
	struct sockaddr_in localip;
	struct sockaddr_in remote;
	uint8_t open_receive_msg_sent;

	char voicemail[AST_MAX_EXTENSION];
	struct ast_event_sub *mwi_event_sub;

	char exten[AST_MAX_EXTENSION];
	char last_exten[AST_MAX_EXTENSION];
	ast_cond_t lookup_cond;
	volatile int lookup;

	uint8_t autoanswer;

	uint8_t registered;
	uint32_t line_count;
	uint32_t speeddial_count;

	struct ast_format_cap *capabilities;
	struct ast_codec_pref codec_pref;

	void *session;

	// A registered device must have a default line. This means that a device
	// with no default line must not be able to register.
	struct sccp_line *default_line;

	AST_RWLIST_HEAD(, sccp_line) lines;
	AST_RWLIST_HEAD(, sccp_speeddial) speeddials;
	AST_LIST_ENTRY(sccp_device) list;
};

AST_RWLIST_HEAD(list_speeddial, sccp_speeddial);
AST_RWLIST_HEAD(list_line, sccp_line);
AST_RWLIST_HEAD(list_device, sccp_device);

void device_unregister(struct sccp_device *device);
void device_register(struct sccp_device *device,
			int8_t protoVersion,
			int type,
			void *session,
			struct sockaddr_in localip);
void device_prepare(struct sccp_device *device);
int device_set_remote(struct sccp_device *device, uint32_t addr, uint32_t port);
struct sccp_line *find_line_by_name(const char *name, struct list_line *list_line);
struct sccp_device *find_device_by_name(const char *name, struct list_device *list_device);
struct sccp_speeddial *device_get_speeddial(struct sccp_device *device, uint32_t instance);
struct sccp_speeddial *device_get_speeddial_by_index(struct sccp_device *device, uint32_t index);
struct sccp_line *device_get_line(struct sccp_device *device, uint32_t instance);
int device_supports_direct_media(struct sccp_device *device);
const char *line_state_str(enum sccp_state line_state);
int device_get_button_count(struct sccp_device *device);
char *complete_sccp_devices(const char *word, int state, struct list_device *list_device);

struct sccp_subchannel *line_get_next_ringin_subchan(struct sccp_line *line);
void subchan_set_on_hold(struct sccp_line *line, uint32_t subchan_id);
void subchan_unset_on_hold(struct sccp_line *line, uint32_t subchan_id);
void subchan_set_state(struct sccp_subchannel *subchan, enum sccp_state state);
void line_select_subchan(struct sccp_line *line, struct sccp_subchannel *subchan);
void line_select_subchan_id(struct sccp_line *line, uint32_t subchan_id);
struct sccp_subchannel *line_get_subchan(struct sccp_line *line, uint32_t subchan_id);
void set_line_state(struct sccp_line *line, enum sccp_state state);
const char *device_regstate_str(int device_state);
int device_type_is_supported(enum sccp_device_type device_type);
const char *device_type_str(enum sccp_device_type device_type);

typedef int (*state_cb_type)(char *context, char* id, struct ast_state_cb_info *info, void *data);
void speeddial_hints_unsubscribe(struct sccp_device *device);
void speeddial_hints_subscribe(struct sccp_device *device, state_cb_type speeddial_hints_cb);

#endif /* SCCP_DEVICE_H */
