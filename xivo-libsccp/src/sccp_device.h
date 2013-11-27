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

#include "sccp_message.h"

enum sccp_call_forward_status {
	SCCP_CFWD_INACTIVE = 1,
	SCCP_CFWD_INPUTEXTEN = 2,
	SCCP_CFWD_ACTIVE = 3,
};

enum sccp_device_registration_state {
	DEVICE_REGISTERED_TRUE = 0x1,
	DEVICE_REGISTERED_FALSE = 0x2,
};

struct sccp_subchannel {

	uint32_t id;
	enum sccp_state state;
	enum sccp_direction direction;
	uint8_t on_hold;
	uint8_t resuming;
	uint8_t autoanswer;
	uint8_t transferring;
	struct ast_sockaddr direct_media_addr;
	struct ast_rtp_instance *rtp;
	struct sccp_line *line;
	struct ast_channel *channel;
	struct sccp_subchannel *related;
	struct ast_format fmt;
	AST_LIST_ENTRY(sccp_subchannel) list;
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

struct sccp_device {

	ast_mutex_t lock;
	uint8_t destroy;
	volatile int open_receive_channel_pending;

	char name[80];
	enum sccp_device_type type;
	uint8_t proto_version;
	uint32_t station_port;
	struct sockaddr_in localip;
	struct sockaddr_in remote;

	char voicemail[AST_MAX_EXTENSION];
	struct ast_event_sub *mwi_event_sub;

	char exten[AST_MAX_EXTENSION];
	char last_exten[AST_MAX_EXTENSION];

	enum sccp_device_registration_state regstate;
	uint32_t line_count;
	uint32_t speeddial_count;

	struct ast_codec_pref codec_pref;
	struct ast_format_cap *caps;	/* Supported capabilities */

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

struct sccp_device *sccp_device_create(const char *name);
void sccp_device_destroy(struct sccp_device *device);
void sccp_device_register(struct sccp_device *device,
			int8_t protoVersion,
			enum sccp_device_type type,
			void *session,
			struct sockaddr_in localip);
void sccp_device_unregister(struct sccp_device *device);
void sccp_device_prepare(struct sccp_device *device);
int sccp_device_set_remote(struct sccp_device *device, uint32_t addr, uint32_t port);
struct sccp_speeddial *sccp_device_get_speeddial(struct sccp_device *device, uint32_t instance);
struct sccp_speeddial *sccp_device_get_speeddial_by_index(struct sccp_device *device, uint32_t index);
struct sccp_line *sccp_device_get_line(struct sccp_device *device, uint32_t instance);
int sccp_device_get_button_count(struct sccp_device *device);
int sccp_device_add_line(struct sccp_device *device, struct sccp_line *line, uint32_t instance);
void sccp_device_subscribe_speeddial_hints(struct sccp_device *device, ast_state_cb_type speeddial_hints_cb);
void sccp_device_unsubscribe_speeddial_hints(struct sccp_device *device);

struct sccp_device *find_device_by_name(const char *name, struct list_device *list_device);
char *complete_sccp_devices(const char *word, int state, struct list_device *list_device);

const char *sccp_device_regstate_str(enum sccp_device_registration_state state);
int sccp_device_type_is_supported(enum sccp_device_type device_type);

#endif /* SCCP_DEVICE_H */
