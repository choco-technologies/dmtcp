#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmtcp.h"

static dmtcp_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmtcp_create();
}

void dmod_test_teardown(void)
{
    dmtcp_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmtcp_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmtcp_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmtcp_is_valid(g_handle));
}

DMOD_TEST_STEP(dmtcp_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmtcp_destroy(NULL);
}
