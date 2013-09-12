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
	int ret = 0;
	enum ast_test_result_state result = AST_TEST_PASS;
	void *retptr = NULL;
	struct sccp_speeddial *speeddial;

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

	speeddial = device_get_speeddial(NULL, 0);
	if (speeddial != NULL) {
		ast_test_status_update(test, "failed: device_get_speeddial(NULL, 0)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	speeddial = device_get_speeddial_by_index(NULL, 0);
	if (speeddial != NULL) {
		ast_test_status_update(test, "failed: device_get_speeddial_by_index(NULL, 0)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = transmit_feature_status(NULL, 0, 0, 0, "");
	if (ret != -1) {
		ast_test_status_update(test, "failed: transmit_feature_status(NULL, 0, 0, 0, "")");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = transmit_feature_status((struct sccp_session *)0x1, 0, 0, 0, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: transmit_feature_status((struct sccp_session *)0x1, 0, 0, 0, NULL");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = speeddial_hints_cb("", "", 0, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: speeddial_hints_cb("", "", 0, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	speeddial_hints_subscribe(NULL, NULL);

	ret = handle_feature_status_req_message(NULL, (struct sccp_session *)0x1);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_feature_status_req_message(NULL, (struct sccp_session *)0x1)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_feature_status_req_message((struct sccp_msg *)0x1, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_feature_status_req_message((struct sccp_msg *)0x1, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_speeddial_message(NULL, (struct sccp_session *)0x1);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_speeddial_message(NULL, "")\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_speeddial_message((struct sccp_msg *)0x1, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_speeddial_message("", NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_speeddial_status_req_message(NULL, (struct sccp_session *)0x1);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_speeddial_status_req_message\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_speeddial_status_req_message((struct sccp_msg *)0x1, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_speeddial_status_req_message\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_template_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_template_req_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_config_status_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_config_status_req_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_time_date_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_time_date_req_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_button_template_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_button_template_req_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_keep_alive_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_keep_alive_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = register_device(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: register_device(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = register_device((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: register_device(0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = sccp_new_channel(NULL, (void*)0xFF);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: sccp_new_channel(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (cb_ast_get_rtp_peer(NULL, (void*)0xFF) != AST_RTP_GLUE_RESULT_FORBID) {
		ast_test_status_update(test, "failed: cb_ast_get_rtp_peer(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	if (cb_ast_get_rtp_peer((void*)0xFF, NULL) != AST_RTP_GLUE_RESULT_FORBID) {
		ast_test_status_update(test, "failed: cb_ast_get_rtp_peer(0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = start_rtp(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: start_rtp(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = sccp_start_the_call(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: sccp_start_the_call(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = sccp_lookup_exten(NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: sccp_lookup_exten(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_offhook_message(NULL, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_offhook_message(NULL, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_onhook_message(NULL, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_onhook_message(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_hold(0, 0, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_hold(0, 0, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_resume(0, 0, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_resume(0, 0, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_transfer(0, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_transfer(0, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_event_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_event_message(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_event_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_event_message(0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_softkey_set_req_message(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_softkey_set_req_message(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = codec_sccp2ast(0, NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: codec_sccp2ast(0, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_capabilities_res_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_capabilities_res_message(NULL, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_capabilities_res_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_capabilities_res_message(0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_open_receive_channel_ack_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_open_receive_channel_ack_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_open_receive_channel_ack_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_open_receive_channel_ack_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_line_status_req_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_line_status_req_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_line_status_req_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_line_status_req_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_register_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_register_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_register_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_register_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_ipport_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_ipport_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_ipport_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_ipport_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_keypad_button_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_ipport_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_keypad_button_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_keypad_button_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_message(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_message(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = handle_message((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: handle_message((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = fetch_data(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: fetch_data(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = cb_ast_request(NULL, (void*)0xFF, (void*)0xFF, (void*)0xFF, (void*)0xFF);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: cb_ast_request(NULL, 0xFF, (void*)0xFF, (void*)0xFF, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = cb_ast_request((void*)0xFF, (void*)0xFF, (void*)0xFF, NULL, (void*)0xFF);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: cb_ast_request((void*)0xFF, 0xFF, NULL, (void*)0xFF, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = cb_ast_request((void*)0xFF, (void*)0xFF, (void*)0xFF, (void*)0xFF, NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: cb_ast_request((void*)0xFF, 0xFF, (void*)0xFF, (void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_call(NULL, (void*)0xFF, 0);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_call(NULL, (void*)0xFF, 0)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_call((void*)0xFF, NULL, 0);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_call((void*)0xFF, NULL, 0)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_hangup(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_hangup(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_answer(NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_answer(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	retptr = cb_ast_read(NULL);
	if (retptr != NULL) {
		ast_test_status_update(test, "failed: cb_ast_read(NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_write(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_write(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_write((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_write((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_indicate(NULL, 0xFF, (void*)0xFF, 0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_indicate(NULL, 0xFF, (void*)0xFF, 0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_fixup(NULL, (void*)0xFF);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_fixup(NULL, (void*)0xFF)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

	ret = cb_ast_fixup((void*)0xFF, NULL);
	if (ret != -1) {
		ast_test_status_update(test, "failed: cb_ast_fixup((void*)0xFF, NULL)\n");
		result = AST_TEST_FAIL;
		goto cleanup;
	}

cleanup:
	return result;
}
