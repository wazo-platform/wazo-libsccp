#ifndef SCCP_MSG_H_
#define SCCP_MSG_H_

#include <stdint.h>

#define SCCP_DESERIALIZER_NOMSG 1
#define SCCP_DESERIALIZER_FULL 2
#define SCCP_DESERIALIZER_EOF 3
#define SCCP_DESERIALIZER_MALFORMED 4
#define SCCP_DESERIALIZER_ERROR 5

struct sccp_msg {
	uint32_t length;
	uint32_t reserved;
	uint32_t id;
};

struct sccp_deserializer {
	struct sccp_msg msg;
	size_t start;
	size_t end;
	char buf[2048];
};

/*!
 * \brief Initialize the deserializer.
 */
void sccp_deserializer_init(struct sccp_deserializer *deserializer);

/*!
 * \brief Read data into the deserializer buffer.
 *
 * \param fd the file descriptor to read data from
 *
 * \retval 0 on success
 * \retval SCCP_DESERIALIZER_FULL if the buffer is full
 * \retval SCCP_DESERIALIZER_EOF if the end of file is reached
 * \retval SCCP_DESERIALIZER_ERROR on other failure
 */
int sccp_deserializer_read(struct sccp_deserializer *deserializer, int fd);

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
int sccp_deserializer_get(struct sccp_deserializer *deserializer, struct sccp_msg **msg);

/* TODO serializer... */

struct sccp_serializer {

};

void sccp_serializer_init(struct sccp_serializer *serializer);

/*!
 * \brief Serialize the message
 */
int sccp_serializer_put(struct sccp_serializer *serializer, struct sccp_msg *msg);

/*!
 * \brief Write the outstanding serialize buffer to the given file descriptor.
 */
int sccp_serializer_write(struct sccp_serializer *serializer, int fd);

#endif /* SCCP_MSG_H_ */
