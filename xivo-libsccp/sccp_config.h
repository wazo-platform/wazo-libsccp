#ifndef SCCP_CONFIG_H_
#define SCCP_CONFIG_H_

#include <stddef.h>

#include <asterisk/astobj2.h>
#include <asterisk/channel.h>

#define SCCP_DEVICE_NAME_MAX 40
#define SCCP_LINE_NAME_MAX 40
#define SCCP_SPEEDDIAL_NAME_MAX 40

struct sccp_cfg {
	struct sccp_general_cfg *general_cfg;
	struct ao2_container *devices_cfg;
	struct ao2_container *lines_cfg;
	struct ao2_container *speeddials_cfg;
};

struct sccp_general_cfg {
	char bindaddr[16];
	int authtimeout;

	struct sccp_device_cfg *guest_device_cfg;

	struct sccp_general_cfg_internal *internal;
};

struct sccp_device_cfg {
	char name[SCCP_DEVICE_NAME_MAX];
	char dateformat[6];
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
	char voicemail[AST_MAX_EXTENSION];
	char vmexten[AST_MAX_EXTENSION];
	int directmedia;
	unsigned int tos_audio;

	struct ast_codec_pref codec_pref;
	struct ast_format_cap *caps;	/* Allowed capabilities */

	struct ast_variable *chanvars;

	struct sccp_line_cfg_internal *internal;
};

struct sccp_speeddial_cfg {
	char name[SCCP_SPEEDDIAL_NAME_MAX];
	char label[40];
	char extension[AST_MAX_EXTENSION];
	int blf;
};

/*
 * Init the config submodule.
 *
 * Must be called once before using anything else in the submodule.
 *
 * Should be called from the module loading function.
 */
int sccp_config_init(void);

/*
 * Destroy/uninitialize the config module.
 *
 * Must be called to free the stuff.
 *
 * Should be called from the module unloading function.
 *
 * References held to the various config objects are still valid,
 * but calling any function except sccp_config_init is undefined.
 */
void sccp_config_destroy(void);

/*
 * Load the config from the configuration file.
 *
 * Should be called only once, after sccp_config_init. If you want to reload the
 * config, call sccp_config_reload.
 *
 * \retval 0 on success
 * \retval non-zero on faiure
 */
int sccp_config_load(void);

/*
 * Reload the config from the configuration file.
 *
 * \retval 0 on success
 * \retval non-zero on faiure
 */
int sccp_config_reload(void);

#endif /* SCCP_CONFIG_H_ */
