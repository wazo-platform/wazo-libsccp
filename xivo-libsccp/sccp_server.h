#ifndef SCCP_SERVER_H_
#define SCCP_SERVER_H_

struct sccp_cfg;
struct sccp_server;

/*!
 * \brief Create a new server.
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_server *sccp_server_create(struct sccp_cfg *cfg);

/*!
 * \brief Destroy the server..
 *
 * \note If the server is running, it will be stopped.
 */
void sccp_server_destroy(struct sccp_server *server);

/*!
 * \brief Start the server.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_server_start(struct sccp_server *server);

/*!
 * \note This both reload the server configuration and the sessions configuration.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_server_reload_config(struct sccp_server *server, struct sccp_cfg *cfg);

/*!
 * \brief Return the number of active sessions.
 */
int sccp_server_session_count(struct sccp_server *server);

#endif /* SCCP_SERVER_H_ */
