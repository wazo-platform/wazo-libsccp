#ifndef SCCP_DEVICE_H_
#define SCCP_DEVICE_H_

struct sccp_cfg;
struct sccp_device;

void sccp_device_reload_config(struct sccp_device *device, struct sccp_cfg *cfg);

int sccp_device_want_disconnect(struct sccp_device *device);

int sccp_device_want_unlink(struct sccp_device *device);

#endif /* SCCP_DEVICE_H_ */
