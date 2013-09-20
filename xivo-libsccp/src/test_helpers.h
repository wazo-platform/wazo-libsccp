#ifndef _SCCP_TEST_HELPERS_H_
#define _SCCP_TEST_HELPERS_H_

#define assert_null_handled(f) \
	do { \
		/* ast_test_status_update(test, "Running: %s", #f); */ \
		f; \
		/* ast_test_status_update(test, "... success\n"); */ \
	} while(0)

#define assert_equal(value, expected, message) \
	do { \
		if ((value) != (expected)) { \
			ast_test_status_update(test, "%s", message); \
			return AST_TEST_FAIL; \
		} \
	} while(0)

#define assert_not_equal(value, expected, message) \
	do { \
		if ((value) == (expected)) { \
			ast_test_status_update(test, "%s", message); \
			return AST_TEST_FAIL; \
		} \
	} while(0)

#define assert_string_equal(value, expected) \
	do { \
		if (strcmp(value, expected)) { \
			ast_test_status_update(test, "failed: %s != %s\n", expected, value); \
			return AST_TEST_FAIL; \
		} \
	} while(0)

#endif /* _SCCP_TEST_HELPERS_H_ */
