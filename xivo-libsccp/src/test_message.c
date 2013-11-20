#include <asterisk/test.h>

#include "test_helpers.h"

AST_TEST_DEFINE(sccp_test_extstate_ast2sccp)
{
	const char* fail_message = "failed: converting extention state from asterisk to sccp\n";

	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_extstate_ast2sccp";
		info->category = "/channel/sccp/";
		info->summary = "Tests the return value of extstate_ast2sccp";
		info->description = "Runs the extstate_ast2sccp function against multiple arguments and checks the result";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	assert_equal(extstate_ast2sccp(AST_EXTENSION_DEACTIVATED), SCCP_BLF_STATUS_UNKNOWN, fail_message);
	assert_equal(extstate_ast2sccp(AST_EXTENSION_REMOVED), SCCP_BLF_STATUS_UNKNOWN, fail_message);
	assert_equal(extstate_ast2sccp(AST_EXTENSION_RINGING), SCCP_BLF_STATUS_ALERTING, fail_message);
	assert_equal(extstate_ast2sccp(AST_EXTENSION_UNAVAILABLE), SCCP_BLF_STATUS_UNKNOWN, fail_message);
	assert_equal(extstate_ast2sccp(AST_EXTENSION_BUSY), SCCP_BLF_STATUS_INUSE, fail_message);
	assert_equal(extstate_ast2sccp(AST_EXTENSION_INUSE), SCCP_BLF_STATUS_INUSE, fail_message);
	assert_equal(extstate_ast2sccp(AST_EXTENSION_ONHOLD), SCCP_BLF_STATUS_INUSE, fail_message);
	assert_equal(extstate_ast2sccp(AST_EXTENSION_NOT_INUSE), SCCP_BLF_STATUS_IDLE, fail_message);
	assert_equal(extstate_ast2sccp(-500), SCCP_BLF_STATUS_UNKNOWN, fail_message);

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(sccp_test_utf8_to_iso88591)
{
	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_utf8_to_iso88591";
		info->category = "/channel/sccp/";
		info->summary = "Test for string conversions";
		info->description = "Test how utf8_to_iso88591 behaves with different arguments.";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	assert_equal(utf8_to_iso88591(NULL), NULL, "failed: utf8_to_iso88591(NULL)\n");
	assert_not_equal(utf8_to_iso88591("0sïkö Düô"), NULL, "failed: retptr == NULL\n");
	assert_string_equal(utf8_to_iso88591("Ã©"), "é");
	assert_string_equal(utf8_to_iso88591("a"), "a");

	return AST_TEST_PASS;
}

AST_TEST_DEFINE(sccp_test_null_arguments)
{
	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_null_arguments";
		info->category = "/channel/sccp/";
		info->summary = "test null arguments";
		info->description = "Test how functions behave when arguments passed are NULL.";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	assert_null_handled(speeddial_hints_subscribe(NULL, NULL));
	assert_null_handled(device_get_speeddial(NULL, 0));
	assert_null_handled(device_get_speeddial_by_index(NULL, 0));
	assert_null_handled(transmit_feature_status(NULL, 0, 0, 0, ""));
	assert_null_handled(transmit_feature_status((struct sccp_session *)0x1, 0, 0, 0, NULL));
	assert_null_handled(speeddial_hints_cb("", "", 0, NULL));
	assert_null_handled(register_device(NULL, (void*)0xFF));
	assert_null_handled(register_device((void*)0xFF, NULL));
	assert_null_handled(sccp_new_channel(NULL, (void*)0xFF, NULL));
	assert_null_handled(start_rtp(NULL));
	assert_null_handled(sccp_start_the_call(NULL));
	assert_null_handled(handle_softkey_hold(0, 0, NULL));
	assert_null_handled(handle_softkey_resume(0, 0, NULL));
	assert_null_handled(handle_softkey_transfer(0, NULL));
	assert_null_handled(handle_softkey_set_req_message(NULL));
	assert_null_handled(codec_sccp2ast(0, NULL));

	return AST_TEST_PASS;
}
