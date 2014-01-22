#ifndef SCCP_DEVICE_H_
#define SCCP_DEVICE_H_

struct sccp_device;
struct sccp_device_cfg;
struct sccp_device_registry;
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

void sccp_device_handle_msg(struct sccp_device *device, struct sccp_msg *msg);

void sccp_device_reload_config(struct sccp_device *device, struct sccp_device_cfg *dev_cfg);

int sccp_device_want_disconnect(struct sccp_device *device);

int sccp_device_want_unlink(struct sccp_device *device);

/* XXX move the device registry stuff in its own .h file ? */

/*!
 * \brief Create a new device registry.
 *
 * \note The device registry is a thread safe container for devices.
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_device_registry *sccp_device_registry_create(void);

/*!
 * \brief Destroy the registry.
 */
void sccp_device_registry_destroy(struct sccp_device_registry *registry);

/*!
 * \brief Add a device to the registry.
 *
 * \note If a device with same name as already been added, this will fail.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_device_registry_add(struct sccp_device_registry *registry, struct sccp_device *device);

/*!
 * \brief Remove a device from the registry.
 */
void sccp_device_registry_remove(struct sccp_device_registry *registry, struct sccp_device *device);

#endif /* SCCP_DEVICE_H_ */
