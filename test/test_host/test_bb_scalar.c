#include "unity.h"
#include "bb_scalar.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

// bb_scalar_parse_bool — table-driven cases covering every accepted truthy/
// falsy token (plus case variants) and the reject path.
typedef struct {
    const char *name;
    const char *val;
    bool expected_ret;
    bool expected_out;
} bb_scalar_bool_case_t;

static const bb_scalar_bool_case_t s_bool_cases[] = {
    { "truthy_1",          "1",     true,  true  },
    { "truthy_true_lower", "true",  true,  true  },
    { "truthy_true_upper", "TRUE",  true,  true  },
    { "truthy_true_mixed", "True",  true,  true  },
    { "truthy_on",         "on",    true,  true  },
    { "truthy_yes_lower",  "yes",   true,  true  },
    { "truthy_yes_upper",  "YES",   true,  true  },
    { "truthy_yes_mixed",  "Yes",   true,  true  },
    { "truthy_t",          "t",     true,  true  },
    { "truthy_y",          "y",     true,  true  },
    { "falsy_0",           "0",     true,  false },
    { "falsy_false_lower", "false", true,  false },
    { "falsy_false_upper", "FALSE", true,  false },
    { "falsy_false_mixed", "False", true,  false },
    { "falsy_off",         "off",   true,  false },
    { "falsy_no_lower",    "no",    true,  false },
    { "falsy_no_upper",    "NO",    true,  false },
    { "falsy_no_mixed",    "No",    true,  false },
    { "falsy_f",           "f",     true,  false },
    { "falsy_n",           "n",     true,  false },
};

void test_bb_scalar_parse_bool_table_driven(void)
{
    for (size_t i = 0; i < sizeof(s_bool_cases) / sizeof(s_bool_cases[0]); i++) {
        const bb_scalar_bool_case_t *tc = &s_bool_cases[i];

        bool out = false;
        bool ret = bb_scalar_parse_bool(tc->val, &out);

        TEST_ASSERT_EQUAL_MESSAGE(tc->expected_ret, ret, tc->name);
        TEST_ASSERT_EQUAL_MESSAGE(tc->expected_out, out, tc->name);
    }
}

void test_bb_scalar_parse_bool_null_val_returns_false(void)
{
    bool out = true;
    bool ret = bb_scalar_parse_bool(NULL, &out);

    TEST_ASSERT_FALSE(ret);
    // *out must be left unwritten on failure.
    TEST_ASSERT_TRUE(out);
}

void test_bb_scalar_parse_bool_empty_string_returns_false(void)
{
    bool out = true;
    bool ret = bb_scalar_parse_bool("", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_TRUE(out);
}

void test_bb_scalar_parse_bool_garbage_returns_false(void)
{
    bool out = true;
    bool ret = bb_scalar_parse_bool("maybe", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_TRUE(out);
}

void test_bb_scalar_parse_bool_numeric_garbage_returns_false(void)
{
    bool out = false;
    bool ret = bb_scalar_parse_bool("2", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_FALSE(out);
}

// A value that shares a prefix with an accepted token but is longer
// exercises istrcmp's other loop-exit path: the shorter candidate string
// (*b) runs out while *a is still non-NUL, rather than both strings
// terminating together (exact match) or an early char mismatch.
void test_bb_scalar_parse_bool_prefix_of_accepted_token_returns_false(void)
{
    bool out = true;
    bool ret = bb_scalar_parse_bool("truex", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_TRUE(out);
}

// bb_scalar_parse_uint

void test_bb_scalar_parse_uint_valid_decimal(void)
{
    unsigned long out = 0;
    bool ret = bb_scalar_parse_uint("12345", &out);

    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT_EQUAL_UINT(12345UL, out);
}

void test_bb_scalar_parse_uint_leading_zeros_accepted(void)
{
    unsigned long out = 0;
    bool ret = bb_scalar_parse_uint("007", &out);

    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT_EQUAL_UINT(7UL, out);
}

void test_bb_scalar_parse_uint_zero(void)
{
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint("0", &out);

    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT_EQUAL_UINT(0UL, out);
}

void test_bb_scalar_parse_uint_null_val_returns_false(void)
{
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint(NULL, &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL_UINT(123UL, out);
}

void test_bb_scalar_parse_uint_empty_string_returns_false(void)
{
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint("", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL_UINT(123UL, out);
}

void test_bb_scalar_parse_uint_non_numeric_returns_false(void)
{
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint("abc", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL_UINT(123UL, out);
}

void test_bb_scalar_parse_uint_trailing_garbage_returns_false(void)
{
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint("12abc", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL_UINT(123UL, out);
}

void test_bb_scalar_parse_uint_leading_whitespace_returns_false(void)
{
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint(" 12", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL_UINT(123UL, out);
}

void test_bb_scalar_parse_uint_leading_plus_returns_false(void)
{
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint("+12", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL_UINT(123UL, out);
}

void test_bb_scalar_parse_uint_leading_minus_returns_false(void)
{
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint("-1", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL_UINT(123UL, out);
}

void test_bb_scalar_parse_uint_boundary_ulong_max(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", ULONG_MAX);

    unsigned long out = 0;
    bool ret = bb_scalar_parse_uint(buf, &out);

    TEST_ASSERT_TRUE(ret);
    TEST_ASSERT_EQUAL_UINT(ULONG_MAX, out);
}

void test_bb_scalar_parse_uint_overflow_returns_false(void)
{
    // A pure-digit run that exceeds ULONG_MAX on any platform: 21 nines.
    unsigned long out = 123;
    bool ret = bb_scalar_parse_uint("999999999999999999999", &out);

    TEST_ASSERT_FALSE(ret);
    TEST_ASSERT_EQUAL_UINT(123UL, out);
}
