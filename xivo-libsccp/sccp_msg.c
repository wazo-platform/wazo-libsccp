#include <string.h>

#include <asterisk.h>
#include <asterisk/logger.h>

#include "sccp_msg.h"
#include "sccp_utils.h"

#define MIN_TOTAL_LENGTH 12
#define MAX_TOTAL_LENGTH sizeof(struct sccp_msg)

void sccp_deserializer_init(struct sccp_deserializer *deserializer)
{
	deserializer->start = 0;
	deserializer->end = 0;
}

int sccp_deserializer_read(struct sccp_deserializer *deserializer, int fd)
{
	ssize_t n;
	size_t bytes_left;

	bytes_left = sizeof(deserializer->buf) - deserializer->end;
	if (!bytes_left) {
		ast_log(LOG_WARNING, "sccp deserializer read failed: buffer is full\n");
		return SCCP_DESERIALIZER_FULL;
	}

	n = read(fd, &deserializer->buf[deserializer->end], bytes_left);
	if (n == -1) {
		ast_log(LOG_ERROR, "sccp deserializer read failed: read: %s\n", strerror(errno));
		return SCCP_DESERIALIZER_ERROR;
	} else if (n == 0) {
		return SCCP_DESERIALIZER_EOF;
	}

	deserializer->end += (size_t) n;
	ast_log(LOG_DEBUG, "new end is %zd\n", deserializer->end);

	return 0;
}

int sccp_deserializer_get(struct sccp_deserializer *deserializer, struct sccp_msg **msg)
{
	size_t avail_bytes;
	size_t new_start;
	size_t total_length;
	uint32_t msg_length;

	avail_bytes = deserializer->end - deserializer->start;
	ast_log(LOG_DEBUG, "pop: %zd %zd (%zd)\n", deserializer->start, deserializer->end, avail_bytes);

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
