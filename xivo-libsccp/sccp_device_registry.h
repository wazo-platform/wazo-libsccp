#ifndef SCCP_DEVICE_REGISTRY_H_
#define SCCP_DEVICE_REGISTRY_H_

struct sccp_device;
struct sccp_device_registry;
struct sccp_device_snapshot;

#define SCCP_DEVICE_REGISTRY_ALREADY 1

/*!
 * \brief Create a new device registry.
 *
 * The device registry is a thread safe container for devices.
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
 * \retval 0 on success
 * \retval SCCP_DEVICE_REGISTRY_ALREADY if a device with the same name is already in the container
 * \retval -1 on other failure
 */
int sccp_device_registry_add(struct sccp_device_registry *registry, struct sccp_device *device);

/*!
 * \brief Remove a device from the registry.
 */
void sccp_device_registry_remove(struct sccp_device_registry *registry, struct sccp_device *device);

/*!
 * \brief Find a device by name.
 *
 * \note The returned object has its reference count incremented by one.
 *
 * \retval the device with the given name, or NULL if no such device exist
 */
struct sccp_device *sccp_device_registry_find(struct sccp_device_registry *registry, const char *name);

/*!
 * \brief Find a line by name.
 *
 * \note The returned object has its reference count incremented by one.
 *
 * \retval the line with the given name, or NULL if no such line exist
 */
struct sccp_line *sccp_device_registry_find_line(struct sccp_device_registry *registry, const char *name);

/*!
 * \brief Completion function for CLI.
 */
char *sccp_device_registry_complete(struct sccp_device_registry *registry, const char *word, int state);

/*!
 * \brief Take a snapshot of all the devices in the registry.
 *
 * \param[out] snapshots address where to store the dynamically allocated snapshots array
 * \param[out] n length of the snapshots array
 *
 * On success, this function allocates memory to hold all the device snapshots and store this
 * address in *snapshots; it is the caller responsibility to eventually call ast_free on *snapshots, else
 * memory will be leaked.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_device_registry_take_snapshots(struct sccp_device_registry *registry, struct sccp_device_snapshot **snapshots, size_t *n);

#endif /* SCCP_DEVICE_REGISTRY_H_ */
