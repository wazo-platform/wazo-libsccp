#ifndef SCCP_SESSION_H_
#define SCCP_SESSION_H_

struct sccp_cfg;
struct sccp_device_registry;
struct sccp_msg;
struct sccp_session;

/*!
 * \brief Create a new session (astobj2 object).
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_session *sccp_session_create(struct sccp_cfg *cfg, struct sccp_device_registry *registry, int sockfd);

/*!
 * \brief Run the session.
 *
 * \note This function exit only when the session stops, either "naturally"
 *       or after a call to sccp_session_stop.
 */
void sccp_session_run(struct sccp_session *session);

/*!
 * \brief Stop the session.
 *
 * \retval 0 on sucess
 * \retval non-zero on failure
 */
int sccp_session_stop(struct sccp_session *session);

/*!
 * \brief Reload the session and the associated device.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_session_reload_config(struct sccp_session *session, struct sccp_cfg *cfg);

/*
 * XXX To be called only from session thread.
 */
int sccp_session_transmit_msg(struct sccp_session *session, struct sccp_msg *msg);

#endif /* SCCP_SESSION_H_ */
