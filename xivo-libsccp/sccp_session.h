#ifndef SCCP_SESSION_H_
#define SCCP_SESSION_H_

struct sccp_session;

/*!
 * \brief Create a new session (astobj2 object).
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_session *sccp_session_create(int sockfd);

/*
 * This function exit only when the session stops, either naturally or by
 * calling sccp_session_stop from another thread.
 *
 * Note that this function is guaranteed to eventually return.
 */
void sccp_session_run(struct sccp_session *session);

int sccp_session_stop(struct sccp_session *session);

int sccp_session_reload_config(struct sccp_session *session);

#endif /* SCCP_SESSION_H_ */
