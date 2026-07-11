#include "unity.h"
#include "bb_config.h"
#include "bb_storage.h"

// Pure bb_config_type_t -> bb_storage_enc_t / scalar-width metadata,
// table-driven over every bb_config_type_t enumerator. See bb_config.c's
// bb_config_type_to_enc / bb_config_scalar_width.

typedef struct {
    bb_config_type_t type;
    bb_storage_enc_t enc;
    size_t           width;
} type_case_t;

static const type_case_t CASES[] = {
    { BB_CONFIG_BOOL, BB_STORAGE_ENC_U8, 1 },
    { BB_CONFIG_U8, BB_STORAGE_ENC_U8, 1 },
    { BB_CONFIG_U16, BB_STORAGE_ENC_U16, 2 },
    { BB_CONFIG_U32, BB_STORAGE_ENC_U32, 4 },
    { BB_CONFIG_I32, BB_STORAGE_ENC_I32, 4 },
    { BB_CONFIG_STR, BB_STORAGE_ENC_STR, 0 },
    { BB_CONFIG_BLOB, BB_STORAGE_ENC_BLOB, 0 },
};

void test_bb_config_type_to_enc_matches_every_type(void)
{
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(CASES[i].enc, bb_config_type_to_enc(CASES[i].type),
                                   "bb_config_type_to_enc mismatch");
    }
}

void test_bb_config_scalar_width_matches_every_type(void)
{
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(CASES[i].width, bb_config_scalar_width(CASES[i].type),
                                   "bb_config_scalar_width mismatch");
    }
}
