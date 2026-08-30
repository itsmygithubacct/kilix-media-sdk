/*
 * Pinned-modem conformance.
 *
 * SPDX-License-Identifier: MIT
 *
 * Every waveform here lives in memory. No device is opened, nothing is
 * played and nothing is recorded, so this suite produces no acoustic
 * evidence and graduates no physical profile.
 */
#include "kilix_acoustic_link.h"
#include "kal_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static kal_options modem_options(uint8_t profile, uint8_t role) {
    kal_options options;
    memset(&options, 0, sizeof options);
    options.sample_rate = KAL_SAMPLE_RATE;
    options.window_frames = 4u;
    options.max_retries = 5u;
    options.profile = profile;
    options.role = role;
    options.modem = (uint8_t)KAL_MODEM_GGWAVE;
    options.allow_high_frequency = 1u;
    return options;
}

/* One frame through the real modem, fed back in fixed-size chunks. */
static int frame_round_trip(uint8_t profile, size_t chunk_samples,
                            uint64_t *airtime_ms, size_t *frame_samples) {
    kal_options tx_options = modem_options(profile, (uint8_t)KAL_ROLE_INITIATOR);
    kal_options rx_options = modem_options(profile, (uint8_t)KAL_ROLE_RESPONDER);
    kal_link *tx = NULL;
    kal_link *rx = NULL;
    kal_stats stats;
    uint8_t payload[KAL_MAX_BEACON_BYTES];
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    int16_t *pcm = NULL;
    size_t capacity;
    size_t written = 0u;
    size_t offset;
    int ok = 0;

    if (kal_link_create(&tx, &tx_options) != KAL_OK) {
        return 0;
    }
    if (kal_link_create(&rx, &rx_options) != KAL_OK) {
        kal_link_free(tx);
        return 0;
    }
    kal_get_stats(tx, &stats);
    capacity = (size_t)stats.frame_airtime_ms * KAL_SAMPLE_RATE / 1000u
        + KAL_SAMPLE_RATE;
    pcm = (int16_t *)calloc(capacity, sizeof(int16_t));
    if (pcm == NULL) {
        kal_link_free(tx);
        kal_link_free(rx);
        return 0;
    }
    memset(payload, 0, sizeof payload);
    for (offset = 0u; offset < sizeof payload; ++offset) {
        payload[offset] = (uint8_t)(offset * 7u + profile);
    }
    if (kal_beacon(tx, payload, sizeof payload) != KAL_OK) {
        goto done;
    }
    if (kal_tx_pull_s16(tx, pcm, capacity, &written, 0u) != KAL_HAVE_OUTPUT
            || written == 0u) {
        goto done;
    }
    for (offset = 0u; offset < written; offset += chunk_samples) {
        size_t take = written - offset;
        if (take > chunk_samples) {
            take = chunk_samples;
        }
        (void)kal_rx_push_s16(rx, pcm + offset, take, 0u);
    }
    if (kal_receive(rx, received, sizeof received, &size) == KAL_OK
            && size == sizeof payload
            && memcmp(received, payload, size) == 0) {
        ok = 1;
    }
    *airtime_ms = stats.frame_airtime_ms;
    *frame_samples = written;

done:
    free(pcm);
    kal_link_free(tx);
    kal_link_free(rx);
    return ok;
}

static void check_every_profile(void) {
    /* Fixed-frame airtime for every enabled profile, at the pinned engine's
     * 64-byte payload mode. These are engine numbers, not room numbers. */
    static const size_t expected_samples[KAL_PROFILE_COUNT] = {
        276480u, 184320u, 92160u, 811008u, 540672u, 276480u
    };
    uint8_t profile;
    size_t decoded = 0u;
    size_t airtime_matches = 0u;

    printf("  profile,frame_samples,airtime_ms,decoded\n");
    for (profile = 0u; profile < (uint8_t)KAL_PROFILE_COUNT; ++profile) {
        uint64_t airtime = 0u;
        size_t samples = 0u;
        const int ok = frame_round_trip(profile, 1024u, &airtime, &samples);
        printf("  %s,%zu,%llu,%d/1\n", kal_profile_string(profile), samples,
               (unsigned long long)airtime, ok);
        decoded += ok ? 1u : 0u;
        if (samples == expected_samples[profile]) {
            airtime_matches++;
        }
        KAL_CHECK(ok == 1);
    }
    KAL_GROUP("profiles decoded through the pinned modem", decoded,
              KAL_PROFILE_COUNT);
    KAL_GROUP("fixed-frame sample counts as pinned", airtime_matches,
              KAL_PROFILE_COUNT);
    KAL_CHECK(decoded == KAL_PROFILE_COUNT);
    KAL_CHECK(airtime_matches == KAL_PROFILE_COUNT);
}

