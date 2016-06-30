#include <asterisk.h>
#include <asterisk/lock.h>
#include <asterisk/logger.h>

#include "sccp_config.h"
#include "sccp_utils.h"

static struct sccp_stat stat;

void sccp_stat_on_device_fault(void)
{
	time_t now = time(NULL);

	stat.device_fault_last = now;
	ast_atomic_fetchadd_int(&stat.device_fault_count, 1);
}

void sccp_stat_on_device_panic(void)
{
	time_t now = time(NULL);

	stat.device_panic_last = now;
	ast_atomic_fetchadd_int(&stat.device_panic_count, 1);
}

void sccp_stat_take_snapshot(struct sccp_stat *dst)
{
	memcpy(dst, &stat, sizeof(*dst));
}

int sccp_socket_set_tos(int sockfd, struct sccp_cfg *new_cfg, struct sccp_cfg *old_cfg)
{
	unsigned int tos = new_cfg->general_cfg->tos;

	if (old_cfg && old_cfg->general_cfg->tos == tos) {
		return 0;
	}

	if (setsockopt(sockfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == -1) {
		ast_log(LOG_ERROR, "socket set tos error: setsockopt: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}
