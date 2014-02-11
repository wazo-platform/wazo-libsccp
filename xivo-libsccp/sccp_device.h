#ifndef SCCP_DEVICE_H_
#define SCCP_DEVICE_H_

#include "sccp.h"
#include "sccp_msg.h"

struct ast_channel;
struct ast_format_cap;
struct sccp_device;
struct sccp_device_cfg;
struct sccp_line;
struct sccp_msg;
struct sccp_session;
struct sccp_subchannel;

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
 * \brief Take a snapshot of information from the device.
 *
 * \note Thread safe.
 *
 * \param snapshot memory where the snapshot will be saved
 */
void sccp_device_take_snapshot(struct sccp_device *device, struct sccp_device_snapshot *snapshot);

/*!
 * \brief Return the number of lines of the device.
 *
 * \note The number of line of a device is a constant attribute.
 */
unsigned int sccp_device_line_count(const struct sccp_device *device);

/*!
 * \brief Get the i'th line of the device (starting from zero).
 *
 * \note The reference count is NOT incremented
 */
struct sccp_line* sccp_device_line(struct sccp_device *device, unsigned int i);

/*!
 * \brief Return the name of the device.
 *
 * \note The name of a device is a constant attribute.
 */
const char *sccp_device_name(const struct sccp_device *device);

/*!
 * \brief Return the name of the line.
 *
 * \note The name of a line is a constant attribute.
 */
const char *sccp_line_name(const struct sccp_line *line);

/*
 * XXX request a new subchannel + channel
 *
 * \note The tech_pvt of the returned channel is a sccp_subchannel pointer.
 */
struct ast_channel *sccp_line_request(struct sccp_line *line, struct ast_format_cap *cap, const char *linkedid, int *cause);

int sccp_subchannel_call(struct sccp_subchannel *subchan);

int sccp_subchannel_hangup(struct sccp_subchannel *subchan);

int sccp_subchannel_answer(struct sccp_subchannel *subchan);

struct ast_frame *sccp_subchannel_read(struct sccp_subchannel *subchan);

int sccp_subchannel_write(struct sccp_subchannel *subchan, struct ast_frame *frame);

int sccp_subchannel_indicate(struct sccp_subchannel *subchan, int ind, const void *data, size_t datalen);

int sccp_subchannel_fixup(struct sccp_subchannel *subchan, struct ast_channel *newchannel);

#endif /* SCCP_DEVICE_H_ */
