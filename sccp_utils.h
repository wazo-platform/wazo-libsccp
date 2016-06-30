#ifndef SCCP_UTILS_H_
#define SCCP_UTILS_H_

#include <time.h>

struct sccp_cfg;

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define letohl(x) (x)
#define letohs(x) (x)
#define htolel(x) (x)
#define htoles(x) (x)
#else
#include <byteswap.h>
#define letohl(x) bswap_32(x)
#define letohs(x) bswap_16(x)
#define htolel(x) bswap_32(x)
#define htoles(x) bswap_16(x)
#endif

struct sccp_stat {
	int device_fault_count;
	time_t device_fault_last;
	int device_panic_count;
	time_t device_panic_last;
};

/*!
 * \brief Update the global device fault count and last device fault time.
 *
 * This function is thread safe.
 */
void sccp_stat_on_device_fault(void);

/*!
 * \brief Update the global device panic count and the last device panic time.
 *
 * This function is thread safe.
 */
void sccp_stat_on_device_panic(void);

/*!
 * \brief Take a snapshot of the global stat and copy it into dst.
 *
 */
void sccp_stat_take_snapshot(struct sccp_stat *dst);

/*!
 * \brief Set the TOS / DSCP value on the given socket from the config.
 *
 * \note Only set the option on sockfd if the tos value in new_cfg differs from the
 *       one in old_cfg, or if old_cfg is NULL.
 */
int sccp_socket_set_tos(int sockfd, struct sccp_cfg *new_cfg, struct sccp_cfg *old_cfg);

#endif /* SCCP_UTILS_H_ */
