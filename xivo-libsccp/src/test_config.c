#include <asterisk/test.h>

#include "sccp_config.h"
#include "sccp_line.h"
#include "sccp_test_helpers.h"

static int config_from_string(struct sccp_configs *config, const char *content)
{
	const char *fname = "/tmp/sccp.conf";
	FILE *conf_file = fopen(fname, "w");

	if (conf_file == NULL) {
		return -1;
	}

	fwrite(content, 1, strlen(content), conf_file);
	fclose(conf_file);

	sccp_config_load(config, fname);

	remove(fname);

	return 0;
}

AST_TEST_DEFINE(sccp_test_config_set_field)
{
	RAII_VAR(struct sccp_configs *, sccp_cfg, sccp_new_config(), sccp_config_destroy);
	char *name;
	char *value;

	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_config_set_field";
		info->category = "/channel/sccp/";
		info->summary = "Tests the sccp_config_set_field function";
		info->description = "Tests wether a configuration option in a configuration file"
		     " has the right behavior on the sccp_configs structure";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	if (sccp_cfg == NULL) {
		return AST_TEST_FAIL;
	}

	{
		name = "bindaddr";
		value = "127.0.0.1";

		sccp_config_set_field(sccp_cfg, name, value);

		assert_string_equal(sccp_cfg->bindaddr, value);
	}

	{
		struct ast_format first;

		name = "allow";
		value = "ulaw";

		sccp_config_set_field(sccp_cfg, name, value);

		ast_codec_pref_index(&sccp_cfg->codec_pref, 0, &first);

		assert_equal(first.id, AST_FORMAT_ULAW, "Prefered codec did not match\n");
	}

	{
		struct ast_format first;

		name = "allow";
		value = "ulaw";

		sccp_config_set_field(sccp_cfg, name, value);
		sccp_config_set_field(sccp_cfg, name, "alaw");

		ast_codec_pref_index(&sccp_cfg->codec_pref, 0, &first);

		assert_equal(first.id, AST_FORMAT_ULAW, "Prefered codec did not match\n");
	}

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(sccp_test_resync)
{
	enum ast_test_result_state ret = AST_TEST_PASS;
	RAII_VAR(struct sccp_configs *, sccp_cfg, sccp_new_config(), sccp_config_destroy);
	const char *conf = NULL;
	struct sccp_line *line = NULL;
	struct sccp_speeddial *speeddial = NULL;
	struct sccp_device *device = NULL;

	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_resync";
		info->category = "/channel/sccp/";
		info->summary = "test sccp resync";
		info->description = "Test if a device is properly resynchronized.";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	ast_test_status_update(test, "Executing sccp resync device...\n");

	if (sccp_cfg == NULL) {
		return AST_TEST_FAIL;
	}

	conf =	"[general]\n"
		"bindaddr=0.0.0.0\n"
		"dateformat=D.M.Y\n"
		"keepalive=10\n"
		"authtimeout=10\n"
		"dialtimeout=3\n"
		"context=default\n"
		"language=en_US\n"
		"vmexten=*988\n"
		"\n"
		"[lines]\n"
		"[200]\n"
		"cid_num=200\n"
		"cid_name=Bob\n"
		"setvar=XIVO=10\n"
		"language=fr_FR\n"
		"context=a_context\n"
		"\n"
		"[speeddials]\n"
		"[sd1000]\n"
		"extension = 1000\n"
		"label = Call 1000\n"
		"blf = yes\n"
		"[sd1001]\n"
		"extension = 1001\n"
		"label = Call 1001\n"
		"blf = no\n"
		"[devices]\n"
		"[SEPACA016FDF235]\n"
		"device=SEPACA016FDF235\n"
		"speeddial=sd1000\n"
		"line=200\n"
		"speeddial=sd1001\n"
		"voicemail=555\n";
	if (config_from_string(sccp_cfg, conf) != 0) {
	     return AST_TEST_FAIL;
	}

	line = sccp_line_find_by_name("200", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s doesn't exist\n", "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(line->cid_name, "Bob")) {
		ast_test_status_update(test, "line->cid_name %s != %s\n", line->cid_name, "Bob");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	device = find_device_by_name("SEPACA016FDF235", &sccp_cfg->list_device);
	if (device == NULL) {
		ast_test_status_update(test, "device name %s doesn't exist\n", "SEPACA016FDF235");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(device->voicemail, "555")) {
		ast_test_status_update(test, "device->voicemail %s != %s\n", device->voicemail, "555");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	speeddial = device_get_speeddial(device, 1);
	if (speeddial == NULL) {
		ast_test_status_update(test, "speeddial instance 1 should exist...\n");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(speeddial->label, "Call 1000")) {
		ast_test_status_update(test, "speeddial->label %s != %s\n", speeddial->label, "Call 1000");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	/* Create a new configuration */
	conf =	"[general]\n"
		"bindaddr=0.0.0.0\n"
		"dateformat=D.M.Y\n"
		"keepalive=10\n"
		"authtimeout=10\n"
		"dialtimeout=3\n"
		"context=default\n"
		"language=en_US\n"
		"vmexten=*988\n"
		"\n"
		"[lines]\n"
		"[200]\n"
		"cid_num=200\n"
		"cid_name=Alice\n"
		"setvar=XIVO=10\n"
		"language=fr_FR\n"
		"context=a_context\n"
		"\n"
		"[speeddials]\n"
		"[sd1000]\n"
		"extension = 1000\n"
		"label = Call Home\n"
		"blf = yes\n"
		"[sd1001]\n"
		"extension = 1001\n"
		"label = Call 1001\n"
		"blf = no\n"
		"[devices]\n"
		"[SEPACA016FDF235]\n"
		"device=SEPACA016FDF235\n"
		"speeddial=sd1000\n"
		"line=200\n"
		"speeddial=sd1001\n"
		"voicemail=557\n";
	if (config_from_string(sccp_cfg, conf) != 0) {
	     return AST_TEST_FAIL;
	}

	(void)AST_LIST_REMOVE(&sccp_cfg->list_device, device, list);
	transmit_reset(device->session, 2);
	device_unregister(device);
	destroy_device_config(sccp_cfg, device);

	if (config_from_string(sccp_cfg, conf) != 0) {
	     return AST_TEST_FAIL;
	}

	/* Verify again */
	line = sccp_line_find_by_name("200", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s doesn't exist\n", "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(line->cid_name, "Alice")) {
		ast_test_status_update(test, "line->cid_name %s != %s\n", line->cid_name, "Alice");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	device = find_device_by_name("SEPACA016FDF235", &sccp_cfg->list_device);
	if (device == NULL) {
		ast_test_status_update(test, "device name %s doesn't exist\n", "SEPACA016FDF235");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(device->voicemail, "557")) {
		ast_test_status_update(test, "device->voicemail %s != %s\n", device->voicemail, "557");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	speeddial = device_get_speeddial(device, 1);
	if (speeddial == NULL) {
		ast_test_status_update(test, "speeddial instance 1 should exist...\n");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(speeddial->label, "Call Home")) {
		ast_test_status_update(test, "speeddial->label %s != %s\n", speeddial->label, "Call Home");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

cleanup:
	return ret;
}

AST_TEST_DEFINE(sccp_test_config)
{
	enum ast_test_result_state ret = AST_TEST_PASS;
	RAII_VAR(struct sccp_configs *, sccp_cfg, sccp_new_config(), sccp_config_destroy);
	char *conf = NULL;
	struct sccp_line *line = NULL;
	struct sccp_speeddial *speeddial = NULL;
	struct sccp_device *device = NULL;

	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_config";
		info->category = "/channel/sccp/";
		info->summary = "test sccp config";
		info->description = "Test wether the sccp configuration is parsed properly.";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	ast_test_status_update(test, "Executing sccp test config...\n");

	if (sccp_cfg == NULL) {
		return AST_TEST_FAIL;
	}

	conf =	"[general]\n"
		"bindaddr=0.0.0.0\n"
		"dateformat=D.M.Y\n"
		"keepalive=10\n"
		"authtimeout=10\n"
		"dialtimeout=3\n"
		"context=default\n"
		"language=en_US\n"
		"vmexten=*988\n"
		"\n"
		"[lines]\n"
		"[200]\n"
		"cid_num=200\n"
		"cid_name=Bob\n"
		"setvar=XIVO=10\n"
		"language=fr_FR\n"
		"context=a_context\n"
		"\n"
		"[speeddials]\n"
		"[sd1000]\n"
		"extension = 1000\n"
		"label = Call 1000\n"
		"blf = yes\n"
		"[sd1001]\n"
		"extension = 1001\n"
		"label = Call 1001\n"
		"blf = no\n"
		"[devices]\n"
		"[SEPACA016FDF235]\n"
		"device=SEPACA016FDF235\n"
		"speeddial=sd1000\n"
		"line=200\n"
		"speeddial=sd1001\n";
	if (config_from_string(sccp_cfg, conf) != 0) {
		return AST_TEST_FAIL;
	}

	if (strcmp(sccp_cfg->bindaddr, "0.0.0.0")) {
		ast_test_status_update(test, "bindaddr %s != %s\n", sccp_cfg->bindaddr, "0.0.0.0");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(sccp_cfg->dateformat, "D.M.Y")) {
		ast_test_status_update(test, "dateformat %s != %s\n", sccp_cfg->dateformat, "D.M.Y");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (sccp_cfg->keepalive != 10) {
		ast_test_status_update(test, "keepalive %i != %i\n", sccp_cfg->keepalive, 10);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (sccp_cfg->authtimeout != 10) {
		ast_test_status_update(test, "authtimeout %i != %i\n", sccp_cfg->authtimeout, 10);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (sccp_cfg->dialtimeout != 3) {
		ast_test_status_update(test, "dialtimeout %i != %i\n", sccp_cfg->dialtimeout, 3);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(sccp_cfg->context, "default")) {
		ast_test_status_update(test, "context %s != %s\n", sccp_cfg->context, "default");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(sccp_cfg->language, "en_US")) {
		ast_test_status_update(test, "language %s != %s\n", sccp_cfg->language, "en_US");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(sccp_cfg->vmexten, "*988")) {
		ast_test_status_update(test, "vmexten '%s' != '%s'\n", sccp_cfg->vmexten, "*988");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	line = sccp_line_find_by_name("200", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s doesn't exist\n", "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (line->device == NULL) {
		ast_test_status_update(test, "line %s has no device\n", line->name);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (line->chanvars == NULL) {
		ast_test_status_update(test, "line %s varchars is NULL\n", line->name);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(line->chanvars->name, "XIVO")) {
		ast_test_status_update(test, "line->chanvars->name %s != %s\n", line->chanvars->name, "XIVO");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(line->chanvars->value, "10")) {
		ast_test_status_update(test, "line->chanvars->value %s != %s\n", line->chanvars->value, "10");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(line->language, "fr_FR")) {
		ast_test_status_update(test, "language %s != %s\n", line->language, "fr_FR");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(line->context, "a_context")) {
		ast_test_status_update(test, "context %s != %s\n", line->context, "a_context");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	device = find_device_by_name("SEPACA016FDF235", &sccp_cfg->list_device);
	if (device == NULL) {
		ast_test_status_update(test, "device name %s doesn't exist\n", "SEPACA016FDF235");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	speeddial = device_get_speeddial(device, 0);
	if (speeddial != NULL) {
		ast_test_status_update(test, "speeddial instance 0 shouldn't exist...\n");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	speeddial = device_get_speeddial(device, 1);
	if (speeddial == NULL) {
		ast_test_status_update(test, "speeddial instance 1 should exist...\n");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(speeddial->name, "sd1000")) {
		ast_test_status_update(test, "%s != %s\n", speeddial->name, "sd1000");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(speeddial->label, "Call 1000")) {
		ast_test_status_update(test, "%s != %s\n", speeddial->label, "Call 1000");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (strcmp(speeddial->extension, "1000")) {
		ast_test_status_update(test, "%s != %s\n", speeddial->extension, "1000");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (speeddial->instance != 1) {
		ast_test_status_update(test, "%d != %d\n", speeddial->instance, 1);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (speeddial->device != device) {
		ast_test_status_update(test, "%p != %p\n", speeddial->device, device);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	speeddial = device_get_speeddial(device, 3);
	if (speeddial == NULL) {
		ast_test_status_update(test, "speeddial instance 3 should exist...\n");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	line = sccp_line_find_by_name("201", &sccp_cfg->list_line);
	if (line != NULL) {
		ast_test_status_update(test, "line 201 doesn't exist...\n");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	conf =	"[general]\n"
		"bindaddr=0.0.0.0\n"
		"dateformat=D.M.Y\n"
		"keepalive=10\n"
		"authtimeout=10\n"
		"dialtimeout=3\n"
		"context=default\n"
		"\n"
		"[lines]\n"
		"[201]\n"
		"cid_num=201\n"
		"cid_name=Alice\n"
		"\n"
		"[devices]\n"
		"[SEPACA016FDF236]\n"
		"device=SEPACA016FDF236\n"
		"line=201";
	if (config_from_string(sccp_cfg, conf) != 0) {
		return AST_TEST_FAIL;
	}

	/* We removed line 200 and its associated device.
	 * We add line 201 with a new device.
	 *
	 * Expectation:
	 * Line 201 must be added to the list.
	 * Line 200 must still be in the list.
	 */

	line = sccp_line_find_by_name("200", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s doesn't exist\n", "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (line->device == NULL) {
		ast_test_status_update(test, "line %s has no device\n", line->name);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	device = find_device_by_name("SEPACA016FDF235", &sccp_cfg->list_device);
	if (device == NULL) {
		ast_test_status_update(test, "device name %s doesn't exist\n", "SEPACA016FDF235");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	line = sccp_line_find_by_name("201", &sccp_cfg->list_line);
	if (line == NULL) {
		ast_test_status_update(test, "line name %s doesn't exist\n", "200");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	if (line->device == NULL) {
		ast_test_status_update(test, "line %s has no device\n", line->name);
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

	device = find_device_by_name("SEPACA016FDF236", &sccp_cfg->list_device);
	if (device == NULL) {
		ast_test_status_update(test, "device name %s doesn't exist\n", "SEPACA016FDF236");
		ret = AST_TEST_FAIL;
		goto cleanup;
	}

cleanup:
	return ret;
}
