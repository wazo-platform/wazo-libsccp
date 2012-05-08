#ifndef SCCP_DEVICE_H
#define SCCP_DEVICE_H

#include <asterisk.h>
#include <asterisk/channel.h>
#include <asterisk/event.h>
#include <asterisk/lock.h>
#include <asterisk/linkedlists.h>

#include <stdint.h>
#include <sys/queue.h>

#define SCCP_DEVICE_7960	7
#define SCCP_DEVICE_7940	8
#define SCCP_DEVICE_7941	115
#define SCCP_DEVICE_7911	307
#define SCCP_DEVICE_7941GE	309
#define SCCP_DEVICE_7906	369
#define SCCP_DEVICE_7905	20000
#define SCCP_DEVICE_7912	30007
#define SCCP_DEVICE_7961	30018

#define SCCP_SPEAKERON		1
#define SCCP_SPEAKEROFF		2

#define SCCP_CFWD_UNACTIVE	1
#define SCCP_CFWD_INPUTEXTEN	2
#define SCCP_CFWD_ACTIVE	3

#define SCCP_OFFHOOK		1
#define SCCP_ONHOOK		2
#define SCCP_RINGOUT		3
#define SCCP_RINGIN		4
#define SCCP_CONNECTED		5
#define SCCP_BUSY		6
#define SCCP_CONGESTION		7
#define SCCP_HOLD		8
#define SCCP_CALLWAIT		9
#define SCCP_TRANSFER		10
#define SCCP_PARK		11
#define SCCP_PROGRESS		12
#define SCCP_INVALID		14

#define SCCP_TONE_SILENCE	0x00
#define SCCP_TONE_DIAL		0x21
#define SCCP_TONE_BUSY		0x23
#define SCCP_TONE_ALERT		0x24
#define SCCP_TONE_REORDER	0x25
#define SCCP_TONE_CALLWAIT	0x2D
#define SCCP_TONE_NONE		0x7F

#define SCCP_LAMP_OFF		1
#define SCCP_LAMP_ON		2
#define SCCP_LAMP_WINK		3
#define SCCP_LAMP_FLASH		4
#define SCCP_LAMP_BLINK		5

#define SCCP_RING_OFF		1
#define SCCP_RING_INSIDE	2
#define SCCP_RING_OUTSIDE	3
#define SCCP_RING_FEATURE	4

#define STIMULUS_REDIAL			0x01
#define STIMULUS_SPEEDDIAL		0x02
#define STIMULUS_HOLD			0x03
#define STIMULUS_TRANSFER		0x04
#define STIMULUS_FORWARDALL		0x05
#define STIMULUS_FORWARDBUSY		0x06
#define STIMULUS_FORWARDNOANSWER	0x07
#define STIMULUS_DISPLAY		0x08
#define STIMULUS_LINE			0x09
#define STIMULUS_VOICEMAIL		0x0F
#define STIMULUS_AUTOANSWER		0x11
#define STIMULUS_CONFERENCE		0x7D
#define STIMULUS_CALLPARK		0x7E
#define STIMULUS_CALLPICKUP		0x7F
#define STIMULUS_NONE			0xFF

/* Button types */
#define BT_REDIAL			STIMULUS_REDIAL
#define BT_SPEEDDIAL			STIMULUS_SPEEDDIAL
#define BT_HOLD				STIMULUS_HOLD
#define BT_TRANSFER			STIMULUS_TRANSFER
#define BT_FORWARDALL			STIMULUS_FORWARDALL
#define BT_FORWARDBUSY			STIMULUS_FORWARDBUSY
#define BT_FORWARDNOANSWER		STIMULUS_FORWARDNOANSWER
#define BT_DISPLAY			STIMULUS_DISPLAY
#define BT_LINE				STIMULUS_LINE
#define BT_VOICEMAIL			STIMULUS_VOICEMAIL
#define BT_AUTOANSWER			STIMULUS_AUTOANSWER
#define BT_CONFERENCE			STIMULUS_CONFERENCE
#define BT_CALLPARK			STIMULUS_CALLPARK
#define BT_CALLPICKUP			STIMULUS_CALLPICKUP
#define BT_NONE				STIMULUS_NONE
#define BT_CUST_LINESPEEDDIAL		0xB0	/* line or speeddial */

