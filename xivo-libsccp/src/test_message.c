#include <asterisk/test.h>

#define assert_null_handled(x, ret) \
	do { \
		ast_test_status_update(test, "Running: %s", #x); \
		if (x != ret) { \
			ast_test_status_update(test, "... failed\n"); \
			result = AST_TEST_FAIL; \
			goto cleanup; \
		} else { \
			ast_test_status_update(test, "... success\n"); \
		} \
	} while(0)

AST_TEST_DEFINE(sccp_test_arguments)
{
	enum ast_test_result_state result = AST_TEST_PASS;

	switch (cmd) {
	case TEST_INIT:
		info->name = "sccp_test_arguments";
		info->category = "/channel/sccp/";
		info->summary = "test arguments";
		info->description = "Test how functions behave when good arguments are given.";

		return AST_TEST_NOT_RUN;

	case TEST_EXECUTE:
		break;
	}

	ast_test_status_update(test, "Executing sccp test good arguments...\n");

	if (extstate_ast2sccp(AST_EXTENSION_DEACTIVATED) != SCCP_BLF_STATUS_UNKNOWN) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (extstate_ast2sccp(AST_EXTENSION_REMOVED) != SCCP_BLF_STATUS_UNKNOWN) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (extstate_ast2sccp(AST_EXTENSION_RINGING) != SCCP_BLF_STATUS_ALERTING) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (extstate_ast2sccp(AST_EXTENSION_UNAVAILABLE) != SCCP_BLF_STATUS_UNKNOWN) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (extstate_ast2sccp(AST_EXTENSION_BUSY) != SCCP_BLF_STATUS_INUSE) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (extstate_ast2sccp(AST_EXTENSION_INUSE) != SCCP_BLF_STATUS_INUSE) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (extstate_ast2sccp(AST_EXTENSION_ONHOLD) != SCCP_BLF_STATUS_INUSE) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (extstate_ast2sccp(AST_EXTENSION_NOT_INUSE) != SCCP_BLF_STATUS_IDLE) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (extstate_ast2sccp(-500) != SCCP_BLF_STATUS_UNKNOWN) {
		ast_test_status_update(test, "failed: converting extention state from asterisk to sccp\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

cleanup:
	return result;
}

AST_TEST_DEFINE(sccp_test_utf8_to_iso88591)
{
	enum ast_test_result_state result = AST_TEST_PASS;
	void *retptr = NULL;

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

	ast_test_status_update(test, "Executing sccp test utf8_to_iso99581...\n");

	retptr = (void*)0x1;
	retptr = (void*)utf8_to_iso88591(NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: utf8_to_iso88591(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = NULL;
	retptr = utf8_to_iso88591("0sïkö Düô");
	if (retptr == NULL) {
		ast_test_status_update(test, "failed: retptr == NULL\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = NULL;
	retptr = utf8_to_iso88591("Ã©");
	if (strcmp(retptr, "é")) {
		ast_test_status_update(test, "failed: 'é' != %s\n", (char *)retptr);
		result = AST_TEST_FAIL;
		goto cleanup;
	}

cleanup:
	return result;
}

AST_TEST_DEFINE(sccp_test_null_arguments)
{
	enum ast_test_result_state result = AST_TEST_PASS;

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

	ast_test_status_update(test, "Executing sccp test null arguments...\n");

	/* returns void will segfault if it fails */
	speeddial_hints_subscribe(NULL, NULL);

	assert_null_handled(device_get_speeddial(NULL, 0), NULL);
	assert_null_handled(device_get_speeddial_by_index(NULL, 0), NULL);
	assert_null_handled(transmit_feature_status(NULL, 0, 0, 0, ""), -1);
	assert_null_handled(transmit_feature_status((struct sccp_session *)0x1, 0, 0, 0, NULL), -1);
	assert_null_handled(speeddial_hints_cb("", "", 0, NULL), -1);
	assert_null_handled(handle_feature_status_req_message(NULL, (struct sccp_session *)0x1), -1);
	assert_null_handled(handle_feature_status_req_message((struct sccp_msg *)0x1, NULL), -1);
	assert_null_handled(handle_speeddial_message(NULL, (struct sccp_session *)0x1), -1);
	assert_null_handled(handle_speeddial_message((struct sccp_msg *)0x1, NULL), -1);
	assert_null_handled(handle_speeddial_status_req_message(NULL, (struct sccp_session *)0x1), -1);
	assert_null_handled(handle_speeddial_status_req_message((struct sccp_msg *)0x1, NULL), -1);
	assert_null_handled(handle_softkey_template_req_message(NULL), -1);
	assert_null_handled(handle_config_status_req_message(NULL), -1);
	assert_null_handled(handle_time_date_req_message(NULL), -1);
	assert_null_handled(handle_button_template_req_message(NULL), -1);
	assert_null_handled(handle_keep_alive_message(NULL), -1);
	assert_null_handled(register_device(NULL, (void*)0xFF), -1);
	assert_null_handled(register_device((void*)0xFF, NULL), -1);
	assert_null_handled(sccp_new_channel(NULL, (void*)0xFF), NULL);
	assert_null_handled(cb_ast_get_rtp_peer(NULL, (void*)0xFF), AST_RTP_GLUE_RESULT_FORBID);
	assert_null_handled(cb_ast_get_rtp_peer((void*)0xFF, NULL), AST_RTP_GLUE_RESULT_FORBID);
	assert_null_handled(start_rtp(NULL), -1);
	assert_null_handled(sccp_start_the_call(NULL), -1);
	assert_null_handled(sccp_lookup_exten(NULL), NULL);
	assert_null_handled(handle_offhook_message(NULL, NULL), -1);
	assert_null_handled(handle_onhook_message(NULL, NULL), -1);
	assert_null_handled(handle_softkey_hold(0, 0, NULL), -1);
	assert_null_handled(handle_softkey_resume(0, 0, NULL), -1);
	assert_null_handled(handle_softkey_transfer(0, NULL), -1);
	assert_null_handled(handle_softkey_event_message(NULL, (void*)0xFF), -1);
	assert_null_handled(handle_softkey_event_message((void*)0xFF, NULL), -1);
	assert_null_handled(handle_softkey_set_req_message(NULL), -1);
	assert_null_handled(codec_sccp2ast(0, NULL), NULL);
	assert_null_handled(handle_capabilities_res_message(NULL, (void*)0xFF), -1);
	assert_null_handled(handle_capabilities_res_message((void*)0xFF, NULL), -1);
	assert_null_handled(handle_open_receive_channel_ack_message(NULL, (void*)0xFF), -1);
	assert_null_handled(handle_open_receive_channel_ack_message((void*)0xFF, NULL), -1);
	assert_null_handled(handle_line_status_req_message(NULL, (void*)0xFF), -1);
	assert_null_handled(handle_line_status_req_message((void*)0xFF, NULL), -1);
	assert_null_handled(handle_register_message(NULL, (void*)0xFF), -1);
	assert_null_handled(handle_register_message((void*)0xFF, NULL), -1);
	assert_null_handled(handle_ipport_message(NULL, (void*)0xFF), -1);
	assert_null_handled(handle_ipport_message((void*)0xFF, NULL), -1);
	assert_null_handled(handle_keypad_button_message(NULL, (void*)0xFF), -1);
	assert_null_handled(handle_keypad_button_message((void*)0xFF, NULL), -1);
	assert_null_handled(handle_message(NULL, (void*)0xFF), -1);
	assert_null_handled(handle_message((void*)0xFF, NULL), -1);
	assert_null_handled(fetch_data(NULL), -1);
	assert_null_handled(cb_ast_request(NULL, (void*)0xFF, (void*)0xFF, (void*)0xFF, (void*)0xFF), NULL);
	assert_null_handled(cb_ast_request((void*)0xFF, (void*)0xFF, (void*)0xFF, NULL, (void*)0xFF), NULL);
	assert_null_handled(cb_ast_request((void*)0xFF, (void*)0xFF, (void*)0xFF, (void*)0xFF, NULL), NULL);
	assert_null_handled(cb_ast_call(NULL, (void*)0xFF, 0), -1);
	assert_null_handled(cb_ast_call((void*)0xFF, NULL, 0), -1);
	assert_null_handled(cb_ast_hangup(NULL), -1);
	assert_null_handled(cb_ast_answer(NULL), -1);
	assert_null_handled(cb_ast_read(NULL), NULL);
	assert_null_handled(cb_ast_write(NULL, (void*)0xFF), -1);
	assert_null_handled(cb_ast_write((void*)0xFF, NULL), -1);
	assert_null_handled(cb_ast_indicate(NULL, 0xFF, (void*)0xFF, 0xFF), -1);
	assert_null_handled(cb_ast_fixup(NULL, (void*)0xFF), -1);
	assert_null_handled(cb_ast_fixup((void*)0xFF, NULL), -1);

cleanup:
	return result;
}
