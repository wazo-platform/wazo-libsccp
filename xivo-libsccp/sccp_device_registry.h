#ifndef SCCP_DEVICE_REGISTRY_H_
#define SCCP_DEVICE_REGISTRY_H_

struct sccp_device;
struct sccp_device_registry;

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

#endif /* SCCP_DEVICE_REGISTRY_H_ */
