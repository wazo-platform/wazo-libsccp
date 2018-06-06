#ifndef SCCP_CHANNEL_TECH_H_
#define SCCP_CHANNEL_TECH_H_

#include <stddef.h>

struct ast_channel;
struct ast_format_cap;
struct ast_frame;
struct ast_assigned_ids;
struct sccp_line;

/*!
 * \brief Partial implementation of ast_channel_tech::requester.
 *
 * \note This function CAN'T be used directly in an ast_channel_tech. You must first obtain a sccp_line,
 *       and then call this function with it.
 */
struct ast_channel *sccp_channel_tech_requester(struct sccp_line *line, const char *options, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, int *cause);

/*!
 * \brief Partial implementation of ast_channel_tech::devicestate.
 *
 * \note This function CAN'T be used directly in an ast_channel_tech. You must first obtain a sccp_line,
 *       and then call this function with it.
 */
int sccp_channel_tech_devicestate(const struct sccp_line *line);

/*!
 * \brief Implementation of ast_channel_tech::call.
 */
int sccp_channel_tech_call(struct ast_channel *channel, const char *dest, int timeout);

/*!
 * \brief implementation of ast_channel_tech::hangup.
 */
int sccp_channel_tech_hangup(struct ast_channel *channel);

/*!
 * \brief Implementation of ast_channel_tech::answer.
 */
int sccp_channel_tech_answer(struct ast_channel *channel);

/*!
 * \brief Implementation of ast_channel_tech::read.
 */
struct ast_frame *sccp_channel_tech_read(struct ast_channel *channel);

/*!
 * \brief Implementation of ast_channel_tech::call.
 */
int sccp_channel_tech_write(struct ast_channel *channel, struct ast_frame *frame);

/*!
 * \brief Implementation of ast_channel_tech::indicate.
 */
int sccp_channel_tech_indicate(struct ast_channel *channel, int ind, const void *data, size_t datalen);

/*!
 * \brief Implementation of ast_channel_tech::fixup.
 */
int sccp_channel_tech_fixup(struct ast_channel *oldchannel, struct ast_channel *newchannel);

/*!
 * \brief Implementation of ast_channel_tech::send_digit_end.
 */
int sccp_channel_tech_send_digit_end(struct ast_channel *channel, char digit, unsigned int duration);

/*!
 * \brief Implementation of ast_channel_tech::func_channel_read.
 */
int sccp_channel_tech_acf_channel_read(struct ast_channel *channel, const char *cmd, char *data, char *buf, size_t len);

#endif /* SCCP_CHANNEL_TECH_H_ */
