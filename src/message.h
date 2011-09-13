#ifndef SCCP_MESSAGE_H
#define SCCP_MESSAGE_H

#include <stdlib.h>
//#include <stddef.h>
#include <stdint.h>

/*********************
 * Protocol Messages *
 *********************/

/* message types */
#define KEEP_ALIVE_MESSAGE 0x0000
/* no additional struct */

#define REGISTER_MESSAGE 0x0001
struct register_message {
	char name[16];
	uint32_t userId;
	uint32_t instance;
	uint32_t ip;
	uint32_t type;
	uint32_t maxStreams;
};

#define BUTTON_TEMPLATE_REQ_MESSAGE 0x000E

#define ALARM_MESSAGE 0x0020
struct alarm_message {
	uint32_t alarmSeverity;
	char displayMessage[80];
	uint32_t alarmParam1;
	uint32_t alarmParam2;
};

#define SOFTKEY_SET_REQ_MESSAGE 0x0025
#define SOFTKEY_TEMPLATE_REQ_MESSAGE 0x0028

#define REGISTER_ACK_MESSAGE 0x0081
struct register_ack_message {
        uint32_t keepAlive;
        char dateTemplate[6];
        char res[2];
        uint32_t secondaryKeepAlive;
        char res2[4];
};

#define BUTTON_TEMPLATE_RES_MESSAGE 0x0097
struct button_definition {
        uint8_t instanceNumber;
        uint8_t buttonDefinition;
};

struct button_template_res_message {
        uint32_t buttonOffset;
        uint32_t buttonCount;
        uint32_t totalButtonCount;
        struct button_definition definition[42];
};

#define REGISTER_REJ_MESSAGE 0x009D
struct register_rej_message {
	char errMsg[33];
};

#define KEEP_ALIVE_ACK_MESSAGE 0x0100

#define SELECT_SOFT_KEYS_MESSAGE 0x0110
struct select_soft_keys_message {
        uint32_t instance;
        uint32_t reference;
        uint32_t softKeySetIndex;
        uint32_t validKeyMask;
};

#define SOFTKEY_TEMPLATE_RES_MESSAGE 0x0108
struct softkey_template_definition {
	char softKeyLabel[16];
	uint32_t softKeyEvent;
};

#define SOFTKEY_SET_RES_MESSAGE 0x0109
struct softkey_set_definition {
        uint8_t softKeyTemplateIndex[16];
        uint16_t softKeyInfoIndex[16];
};

struct softkey_set_res_message {
        uint32_t softKeySetOffset;
        uint32_t softKeySetCount;
        uint32_t totalSoftKeySetCount;
        struct softkey_set_definition softKeySetDefinition[16];
        uint32_t res;
};

struct softkey_template_res_message {
        uint32_t softKeyOffset;
        uint32_t softKeyCount;
        uint32_t totalSoftKeyCount;
        struct softkey_template_definition softKeyTemplateDefinition[32];
};

union sccp_data {
	struct alarm_message alarm;
	struct register_message reg;
	struct register_ack_message regack;
	struct register_rej_message regrej;
	struct button_template_res_message buttontemplate;
        struct softkey_set_res_message softkeysets;
	struct softkey_template_res_message softkeytemplate;
        struct select_soft_keys_message selectsoftkey;

};

/* message composition */
struct sccp_msg {
	uint32_t length;
	uint32_t reserved;
	uint32_t id;
	union sccp_data data;
};

struct sccp_msg *msg_alloc(size_t data_length, int message_id);

#endif /* SCCP_MESSAGE_H */
