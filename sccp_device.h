#ifndef SCCP_DEVICE_H_
#define SCCP_DEVICE_H_

#include "sccp.h"
#include "sccp_msg.h"

struct ast_channel;
struct ast_format_cap;
struct ast_rtp_instance;
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
	int guest;
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
 * \note Quite a few operations have undefined behavior once the device is destroyed, including this one.
 */
void sccp_device_destroy(struct sccp_device *device);

/*!
 * \note Must be called only from the session thread.
 * \note It is an undefined behaviour to call this function on a destroyed device.
 */
int sccp_device_handle_msg(struct sccp_device *device, struct sccp_msg *msg);

/*!
 * \note Must be called only from the session thread.
 * \note It is an undefined behaviour to call this function on a destroyed device.
 */
int sccp_device_reload_config(struct sccp_device *device, struct sccp_device_cfg *device_cfg);

/*!
 * \brief Signal that the remote peer has closed the connection.
 *
 * \note Must be called only from the session thread.
 * \note It is an undefined behaviour to call this function on a destroyed device.
 */
void sccp_device_on_connection_lost(struct sccp_device *device);

/*!
 * \brief Signal that some data has been read from the session socket.
 *
 * \note Must be called only from the session thread.
 * \note It is an undefined behaviour to call this function on a destroyed device.
 */
void sccp_device_on_data_read(struct sccp_device *device);

/*!
 * \brief Signal that the registration was successful.
 *
 * \note Must be called only from the session thread.
 * \note It is an undefined behaviour to call this function on a destroyed device.
 */
void sccp_device_on_registration_success(struct sccp_device *device);

/*!
 * \brief Reset the device.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_device_reset(struct sccp_device *device, enum sccp_reset_type type);

/*!
 * \brief Take a snapshot of information from the device.
 *
 * \param snapshot memory where the snapshot will be saved
 */
void sccp_device_take_snapshot(struct sccp_device *device, struct sccp_device_snapshot *snapshot);

/*!
 * \brief Return the number of lines of the device.
 *
 * \note The number of line of a device is a constant attribute.
 * \note It is an undefined behaviour to call this function on a destroyed device.
 */
unsigned int sccp_device_line_count(const struct sccp_device *device);

/*!
 * \brief Get the i'th line of the device (starting from zero).
 *
 * \note The reference count is NOT incremented
 * \note It is an undefined behaviour to call this function on a destroyed device.
 */
struct sccp_line* sccp_device_line(struct sccp_device *device, unsigned int i);

/*!
 * \brief Return the name of the device.
 *
 * \note The name of a device is a constant attribute.
 */
const char *sccp_device_name(const struct sccp_device *device);

/*!
 * \brief Check if the device has an incoming active subchannel
 */
int sccp_device_has_active_incoming_subchan(const struct sccp_device *device);

/*!
 * \brief Transmit a call state to the given device
 */
void sccp_device_transmit_callstate(struct sccp_device *device, enum sccp_state state);

/*!
 * \brief Check if the device has an active subchannel
 */
int sccp_device_has_active_subchan(const struct sccp_device *device);

/*!
 * \brief Return non-zero if the device is a guest device.
 *
 * \note The "guest" state of a device is a constant attribute.
 */
int sccp_device_is_guest(struct sccp_device *device);

/*!
 * \brief Return the name of the line.
 *
 * \note The name of a line is a constant attribute.
 */
const char *sccp_line_name(const struct sccp_line *line);

/*!
 * \brief Return the device of the line.
 *
 */
struct sccp_device *sccp_line_device(const struct sccp_line *line);

/*!
 * \brief Transmit a tone to the device.
 *
 */
void sccp_device_transmit_tone(struct sccp_device *device, enum sccp_tone tone);

#endif /* SCCP_DEVICE_H_ */
