#ifndef SCCP_DEVICE_H_
#define SCCP_DEVICE_H_

struct sccp_device;
struct sccp_device_cfg;
struct sccp_msg;
struct sccp_session;

/*!
 * \brief Create a new device (astobj2 object).
 *
 * \note Does not actually care if another device with the same name has been registered.
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_device *sccp_device_create(struct sccp_device_cfg *device_cfg, const char *name, struct sccp_session *session);

/*!
 * \brief Destroy the device.
 *
 * \note This does not decrease the reference count of the object.
 */
void sccp_device_destroy(struct sccp_device *device);

/*
 * XXX should we use accessor or should we make available the struct sccp_device structure ?
 */
const char *sccp_device_name(const struct sccp_device *device);

void sccp_device_handle_msg(struct sccp_device *device, struct sccp_msg *msg);

void sccp_device_reload_config(struct sccp_device *device, struct sccp_device_cfg *dev_cfg);

int sccp_device_want_disconnect(struct sccp_device *device);

int sccp_device_want_unlink(struct sccp_device *device);

#endif /* SCCP_DEVICE_H_ */
