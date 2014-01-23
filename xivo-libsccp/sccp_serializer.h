#ifndef SCCP_SERIALIZER_H_
#define SCCP_SERIALIZER_H_

#include "sccp_msg.h"

#define SCCP_DESERIALIZER_ERROR -1
#define SCCP_DESERIALIZER_NOMSG 1
#define SCCP_DESERIALIZER_FULL 2
#define SCCP_DESERIALIZER_EOF 3
#define SCCP_DESERIALIZER_MALFORMED 4

struct sccp_deserializer {
	struct sccp_msg msg;
	size_t start;
	size_t end;
	int fd;
	char buf[2048];
};

/*!
 * \brief Initialize the deserializer.
 *
 * \param fd the file descriptor to read data from
 */
void sccp_deserializer_init(struct sccp_deserializer *dzer, int fd);

/*!
 * \brief Read data into the deserializer buffer.
 *
 * \retval 0 on success
 * \retval SCCP_DESERIALIZER_FULL if the buffer is full
 * \retval SCCP_DESERIALIZER_EOF if the end of file is reached
 * \retval SCCP_DESERIALIZER_ERROR on other failure
 */
int sccp_deserializer_read(struct sccp_deserializer *dzer);

/*!
 * \brief Get the next message from the deserializer.
 *
 * \param msg output parameter used to store the address of the parsed message
 *
 * \retval 0 on success
 * \retval SCCP_DESERIALIZER_NOMSG if no message are available
 * \retval SCCP_DESERIALIZER_MALFORMED if next message is malformed
 *
 * \note The message stored in *msg is only valid between calls to this function.
 */
int sccp_deserializer_pop(struct sccp_deserializer *dzer, struct sccp_msg **msg);

struct sccp_serializer {
	struct sccp_msg msg;
	int fd;
	int error;
	uint8_t proto_version;
};

/*!
 * \brief Initialize the serializer.
 *
 * \param fd the file descriptor to write date to
 */
void sccp_serializer_init(struct sccp_serializer *szer, int fd);

void sccp_serializer_set_proto_version(struct sccp_serializer *szer, uint8_t proto_version);

/* XXX all the function return something but we never check it, so that's a bit pointless... */

int sccp_serializer_push_button_template_res(struct sccp_serializer *szer, struct button_definition *definition, size_t n);

int sccp_serializer_push_capabilities_req(struct sccp_serializer *szer);

int sccp_serializer_push_config_status_res(struct sccp_serializer *szer, const char *name, uint32_t line_count, uint32_t speeddial_count);

int sccp_serializer_push_clear_message(struct sccp_serializer *szer);

int sccp_serializer_push_feature_status(struct sccp_serializer *szer, uint32_t instance, enum sccp_button_type type, enum sccp_blf_status status, const char *label);

int sccp_serializer_push_forward_status_res(struct sccp_serializer *szer, uint32_t line_instance, const char *extension, uint32_t status);

int sccp_serializer_push_keep_alive_ack(struct sccp_serializer *szer);

int sccp_serializer_push_line_status_res(struct sccp_serializer *szer, uint32_t line_instance, const char *cid_name, const char *cid_num);

int sccp_serializer_push_register_ack(struct sccp_serializer *szer, uint8_t proto_version, uint32_t keepalive, const char *datefmt);

int sccp_serializer_push_register_rej(struct sccp_serializer *szer);

int sccp_serializer_push_select_softkeys(struct sccp_serializer *szer, uint32_t line_instance, uint32_t callid, enum sccp_softkey_status softkey);

int sccp_serializer_push_softkey_set_res(struct sccp_serializer *szer);

int sccp_serializer_push_softkey_template_res(struct sccp_serializer *szer);

int sccp_serializer_push_speeddial_stat_res(struct sccp_serializer *szer, uint32_t index, const char *extension, const char *label);

int sccp_serializer_push_time_date_res(struct sccp_serializer *szer);

int sccp_serializer_push_reset(struct sccp_serializer *szer, enum sccp_reset_type type);

/*!
 * \brief Push a message to the deserializer buffer.
 *
 * \note In fact, right now, the buffer is flushed on every call to push, i.e.
 *       a write system call is done on every call to push, so there's not
 *       really any before (and that's why there's no exposed flush function)
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_serializer_push(struct sccp_serializer *szer, struct sccp_msg *msg);

#endif /* SCCP_SERIALIZER_H_ */