#define KEYDEF_ONHOOK			0
#define KEYDEF_CONNECTED		1
#define KEYDEF_ONHOLD			2
#define KEYDEF_RINGIN			3
#define KEYDEF_OFFHOOK			4
#define KEYDEF_CONNINTRANSFER		5
#define KEYDEF_CALLFWD			6
//#define KEYDEF_CONNWITHCONF		7
//#define KEYDEF_RINGOUT			8
#define KEYDEF_AUTOANSWER		9
//#define KEYDEF_UNKNOWN			10

#define SOFTKEY_NONE			0x00
#define SOFTKEY_REDIAL			0x01
#define SOFTKEY_NEWCALL			0x02
#define SOFTKEY_HOLD			0x03
#define SOFTKEY_TRNSFER			0x04
#define SOFTKEY_CFWDALL			0x05
#define SOFTKEY_CFWDBUSY		0x06
#define SOFTKEY_CFWDNOANSWER		0x07
#define SOFTKEY_BKSPC			0x08
#define SOFTKEY_ENDCALL			0x09
#define SOFTKEY_RESUME			0x0A
#define SOFTKEY_ANSWER			0x0B
#define SOFTKEY_INFO			0x0C
#define SOFTKEY_CONFRN			0x0D
#define SOFTKEY_PARK			0x0E
#define SOFTKEY_JOIN			0x0F
#define SOFTKEY_MEETME			0x10
#define SOFTKEY_PICKUP			0x11
#define SOFTKEY_GPICKUP			0x12
#define SOFTKEY_DIAL			0x13
#define SOFTKEY_CANCEL			0x08

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
	SOFTKEY_NEWCALL,
	SOFTKEY_CFWDALL,
};

static const uint8_t softkey_default_connected[] = {
	SOFTKEY_NEWCALL,
	SOFTKEY_HOLD,
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
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
	SOFTKEY_DIAL,
};

static const uint8_t softkey_default_connintransfer[] = {
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
};

static const uint8_t softkey_default_callfwd[] = {
	SOFTKEY_CANCEL,
	SOFTKEY_CFWDALL,
};

/*static const uint8_t softkey_default_connwithconf[] = {
	SOFTKEY_NONE,
};*/

/*static const uint8_t softkey_default_ringout[] = {
	SOFTKEY_NONE,
	SOFTKEY_ENDCALL,
};*/

static const uint8_t softkey_default_autoanswer[] = {
	SOFTKEY_NONE,
};

/*static const uint8_t softkey_default_unknown[] = {
	SOFTKEY_NONE,
};*/

static const struct softkey_definitions softkey_default_definitions[] = {
	{KEYDEF_ONHOOK, softkey_default_onhook, sizeof(softkey_default_onhook) / sizeof(uint8_t)},
	{KEYDEF_CONNECTED, softkey_default_connected, sizeof(softkey_default_connected) / sizeof(uint8_t)},
	{KEYDEF_ONHOLD, softkey_default_onhold, sizeof(softkey_default_onhold) / sizeof(uint8_t)},
	{KEYDEF_RINGIN, softkey_default_ringin, sizeof(softkey_default_ringin) / sizeof(uint8_t)},
	{KEYDEF_OFFHOOK, softkey_default_offhook, sizeof(softkey_default_offhook) / sizeof(uint8_t)},
	{KEYDEF_CONNINTRANSFER, softkey_default_connintransfer, sizeof(softkey_default_connintransfer) / sizeof(uint8_t)},
	{KEYDEF_CALLFWD, softkey_default_callfwd, sizeof(softkey_default_callfwd) / sizeof(uint8_t)},
//	{KEYDEF_CONNWITHCONF, softkey_default_connwithconf, sizeof(softkey_default_connwithconf) / sizeof(uint8_t)},
//	{KEYDEF_RINGOUT, softkey_default_ringout, sizeof(softkey_default_ringout) / sizeof(uint8_t)},
	{KEYDEF_AUTOANSWER, softkey_default_autoanswer, sizeof(softkey_default_autoanswer) / sizeof(uint8_t)},
//	{KEYDEF_UNKNOWN, softkey_default_unknown, sizeof(softkey_default_unknown) / sizeof(uint8_t)}
};

