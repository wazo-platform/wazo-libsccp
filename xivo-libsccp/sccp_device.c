#include <asterisk.h>

#include "sccp_config.h"
#include "sccp_device.h"

void sccp_device_reload_config(struct sccp_device *device, struct sccp_cfg *cfg)
{
}

int sccp_device_want_disconnect(struct sccp_device *device)
{
	return 0;
}

int sccp_device_want_unlink(struct sccp_device *device)
{
	return 0;
}
