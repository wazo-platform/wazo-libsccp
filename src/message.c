
#include "asterisk.h"
#include "asterisk/utils.h"

#include "message.h"
#include "utils.h"

struct sccp_msg *msg_alloc(size_t data_length, int message_id)
{
	struct sccp_msg *msg;

	msg = ast_calloc(1, 12 + 4 + data_length);

	msg->length = htolel(4 + data_length);
	msg->id = message_id;

	return msg;
}


