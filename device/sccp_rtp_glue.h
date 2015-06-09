#ifndef SCCP_RTP_GLUE_H_
#define SCCP_RTP_GLUE_H_

enum ast_rtp_glue_result;
struct ast_channel;
struct ast_format_cap;
struct ast_rtp_instance;

/*!
 * \brief Implementation of ast_rtp_glue::get_rtp_info.
 */
enum ast_rtp_glue_result sccp_rtp_glue_get_rtp_info(struct ast_channel *channel, struct ast_rtp_instance **instance);

/*!
 * \brief Implementation of ast_rtp_glue::update_peer.
 */
int sccp_rtp_glue_update_peer(struct ast_channel *channel, struct ast_rtp_instance *rtp, struct ast_rtp_instance *vrtp, struct ast_rtp_instance *trtp, const struct ast_format_cap *cap, int nat_active);

/*!
 * \brief Implementation of ast_rtp_glue::get_codec.
 */
void sccp_rtp_glue_get_codec(struct ast_channel *channel, struct ast_format_cap *result);

#endif /* SCCP_RTP_GLUE_H_ */
