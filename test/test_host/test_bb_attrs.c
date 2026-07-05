// Host tests for bb_attrs (intrusive attrs header).
#include "unity.h"
#include "bb_attrs.h"

#include <string.h>

typedef struct {
    int         id;
    bb_attrs_t  attrs;
    int         payload;
} bb_attrs_test_owner_t;

void test_bb_attrs_container_of_round_trip(void)
{
    bb_attrs_test_owner_t owner = { .id = 42, .payload = 7 };
    owner.attrs.priority        = 3;
    owner.attrs.kind            = 1;
    owner.attrs.tag_mask        = 0x10;
    owner.attrs.delivery_class  = BB_ATTRS_DELIVERY_DEFERRABLE;

    bb_attrs_test_owner_t *recovered =
        bb_attrs_container_of(&owner.attrs, bb_attrs_test_owner_t, attrs);

    TEST_ASSERT_EQUAL_PTR(&owner, recovered);
    TEST_ASSERT_EQUAL_INT(42, recovered->id);
    TEST_ASSERT_EQUAL_INT(7, recovered->payload);
}

void test_bb_attrs_delivery_class_constants(void)
{
    TEST_ASSERT_EQUAL_INT(0, BB_ATTRS_DELIVERY_MUST);
    TEST_ASSERT_EQUAL_INT(1, BB_ATTRS_DELIVERY_DEFERRABLE);
}
