#ifndef SCCP_DEVICE_H_
#define SCCP_DEVICE_H_

#include "sccp.h"
#include "sccp_msg.h"

struct sccp_device;
struct sccp_device_cfg;
struct sccp_msg;
struct sccp_session;

struct sccp_device_info {
	const char *name;
	enum sccp_device_type type;
	uint8_t proto_version;
};

struct sccp_device_snapshot {
	enum sccp_device_type type;
	uint8_t proto_version;
	char name[SCCP_DEVICE_NAME_MAX];
	char ipaddr[16];
	char capabilities[32];
};

/*!
 * \brief Create a new device (astobj2 object).
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_device *sccp_device_create(struct sccp_device_cfg *device_cfg, struct sccp_session *session, struct sccp_device_info *info);

/*!
 * \brief Destroy the device.
 *
 * \note This does not decrease the reference count of the object.
 * \note Must be called only from the session thread.
 */
void sccp_device_destroy(struct sccp_device *device);

/*
 * \note Must be called only from the session thread.
 * \note This function can modify the "stop" flag.
 */
int sccp_device_handle_msg(struct sccp_device *device, struct sccp_msg *msg);

/*!
 * \note Must be called only from the session thread.
 * \note This function can modify the "stop" flag.
 */
int sccp_device_reload_config(struct sccp_device *device, struct sccp_device_cfg *device_cfg);

/*!
 * \brief Reset the device.
 *
 * \note Thread safe.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_device_reset(struct sccp_device *device, enum sccp_reset_type type);

/*
 * Called when the session detected the remote peer has closed the connection.
 *
 * \note When the session detects a connection lost, it calls this function and then
 *       stop, so no need to ask the session to stop here.
 */
void sccp_device_on_connection_lost(struct sccp_device *device);

/*
 * Called each time data is read from the session socket.
 *
 * \note Must be called only from the session thread.
 */
void sccp_device_on_data_read(struct sccp_device *device);

/*
 * Called every time a progress is signaled to the session.
 *
 * XXX to use when stuff needs to be done in the session thread yet we are
 *     in another thread
 *
 * \note Must be called only from the session thread.
 * \note This function can modify the "stop" flag.
 *
 * \see sccp_session_progress
 */
void sccp_device_on_progress(struct sccp_device *device);

/*
 * Called exactly once after the device has been succesfully added to the registry
 * (which is the last step in the registration process)
 */
void sccp_device_on_registration_success(struct sccp_device *device);

/*!
 * \brief Return the name of the device.
 */
const char *sccp_device_name(const struct sccp_device *device);

/*!
 * \brief Take a snapshot of information from the device.
 *
 * \note Thread safe.
 *
 * \param snapshot memory where the snapshot will be saved
 */
void sccp_device_take_snapshot(struct sccp_device *device, struct sccp_device_snapshot *snapshot);

#endif /* SCCP_DEVICE_H_ */
