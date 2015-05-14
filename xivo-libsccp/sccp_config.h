#ifndef SCCP_CONFIG_H_
#define SCCP_CONFIG_H_

#include <asterisk/channel.h>
#include <stddef.h>

#include "sccp.h"

struct ao2_container;

struct sccp_cfg {
	struct sccp_general_cfg *general_cfg;
	struct ao2_container *devices_cfg;
	struct ao2_container *lines_cfg;
	struct ao2_container *speeddials_cfg;
};

struct sccp_general_cfg {
	int authtimeout;

	struct sccp_device_cfg *guest_device_cfg;

	struct sccp_general_cfg_internal *internal;
};

struct sccp_device_cfg {
	char name[SCCP_DEVICE_NAME_MAX];
	char dateformat[6];
	char voicemail[AST_MAX_EXTENSION];
	char vmexten[AST_MAX_EXTENSION];
	char timezone[40];
	int keepalive;
	int dialtimeout;

	size_t speeddial_count;
	struct sccp_line_cfg *line_cfg;
	struct sccp_speeddial_cfg **speeddials_cfg;

	struct sccp_device_cfg_internal *internal;
};

struct sccp_line_cfg {
	char name[SCCP_LINE_NAME_MAX];
	char cid_num[40];
	char cid_name[40];
	char language[MAX_LANGUAGE];
	char context[AST_MAX_CONTEXT];
	int directmedia;
	unsigned int tos_audio;

	ast_group_t callgroups;
	ast_group_t pickupgroups;

	struct ast_namedgroups *named_callgroups;
	struct ast_namedgroups *named_pickupgroups;

	struct ast_format_cap *caps;

	struct ast_variable *chanvars;

	struct sccp_line_cfg_internal *internal;
};

struct sccp_speeddial_cfg {
	char name[SCCP_SPEEDDIAL_NAME_MAX];
	char label[40];
	char extension[AST_MAX_EXTENSION];
	int blf;
};

/*!
 * \brief Initialize the config submodule.
 *
 * \note Must be called once before using anything else in the submodule.
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int sccp_config_init(void);

/*!
 * \brief Free the resources associated to the config submodule.
 */
void sccp_config_destroy(void);

/*!
 * \brief Load the config from the configuration file.
 *
 * \note Should be called only once, after sccp_config_init. If you want to reload the
 *       config, call sccp_config_reload instead.
 *
 * \retval 0 on success
 * \retval non-zero on faiure
 */
int sccp_config_load(void);

/*!
 * \brief Reload the config from the configuration file.
 *
 * \retval 0 on success
 * \retval non-zero on faiure
 */
int sccp_config_reload(void);

/*!
 * \brief Get the current config.
 *
 * \note The returned object has its reference count incremented by one.
 */
struct sccp_cfg *sccp_config_get(void);

/*!
 * \brief Find the device config with the given name.
 *
 * \note The returned object has its reference count incremented by one.
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_device_cfg *sccp_cfg_find_device(struct sccp_cfg *cfg, const char *name);

/*!
 * \brief Find the device config with the given name, or the guest device config.
 *
 * \note The returned object has its reference count incremented by one
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_device_cfg *sccp_cfg_find_device_or_guest(struct sccp_cfg *cfg, const char *name);

/*!
 * \brief Find the line config with the given name.
 *
 * \note The returned object has its reference count incremented by one.
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_line_cfg *sccp_cfg_find_line(struct sccp_cfg *cfg, const char *name);

#endif /* SCCP_CONFIG_H_ */