static void check_chunk_boundaries(void) {
    /* The adapter may hand the core any chunk size; decode must not depend
     * on the caller's buffering. */
    static const size_t chunks[] = {1u, 7u, 333u, 1023u, 1024u, 1025u, 4096u, 65536u};
    const size_t count = sizeof chunks / sizeof chunks[0];
    size_t index;
    size_t decoded = 0u;

    for (index = 0u; index < count; ++index) {
        uint64_t airtime = 0u;
        size_t samples = 0u;
        if (frame_round_trip((uint8_t)KAL_PROFILE_AUDIBLE_FASTEST, chunks[index],
                             &airtime, &samples)) {
            decoded++;
        }
    }
    KAL_GROUP("PCM chunk sizes decoded", decoded, count);
    KAL_CHECK(decoded == count);
}

static void check_full_transfer(void) {
    /* A complete session over the real modem: HELLO, negotiation, DATA,
     * folded FIN. Still entirely in memory. */
    kal_options a = modem_options((uint8_t)KAL_PROFILE_AUDIBLE_FASTEST,
                                  (uint8_t)KAL_ROLE_INITIATOR);
    kal_options b = modem_options((uint8_t)KAL_PROFILE_AUDIBLE_FASTEST,
                                  (uint8_t)KAL_ROLE_RESPONDER);
    kal_link *side[2] = {NULL, NULL};
    uint8_t message[200];
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    int16_t *pcm = NULL;
    size_t capacity;
    uint64_t now = 0u;
    unsigned int step;
    int delivered = 0;
    kal_stats stats;
    size_t index;

    KAL_CHECK(kal_link_create(&side[0], &a) == KAL_OK);
    KAL_CHECK(kal_link_create(&side[1], &b) == KAL_OK);
    if (side[0] == NULL || side[1] == NULL) {
        return;
    }
    kal_get_stats(side[0], &stats);
    capacity = (size_t)stats.frame_airtime_ms * KAL_SAMPLE_RATE / 1000u
        + KAL_SAMPLE_RATE;
    pcm = (int16_t *)calloc(capacity, sizeof(int16_t));
    KAL_CHECK(pcm != NULL);
    if (pcm == NULL) {
        kal_link_free(side[0]);
        kal_link_free(side[1]);
        return;
    }
    for (index = 0u; index < sizeof message; ++index) {
        message[index] = (uint8_t)(index * 13u + 5u);
    }
    KAL_CHECK(kal_send(side[0], message, sizeof message) == KAL_OK);
    for (step = 0u; step < 256u && !delivered; ++step) {
        int direction;
        int moved = 0;
        for (direction = 0; direction < 2; ++direction) {
            size_t written = 0u;
            if (kal_tx_pull_s16(side[direction], pcm, capacity, &written, now)
                    != KAL_HAVE_OUTPUT || written == 0u) {
                continue;
            }
            moved = 1;
            (void)kal_rx_push_s16(side[direction == 0 ? 1 : 0], pcm, written, now);
            now += (written * 1000u) / KAL_SAMPLE_RATE;
        }
        if (kal_receive(side[1], received, sizeof received, &size) == KAL_OK) {
            delivered = 1;
        }
        if (!moved && !delivered) {
            uint64_t deadline = 0u;
            if (kal_next_deadline(side[0], &deadline) && deadline > now) {
                now = deadline;
                (void)kal_tick(side[0], now);
                (void)kal_tick(side[1], now);
            } else {
                break;
            }
        }
    }
    kal_get_stats(side[0], &stats);
    KAL_CHECK(delivered == 1);
    KAL_CHECK(size == sizeof message);
    KAL_CHECK(memcmp(received, message, size) == 0);
    KAL_CHECK(stats.messages_sent == 1u);
    /* 200 + 32 digest bytes over 44-byte segments is 6 DATA frames. */
    printf("  full transfer frames sent %u retransmitted %u air %.3fs\n",
           stats.frames_sent, stats.frames_retransmitted, (double)now / 1000.0);
    KAL_CHECK(stats.frames_retransmitted == 0u);
    free(pcm);
    kal_link_free(side[0]);
    kal_link_free(side[1]);
}

static void check_instance_budget(void) {
    /* The pinned engine holds a small fixed number of instances per process.
     * Exhaustion is reported, not worked around. */
    kal_options options = modem_options((uint8_t)KAL_PROFILE_AUDIBLE_FASTEST,
                                        (uint8_t)KAL_ROLE_INITIATOR);
    kal_link *links[8];
    size_t created = 0u;
    size_t index;
    kal_result last = KAL_OK;

    memset(links, 0, sizeof links);
    for (index = 0u; index < 8u; ++index) {
        last = kal_link_create(&links[index], &options);
        if (last != KAL_OK) {
            break;
        }
        created++;
    }
    KAL_CHECK(created >= 1u);
    KAL_CHECK(created < 8u);
    KAL_CHECK(last == KAL_ERR_MEMORY);
    printf("  concurrent ggwave-backed links %zu (engine limit reached: %s)\n",
           created, kal_result_string(last));
    for (index = 0u; index < created; ++index) {
        kal_link_free(links[index]);
    }
    /* The budget is returned on free. */
    KAL_CHECK(kal_link_create(&links[0], &options) == KAL_OK);
    kal_link_free(links[0]);
}

int main(void) {
    printf("test_modem — pinned ggwave conformance (no device, no sound)\n");
    check_every_profile();
    check_chunk_boundaries();
    check_full_transfer();
    check_instance_budget();
    return kal_test_report("test_modem");
}
