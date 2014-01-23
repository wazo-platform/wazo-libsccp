#ifndef SCCP_SESSION_H_
#define SCCP_SESSION_H_

struct sccp_cfg;
struct sccp_device;
struct sccp_device_registry;
struct sccp_msg;
struct sccp_session;

/*!
 * \brief Function type for device task callback
 *
 * \note Part of the device API.
 */
typedef void (*sccp_device_task_cb)(struct sccp_device *device, void *data);

struct sccp_device_task {
	sccp_device_task_cb callback;
	void *data;
};

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

/*!
 * \brief Add a device task.
 *
 * \note Must be called only from the session thread.
 * \note Part of the device API.
 */
int sccp_session_add_device_task(struct sccp_session *session, struct sccp_device_task task, int sec);

/*!
 * \brief Remove a device task.
 *
 * \note Must be called only from the session thread.
 * \note Part of the device API.
 */
void sccp_session_remove_device_task(struct sccp_session *session, struct sccp_device_task task);

/*
 * XXX called to force the session thread to call sccp_device_progress(session->device)
 * XXX name is bad
 *
 * \note Part of the device API.
 */
void sccp_session_progress(struct sccp_session *session);

/*!
 * \brief Return the serializer of the session.
 *
 * \note Part of the device API.
 */
struct sccp_serializer *sccp_session_serializer(struct sccp_session *session);

#endif /* SCCP_SESSION_H_ */
