#include <asterisk.h>
#include <asterisk/lock.h>

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