struct button_definition_template {
	uint8_t buttonDefinition;
};

struct sccp_subchannel {

	uint32_t id;
	uint32_t state;
	struct ast_rtp_instance *rtp;
	struct sccp_line *line;
	struct ast_channel *channel;
	struct sccp_subchannel *related;
	AST_LIST_ENTRY(sccp_subchannel) list;
};

struct sccp_line {

	ast_mutex_t lock;

	char name[80];
	char cid_num[80];
	char cid_name[80];

	char language[MAX_LANGUAGE];
	struct ast_variable *chanvars;

	uint32_t serial_callid;
	uint32_t instance;
	uint32_t state;

	uint8_t callfwd;
	uint32_t callfwd_id;
	char callfwd_exten[AST_MAX_EXTENSION];

	uint32_t count_subchan;
	struct sccp_subchannel *active_subchan;
	AST_LIST_HEAD(, sccp_subchannel) subchans;

	struct ast_codec_pref codec_pref;
	struct sccp_device *device;

	//TAILQ_ENTRY(sccp_line) qline;
	AST_LIST_ENTRY(sccp_line) list;
	AST_LIST_ENTRY(sccp_line) list_per_device;
};

#define DEVICE_REGISTERED_TRUE	0x1
#define DEVICE_REGISTERED_FALSE	0x2

struct sccp_device {

	ast_mutex_t lock;

	char name[80];
	int type;
	int state;
	uint8_t protoVersion;
	uint32_t station_port;
	struct sockaddr_in localip;

	char voicemail[AST_MAX_EXTENSION];
	struct ast_event_sub *mwi_event_sub;

	char exten[AST_MAX_EXTENSION];
	pthread_t lookup_thread;
	int lookup;

	uint8_t autoanswer; /* Default mic off */

	uint8_t registered;
	uint32_t line_count;
	uint32_t speeddial_count;

	format_t codecs;
	struct ast_codec_pref codec_pref;

	void *session;

	struct sccp_line *default_line;
	struct sccp_line *active_line;
	uint32_t active_line_cnt;

	//TAILQ_HEAD(, sccp_line) qlines;
	AST_RWLIST_HEAD(, sccp_line) lines;
	AST_LIST_ENTRY(sccp_device) list;
};

AST_RWLIST_HEAD(list_line, sccp_line);
AST_RWLIST_HEAD(list_device, sccp_device);

void device_unregister(struct sccp_device *device);
void device_register(struct sccp_device *device,
			int8_t protoVersion,
			int type,
			void *session,
			struct sockaddr_in localip);
void device_prepare(struct sccp_device *device);
struct sccp_line *find_line_by_name(const char *name, struct list_line *list_line);
struct sccp_device *find_device_by_name(const char *name, struct list_device *list_device);
struct sccp_line *device_get_line(struct sccp_device *device, uint32_t instance);
int device_type_is_supported(int device_type);
int device_get_button_template(struct sccp_device *device, struct button_definition_template *btl);

struct sccp_subchannel *line_get_next_ringin_subchan(struct sccp_line *line);
void subchan_set_state(struct sccp_subchannel *subchan, int state);
void line_select_subchan(struct sccp_line *line, struct sccp_subchannel *subchan);
void line_select_subchan_id(struct sccp_line *line, uint32_t subchan_id);
struct sccp_subchannel *line_get_subchan(struct sccp_line *line, uint32_t subchan_id);
void set_line_state(struct sccp_line *line, int state);
void device_enqueue_line(struct sccp_device *device, struct sccp_line *line);
void device_release_line(struct sccp_device *device, struct sccp_line *line);
struct sccp_line *device_get_active_line(struct sccp_device *device);
char *device_regstate_str(int device_state);
int device_type_is_supported(int device_type);
char *device_type_str(int device_type);

#endif /* SCCP_DEVICE_H */
