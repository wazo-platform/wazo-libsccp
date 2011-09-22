#ifndef SCCP_DEVICE_H
#define SCCP_DEVICE_H

#include <asterisk.h>
#include <asterisk/linkedlists.h>

#include <stdint.h>

#include "message.h"

#define SCCP_DEVICE_7940	8
#define SCCP_DEVICE_7941	115

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

#define SCCP_LAMP_OFF		1
#define SCCP_LAMP_ON		2
#define SCCP_LAMP_WINK		3
#define SCCP_LAMP_FLASH		4
#define SCCP_LAMP_BLINK		5

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
#define BT_NONE				0x00
#define BT_CUST_LINESPEEDDIAL		0xB0	/* line or speeddial */

#define KEYDEF_ONHOOK			0
#define KEYDEF_CONNECTED		1
#define KEYDEF_ONHOLD			2
#define KEYDEF_RINGIN			3
#define KEYDEF_OFFHOOK			4
#define KEYDEF_CONNWITHTRANS		5
#define KEYDEF_DADFD			6	/* Digits After Dialing First Digit */
#define KEYDEF_CONNWITHCONF		7
#define KEYDEF_RINGOUT			8
#define KEYDEF_OFFHOOKWITHFEAT		9
#define KEYDEF_UNKNOWN			10

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

struct softkey_template_definition softkey_template_default[] = {
	{"Redial",	0x01},
	{"NewCall",	0x02},
	{"Hold",	0x03},
	{"Trnsfer",	0x04},
	{"CFwdAll",	0x05},
	{"CFwdBusy",	0x06},
	{"CFwdNoAnswer",0x07},
	{"<<",		0x08},
	{"EndCall",	0x09},
	{"Resume",	0x0A},
	{"Answer",	0x0B},
	{"Info",	0x0C},
	{"Confrn",	0x0D},
	{"Park",	0x0E},
	{"Join",	0x0F},
	{"MeetMe",	0x10},
	{"PickUp",	0x11},
	{"GPickUp",	0x12},
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
	SOFTKEY_CFWDBUSY,
	/*SOFTKEY_GPICKUP,
	SOFTKEY_CONFRN, */
};

static const uint8_t softkey_default_connected[] = {
	SOFTKEY_HOLD,
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
	SOFTKEY_PARK,
	SOFTKEY_CFWDALL,
	SOFTKEY_CFWDBUSY,
};

static const uint8_t softkey_default_onhold[] = {
	SOFTKEY_RESUME,
	SOFTKEY_NEWCALL,
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
};

static const uint8_t softkey_default_ringin[] = {
	SOFTKEY_ANSWER,
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
};

static const uint8_t softkey_default_offhook[] = {
	SOFTKEY_REDIAL,
	SOFTKEY_ENDCALL,
	SOFTKEY_CFWDALL,
	SOFTKEY_CFWDBUSY,
	/*SOFTKEY_GPICKUP, */
};

static const uint8_t softkey_default_connwithtrans[] = {
	SOFTKEY_HOLD,
	SOFTKEY_ENDCALL,
	SOFTKEY_TRNSFER,
	SOFTKEY_PARK,
	SOFTKEY_CFWDALL,
	SOFTKEY_CFWDBUSY,
};

static const uint8_t softkey_default_dadfd[] = {
	SOFTKEY_BKSPC,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_connwithconf[] = {
	SOFTKEY_NONE,
};

static const uint8_t softkey_default_ringout[] = {
	SOFTKEY_NONE,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_offhookwithfeat[] = {
	SOFTKEY_REDIAL,
	SOFTKEY_ENDCALL,
};

static const uint8_t softkey_default_unknown[] = {
	SOFTKEY_NONE,
};

static const struct softkey_definitions softkey_default_definitions[] = {
	{KEYDEF_ONHOOK, softkey_default_onhook, sizeof(softkey_default_onhook) / sizeof(uint8_t)},
	{KEYDEF_CONNECTED, softkey_default_connected, sizeof(softkey_default_connected) / sizeof(uint8_t)},
	{KEYDEF_ONHOLD, softkey_default_onhold, sizeof(softkey_default_onhold) / sizeof(uint8_t)},
	{KEYDEF_RINGIN, softkey_default_ringin, sizeof(softkey_default_ringin) / sizeof(uint8_t)},
	{KEYDEF_OFFHOOK, softkey_default_offhook, sizeof(softkey_default_offhook) / sizeof(uint8_t)},
	{KEYDEF_CONNWITHTRANS, softkey_default_connwithtrans, sizeof(softkey_default_connwithtrans) / sizeof(uint8_t)},
	{KEYDEF_DADFD, softkey_default_dadfd, sizeof(softkey_default_dadfd) / sizeof(uint8_t)},
	{KEYDEF_CONNWITHCONF, softkey_default_connwithconf, sizeof(softkey_default_connwithconf) / sizeof(uint8_t)},
	{KEYDEF_RINGOUT, softkey_default_ringout, sizeof(softkey_default_ringout) / sizeof(uint8_t)},
	{KEYDEF_OFFHOOKWITHFEAT, softkey_default_offhookwithfeat, sizeof(softkey_default_offhookwithfeat) / sizeof(uint8_t)},
	{KEYDEF_UNKNOWN, softkey_default_unknown, sizeof(softkey_default_unknown) / sizeof(uint8_t)}
};

struct button_definition_template {
	uint8_t buttonDefinition;
};

struct sccp_line {

	char name[80];
	int instance;
	struct sccp_device *device;
	AST_LIST_ENTRY(sccp_line) list;
	AST_LIST_ENTRY(sccp_line) list_per_device;
};

struct sccp_device {

	char name[80];
	uint8_t registered;
	int type;
	AST_LIST_HEAD(, sccp_line) lines;
	AST_LIST_ENTRY(sccp_device) list;
};

AST_LIST_HEAD(list_line, sccp_line);
AST_LIST_HEAD(list_device, sccp_device);

extern struct list_line list_line;	/* global */
extern struct list_device list_device;	/* global */

int device_get_button_template(struct sccp_device *device,
							   struct button_definition_template *btl);
struct sccp_line *device_get_line(struct sccp_device *device, int instance);
int device_type_is_supported(int device_type);

#endif /* SCCP_DEVICE_H */
