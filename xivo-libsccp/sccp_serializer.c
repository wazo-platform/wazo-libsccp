#include <string.h>

#include <asterisk.h>
#include <asterisk/logger.h>

#include "sccp_serializer.h"
#include "sccp_utils.h"

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
	if (avail_bytes < SCCP_MSG_MIN_TOTAL_LEN) {
		return SCCP_DESERIALIZER_NOMSG;
	}

	memcpy(&msg_length, &deserializer->buf[deserializer->start], sizeof(msg_length));
	total_length = letohl(msg_length) + SCCP_MSG_LEN_OFFSET;
	if (total_length < SCCP_MSG_MIN_TOTAL_LEN || total_length > SCCP_MSG_MAX_TOTAL_LEN) {
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
