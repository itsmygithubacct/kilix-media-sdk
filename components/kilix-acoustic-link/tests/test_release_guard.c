/*
 * Release-library guard.
 *
 * SPDX-License-Identifier: MIT
 *
 * Built WITHOUT KAL_ENABLE_TEST_MODEM and linked against the shipped
 * archive. The deterministic test backend applies no modulation at all, so a
 * release library that accepted it would put application bytes on a speaker
 * in the clear. It must be refused.
 */
#include "kilix_acoustic_link.h"
#include "kal_test.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    kal_options options;
    kal_link *link = NULL;

    printf("test_release_guard — shipped library rejects the test backend\n");
    memset(&options, 0, sizeof options);
    options.sample_rate = KAL_SAMPLE_RATE;
    options.profile = (uint8_t)KAL_PROFILE_AUDIBLE_NORMAL;
    options.role = (uint8_t)KAL_ROLE_INITIATOR;
    options.modem = (uint8_t)KAL_MODEM_TEST_PASSTHROUGH;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_ERR_UNSUPPORTED);
    KAL_CHECK(link == NULL);

    options.modem = (uint8_t)KAL_MODEM_GGWAVE;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_OK);
    kal_link_free(link);
    link = NULL;

    /* An unknown backend value is refused too. */
    options.modem = 7u;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_ERR_INVALID);
    KAL_GROUP("test backend accepted by the release library", 0u, 0u);
    return kal_test_report("test_release_guard");
}
