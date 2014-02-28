#ifndef SCCP_SERIALIZER_H_
#define SCCP_SERIALIZER_H_

#include "sccp_msg.h"

/* XXX rename file ? move to sccp_msg ? */

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
 * \retval -1 on other failure
 */
int sccp_deserializer_read(struct sccp_deserializer *dzer);

/*!
 * \brief Get the next message from the deserializer.
 *
 * \param msg output parameter used to store the address of the parsed message
 *
 * \note The message stored in *msg is only valid between calls to this function.
 *
 * \retval 0 on success
 * \retval SCCP_DESERIALIZER_NOMSG if no message are available
 * \retval SCCP_DESERIALIZER_MALFORMED if next message is malformed
 */
int sccp_deserializer_pop(struct sccp_deserializer *dzer, struct sccp_msg **msg);

#endif /* SCCP_SERIALIZER_H_ */
