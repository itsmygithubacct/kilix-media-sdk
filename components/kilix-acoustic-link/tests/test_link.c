/*
 * Reliability matrix under virtual time.
 *
 * SPDX-License-Identifier: MIT
 *
 * No test in this file sleeps or reads a real clock: the simulator owns the
 * monotonic value both links see, so every timeout, backoff and retry is
 * reproducible. The deterministic test modem carries one frame per PCM
 * burst, which lets the channel policy drop, duplicate, reorder or corrupt
 * at exact frame boundaries.
 */
#include "kilix_acoustic_link.h"
#include "kal_test.h"

#include <string.h>

#define SIM_PCM_CAPACITY 8192u
#define SIM_MAX_STEPS 4000u

enum channel_action {
    CH_DELIVER = 0,
    CH_DROP = 1,
    CH_DUPLICATE = 2,
    CH_DELAY = 3,
    CH_CORRUPT = 4
};

struct sim;
typedef enum channel_action (*channel_fn)(struct sim *sim, int direction,
                                          unsigned int index);

struct held {
    int16_t pcm[SIM_PCM_CAPACITY];
    size_t length;
    int valid;
};

struct sim {
    kal_link *side[2];
    uint64_t now_ms;
    channel_fn channel;
    unsigned int sent[2];
    unsigned int delivered_frames[2];
    unsigned int dropped[2];
    unsigned int duplicated[2];
    unsigned int delayed[2];
    unsigned int corrupted[2];
    struct held hold[2];
    uint64_t airtime_ms;
    int cancel_seen[2];
};

static enum channel_action channel_clean(struct sim *sim, int direction,
                                         unsigned int index) {
    (void)sim; (void)direction; (void)index;
    return CH_DELIVER;
}

static kal_options base_options(uint8_t role) {
    kal_options options;
    memset(&options, 0, sizeof options);
    options.sample_rate = KAL_SAMPLE_RATE;
    options.max_message_bytes = KAL_MAX_MESSAGE_BYTES;
    options.window_frames = 4u;
    options.max_retries = 5u;
    options.profile = (uint8_t)KAL_PROFILE_AUDIBLE_NORMAL;
    options.role = role;
    options.modem = (uint8_t)KAL_MODEM_TEST_PASSTHROUGH;
    return options;
}

static int sim_init(struct sim *sim, channel_fn channel, uint16_t window) {
    kal_options a = base_options((uint8_t)KAL_ROLE_INITIATOR);
    kal_options b = base_options((uint8_t)KAL_ROLE_RESPONDER);
    kal_stats stats;

    memset(sim, 0, sizeof *sim);
    a.window_frames = window;
    b.window_frames = window;
    if (kal_link_create(&sim->side[0], &a) != KAL_OK) {
        return 0;
    }
    if (kal_link_create(&sim->side[1], &b) != KAL_OK) {
        return 0;
    }
    sim->channel = channel == NULL ? channel_clean : channel;
    kal_get_stats(sim->side[0], &stats);
    sim->airtime_ms = stats.frame_airtime_ms == 0u ? 1u : stats.frame_airtime_ms;
    return 1;
}

static void sim_free(struct sim *sim) {
    kal_link_free(sim->side[0]);
    kal_link_free(sim->side[1]);
    sim->side[0] = NULL;
    sim->side[1] = NULL;
}

static void deliver(struct sim *sim, int direction, const int16_t *pcm,
                    size_t length) {
    const int peer = direction == 0 ? 1 : 0;
    const kal_result result =
        kal_rx_push_s16(sim->side[peer], pcm, length, sim->now_ms);
    if (result == KAL_ERR_CANCELLED) {
        sim->cancel_seen[peer] = 1;
    }
    sim->delivered_frames[direction]++;
}

/* One half-duplex turn: each side offers at most one frame of PCM. */
static int sim_step(struct sim *sim) {
    int direction;
    int moved = 0;

    for (direction = 0; direction < 2; ++direction) {
        int16_t pcm[SIM_PCM_CAPACITY];
        size_t written = 0u;
        kal_result pulled;
        enum channel_action action;

        pulled = kal_tx_pull_s16(sim->side[direction], pcm, SIM_PCM_CAPACITY,
                                 &written, sim->now_ms);
        if (pulled != KAL_HAVE_OUTPUT || written == 0u) {
            continue;
        }
        moved = 1;
        sim->sent[direction]++;
        action = sim->channel(sim, direction, sim->sent[direction] - 1u);
        switch (action) {
        case CH_DROP:
            sim->dropped[direction]++;
            break;
        case CH_DUPLICATE:
            /* A real repeat costs its own airtime; an instant repeat is an
             * engine artefact and is covered separately. */
            sim->duplicated[direction]++;
            deliver(sim, direction, pcm, written);
            sim->now_ms += sim->airtime_ms * 3u;
            deliver(sim, direction, pcm, written);
            break;
        case CH_DELAY:
            sim->delayed[direction]++;
            if (sim->hold[direction].valid) {
                deliver(sim, direction, sim->hold[direction].pcm,
                        sim->hold[direction].length);
            }
            memcpy(sim->hold[direction].pcm, pcm, written * sizeof(int16_t));
            sim->hold[direction].length = written;
            sim->hold[direction].valid = 1;
            break;
        case CH_CORRUPT: {
            int16_t mutated[SIM_PCM_CAPACITY];
            sim->corrupted[direction]++;
            memcpy(mutated, pcm, written * sizeof(int16_t));
            mutated[written / 2u] = (int16_t)((mutated[written / 2u] + 1) & 0xff);
            deliver(sim, direction, mutated, written);
            break;
        }
        case CH_DELIVER:
        default:
            deliver(sim, direction, pcm, written);
            break;
        }
        sim->now_ms += sim->airtime_ms;
    }

    if (!moved) {
        /* Nothing on air: jump to the earliest armed deadline. */
        uint64_t earliest = UINT64_MAX;
        uint64_t deadline = 0u;
        int index;
        for (index = 0; index < 2; ++index) {
            if (kal_next_deadline(sim->side[index], &deadline)
                    && deadline < earliest) {
                earliest = deadline;
            }
        }
        if (earliest == UINT64_MAX) {
            return 0;
        }
        sim->now_ms = earliest > sim->now_ms ? earliest : sim->now_ms + 1u;
        for (index = 0; index < 2; ++index) {
            (void)kal_tick(sim->side[index], sim->now_ms);
        }
    }
    /* Flush any held frame once the channel goes quiet. */
    for (direction = 0; direction < 2; ++direction) {
        if (!moved && sim->hold[direction].valid) {
            deliver(sim, direction, sim->hold[direction].pcm,
                    sim->hold[direction].length);
            sim->hold[direction].valid = 0;
        }
    }
    return 1;
}

static int run_until_delivered(struct sim *sim, int receiver, uint8_t *out,
                              size_t capacity, size_t *size,
                              unsigned int max_steps) {
    unsigned int step;
    for (step = 0u; step < max_steps; ++step) {
        if (kal_receive(sim->side[receiver], out, capacity, size) == KAL_OK) {
            /* Drain the acknowledgement that follows delivery. */
            unsigned int tail;
            for (tail = 0u; tail < 16u; ++tail) {
                (void)sim_step(sim);
            }
            return 1;
        }
        if (!sim_step(sim)) {
            break;
        }
    }
    return kal_receive(sim->side[receiver], out, capacity, size) == KAL_OK;
}

static void fill_pattern(uint8_t *buffer, size_t size, unsigned int seed) {
    size_t index;
    for (index = 0u; index < size; ++index) {
        buffer[index] = (uint8_t)((index * 31u + seed * 17u + 11u) & 0xffu);
    }
}

/* ------------------------------------------------------------- scenarios */

static enum channel_action channel_drop_every_third(struct sim *sim,
                                                    int direction,
                                                    unsigned int index) {
    (void)sim;
    if (direction == 0 && index != 0u && (index % 3u) == 0u) {
        return CH_DROP;
    }
    return CH_DELIVER;
}

static enum channel_action channel_duplicate_all(struct sim *sim, int direction,
                                                 unsigned int index) {
    (void)sim; (void)direction; (void)index;
    return CH_DUPLICATE;
}

static enum channel_action channel_reorder(struct sim *sim, int direction,
                                           unsigned int index) {
    (void)sim;
    if (direction == 0 && (index % 2u) == 1u) {
        return CH_DELAY;
    }
    return CH_DELIVER;
}

static enum channel_action channel_drop_first_acks(struct sim *sim,
                                                   int direction,
                                                   unsigned int index) {
    (void)sim;
    if (direction == 1 && index >= 1u && index <= 2u) {
        return CH_DROP;
    }
    return CH_DELIVER;
}

static enum channel_action channel_corrupt_every_fourth(struct sim *sim,
                                                        int direction,
                                                        unsigned int index) {
    (void)sim;
    if (direction == 0 && index != 0u && (index % 4u) == 0u) {
        return CH_CORRUPT;
    }
    return CH_DELIVER;
}

static enum channel_action channel_block_all_data(struct sim *sim,
                                                  int direction,
                                                  unsigned int index) {
    (void)sim; (void)index;
    return direction == 0 ? CH_DROP : CH_DELIVER;
}

struct scenario {
    const char *name;
    channel_fn channel;
    size_t message_size;
    uint16_t window;
};

static const struct scenario scenarios[] = {
    {"clean-1-byte",            channel_clean,                 1u,    4u},
    {"clean-32-byte",           channel_clean,                 32u,   4u},
    {"clean-64-byte",           channel_clean,                 64u,   4u},
    {"clean-256-byte",          channel_clean,                 256u,  4u},
    {"clean-4096-byte",         channel_clean,                 4096u, 4u},
    {"clean-window-1",          channel_clean,                 256u,  1u},
    {"clean-window-16",         channel_clean,                 4096u, 16u},
    {"loss-every-third-data",   channel_drop_every_third,      512u,  4u},
    {"duplicate-every-frame",   channel_duplicate_all,         256u,  4u},
    {"reorder-alternating",     channel_reorder,               512u,  4u},
    {"delayed-and-lost-acks",   channel_drop_first_acks,       512u,  4u},
    {"corrupt-every-fourth",    channel_corrupt_every_fourth,  512u,  4u}
};

static void check_scenarios(void) {
    const size_t count = sizeof scenarios / sizeof scenarios[0];
    size_t index;
    size_t passed = 0u;

    for (index = 0u; index < count; ++index) {
        struct sim sim;
        uint8_t message[KAL_MAX_MESSAGE_BYTES];
        uint8_t received[KAL_MAX_MESSAGE_BYTES];
        size_t size = 0u;
        int ok;

        if (!sim_init(&sim, scenarios[index].channel, scenarios[index].window)) {
            KAL_CHECK(0);
            continue;
        }
        fill_pattern(message, scenarios[index].message_size, (unsigned int)index);
        ok = kal_send(sim.side[0], message, scenarios[index].message_size) == KAL_OK;
        ok = ok && run_until_delivered(&sim, 1, received, sizeof received, &size,
                                       SIM_MAX_STEPS);
        ok = ok && size == scenarios[index].message_size;
        ok = ok && memcmp(received, message, size) == 0;
        ok = ok && kal_tx_status(sim.side[0]) == KAL_OK;
        if (!ok) {
            fprintf(stderr, "scenario %s failed (size=%zu)\n",
                    scenarios[index].name, size);
        }
        passed += ok ? 1u : 0u;
        KAL_CHECK(ok);
        sim_free(&sim);
    }
    KAL_GROUP("delivery scenarios", passed, count);
}

static void check_no_double_delivery(void) {
    /* The delivered ACK is lost, so the sender retransmits the final DATA.
     * The receiver must replay its cached ACK and deliver exactly once. */
    struct sim sim;
    uint8_t message[128];
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    unsigned int deliveries = 0u;
    unsigned int step;
    kal_stats stats;

    KAL_CHECK(sim_init(&sim, channel_clean, 4u));
    fill_pattern(message, sizeof message, 5u);
    KAL_CHECK(kal_send(sim.side[0], message, sizeof message) == KAL_OK);
    for (step = 0u; step < SIM_MAX_STEPS; ++step) {
        if (kal_receive(sim.side[1], received, sizeof received, &size) == KAL_OK) {
            deliveries++;
        }
        if (!sim_step(&sim)) {
            break;
        }
    }
    KAL_CHECK(deliveries == 1u);
    KAL_CHECK(size == sizeof message);
    kal_get_stats(sim.side[1], &stats);
    KAL_CHECK(stats.messages_delivered == 1u);
    KAL_GROUP("application deliveries for one message", deliveries, 1u);
    sim_free(&sim);
}

static void check_retry_exhaustion(void) {
    struct sim sim;
    uint8_t message[64];
    unsigned int step;
    kal_stats stats;

    KAL_CHECK(sim_init(&sim, channel_block_all_data, 4u));
    fill_pattern(message, sizeof message, 9u);
    KAL_CHECK(kal_send(sim.side[0], message, sizeof message) == KAL_OK);
    for (step = 0u; step < SIM_MAX_STEPS; ++step) {
        if (kal_tx_status(sim.side[0]) == KAL_ERR_TIMEOUT) {
            break;
        }
        if (!sim_step(&sim)) {
            break;
        }
    }
    /* Bounded failure, not an unbounded retry loop. */
    KAL_CHECK(kal_tx_status(sim.side[0]) == KAL_ERR_TIMEOUT);
    kal_get_stats(sim.side[0], &stats);
    KAL_CHECK(stats.timeouts >= 1u);
    KAL_CHECK(stats.messages_sent == 0u);
    sim_free(&sim);
}

static void check_backoff_is_bounded_and_growing(void) {
    struct sim sim;
    uint8_t message[64];
    uint64_t deadlines[8];
    size_t observed = 0u;
    unsigned int step;
    size_t index;
    int monotonic = 1;

    KAL_CHECK(sim_init(&sim, channel_block_all_data, 4u));
    fill_pattern(message, sizeof message, 3u);
    KAL_CHECK(kal_send(sim.side[0], message, sizeof message) == KAL_OK);
    for (step = 0u; step < SIM_MAX_STEPS && observed < 8u; ++step) {
        uint64_t deadline = 0u;
        if (kal_next_deadline(sim.side[0], &deadline)) {
            if (observed == 0u || deadlines[observed - 1u] != deadline) {
                deadlines[observed++] = deadline;
            }
        }
        if (!sim_step(&sim)) {
            break;
        }
    }
    for (index = 1u; index < observed; ++index) {
        if (deadlines[index] <= deadlines[index - 1u]) {
            monotonic = 0;
        }
    }
    KAL_CHECK(observed >= 3u);
    KAL_CHECK(monotonic == 1);
    KAL_GROUP("distinct retransmission deadlines", observed, observed);
    sim_free(&sim);
}

static void check_cancel(void) {
    struct sim sim;
    uint8_t message[512];
    unsigned int step;

    KAL_CHECK(sim_init(&sim, channel_clean, 4u));
    fill_pattern(message, sizeof message, 4u);
    KAL_CHECK(kal_send(sim.side[0], message, sizeof message) == KAL_OK);
    for (step = 0u; step < 6u; ++step) {
        (void)sim_step(&sim);
    }
    KAL_CHECK(kal_cancel(sim.side[0], (uint16_t)KAL_CANCEL_USER) == KAL_OK);
    for (step = 0u; step < 40u; ++step) {
        (void)sim_step(&sim);
    }
    KAL_CHECK(kal_tx_status(sim.side[0]) == KAL_ERR_CANCELLED);
    KAL_CHECK(sim.cancel_seen[1] == 1);
    {
        uint8_t received[KAL_MAX_MESSAGE_BYTES];
        size_t size = 0u;
        /* A cancelled transfer delivers nothing. */
        KAL_CHECK(kal_receive(sim.side[1], received, sizeof received, &size)
                  == KAL_WANT_INPUT);
    }
    sim_free(&sim);
}

static void check_simultaneous_initiation(void) {
    /* Two initiators start at once. Exactly one session must survive and the
     * message must still arrive. */
    kal_options a = base_options((uint8_t)KAL_ROLE_INITIATOR);
    kal_options b = base_options((uint8_t)KAL_ROLE_INITIATOR);
    struct sim sim;
    uint8_t message[64];
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    kal_stats stats_a;
    kal_stats stats_b;
    unsigned int step;
    int delivered = 0;

    memset(&sim, 0, sizeof sim);
    KAL_CHECK(kal_link_create(&sim.side[0], &a) == KAL_OK);
    KAL_CHECK(kal_link_create(&sim.side[1], &b) == KAL_OK);
    sim.channel = channel_clean;
    kal_get_stats(sim.side[0], &stats_a);
    sim.airtime_ms = stats_a.frame_airtime_ms == 0u ? 1u : stats_a.frame_airtime_ms;

    fill_pattern(message, sizeof message, 2u);
    KAL_CHECK(kal_send(sim.side[0], message, sizeof message) == KAL_OK);
    KAL_CHECK(kal_send(sim.side[1], message, sizeof message) == KAL_OK);
    for (step = 0u; step < SIM_MAX_STEPS; ++step) {
        if (kal_receive(sim.side[0], received, sizeof received, &size) == KAL_OK
                || kal_receive(sim.side[1], received, sizeof received, &size) == KAL_OK) {
            delivered = 1;
            break;
        }
        if (!sim_step(&sim)) {
            break;
        }
    }
    kal_get_stats(sim.side[0], &stats_a);
    kal_get_stats(sim.side[1], &stats_b);
    KAL_CHECK(delivered == 1);
    KAL_CHECK(size == sizeof message);
    KAL_CHECK(memcmp(received, message, size) == 0);
    /* Both sides converged on one session identifier. */
    KAL_CHECK(stats_a.session_open == 1u && stats_b.session_open == 1u);
    KAL_CHECK(stats_a.session_id == stats_b.session_id);
    sim_free(&sim);
}

static void check_persistent_session_ceremony(void) {
    /*
     * The F111/F113 ceremony floor, executed rather than asserted: five
     * application messages over ONE persistent session, alternating
     * direction. The recorded no-loss floor is 18 DATA frames, 5 delivered
     * ACKs and 2 session-opening frames = 25 transmissions.
     */
    static const size_t ceremony_sizes[5] = {102u, 118u, 102u, 90u, 90u};
    static const unsigned int expected_data[5] = {4u, 4u, 4u, 3u, 3u};
    struct sim sim;
    size_t index;
    unsigned int delivered_messages = 0u;
    unsigned int total_data = 0u;
    kal_stats stats_a;
    kal_stats stats_b;
    uint32_t opening_session = 0u;

    KAL_CHECK(sim_init(&sim, channel_clean, 4u));
    for (index = 0u; index < 5u; ++index) {
        const int sender = (index % 2u) == 0u ? 0 : 1;
        const int receiver = sender == 0 ? 1 : 0;
        uint8_t message[128];
        uint8_t received[KAL_MAX_MESSAGE_BYTES];
        size_t size = 0u;

        fill_pattern(message, ceremony_sizes[index], (unsigned int)index + 40u);
        KAL_CHECK(kal_send(sim.side[sender], message, ceremony_sizes[index]) == KAL_OK);
        if (run_until_delivered(&sim, receiver, received, sizeof received, &size,
                                SIM_MAX_STEPS)
                && size == ceremony_sizes[index]
                && memcmp(received, message, size) == 0) {
            delivered_messages++;
        }
        total_data += expected_data[index];
        if (index == 0u) {
            kal_get_stats(sim.side[0], &stats_a);
            opening_session = stats_a.session_id;
        }
    }
    kal_get_stats(sim.side[0], &stats_a);
    kal_get_stats(sim.side[1], &stats_b);

    KAL_GROUP("ceremony messages delivered", delivered_messages, 5u);
    KAL_CHECK(delivered_messages == 5u);
    KAL_GROUP("ceremony DATA frames (floor)", total_data, 18u);
    KAL_CHECK(total_data == 18u);
    /* One session opening for the whole ceremony, not one per message. */
    KAL_CHECK(stats_a.session_id == opening_session);
    KAL_CHECK(stats_a.session_id == stats_b.session_id);
    KAL_CHECK(sim.sent[0] + sim.sent[1] >= 25u);
    printf("  ceremony transmissions observed         %u (floor 25)\n",
           sim.sent[0] + sim.sent[1]);
    sim_free(&sim);
}

static void check_engine_repeat_suppressed(void) {
    /* The same frame delivered twice inside one frame airtime is one
     * transmission reported twice, and must reach the protocol once. */
    struct sim sim;
    uint8_t payload[16];
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    int16_t pcm[SIM_PCM_CAPACITY];
    size_t written = 0u;
    kal_stats stats;
    unsigned int deliveries = 0u;

    KAL_CHECK(sim_init(&sim, channel_clean, 4u));
    fill_pattern(payload, sizeof payload, 12u);
    KAL_CHECK(kal_beacon(sim.side[0], payload, sizeof payload) == KAL_OK);
    KAL_CHECK(kal_tx_pull_s16(sim.side[0], pcm, SIM_PCM_CAPACITY, &written,
                              sim.now_ms) == KAL_HAVE_OUTPUT);
    (void)kal_rx_push_s16(sim.side[1], pcm, written, sim.now_ms);
    (void)kal_rx_push_s16(sim.side[1], pcm, written, sim.now_ms);
    (void)kal_rx_push_s16(sim.side[1], pcm, written, sim.now_ms);
    while (kal_receive(sim.side[1], received, sizeof received, &size) == KAL_OK) {
        deliveries++;
    }
    kal_get_stats(sim.side[1], &stats);
    KAL_CHECK(deliveries == 1u);
    KAL_CHECK(stats.duplicates_suppressed >= 2u);
    KAL_GROUP("instant repeats delivered", deliveries, 1u);
    sim_free(&sim);
}

static void check_beacon(void) {
    struct sim sim;
    uint8_t payload[32];
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    unsigned int step;

    KAL_CHECK(sim_init(&sim, channel_clean, 4u));
    fill_pattern(payload, sizeof payload, 8u);
    KAL_CHECK(kal_beacon(sim.side[0], payload, sizeof payload) == KAL_OK);
    for (step = 0u; step < 32u; ++step) {
        (void)sim_step(&sim);
    }
    KAL_CHECK(kal_receive(sim.side[1], received, sizeof received, &size) == KAL_OK);
    KAL_CHECK(size == sizeof payload);
    KAL_CHECK(memcmp(received, payload, size) == 0);
    /* A beacon over the size limit is refused before any sound is produced. */
    KAL_CHECK(kal_beacon(sim.side[0], payload, KAL_MAX_BEACON_BYTES + 1u)
              == KAL_ERR_INVALID);
    KAL_CHECK(kal_beacon(sim.side[0], payload, 0u) == KAL_ERR_INVALID);
    sim_free(&sim);
}

static void check_reset_and_reuse(void) {
    struct sim sim;
    uint8_t message[64];
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;

    KAL_CHECK(sim_init(&sim, channel_clean, 4u));
    fill_pattern(message, sizeof message, 6u);
    KAL_CHECK(kal_send(sim.side[0], message, sizeof message) == KAL_OK);
    KAL_CHECK(run_until_delivered(&sim, 1, received, sizeof received, &size,
                                  SIM_MAX_STEPS));
    kal_link_reset(sim.side[0]);
    kal_link_reset(sim.side[1]);
    {
        kal_stats stats;
        kal_get_stats(sim.side[0], &stats);
        KAL_CHECK(stats.session_open == 0u);
    }
    /* A reset link re-opens a session and transfers again. */
    KAL_CHECK(kal_send(sim.side[0], message, sizeof message) == KAL_OK);
    size = 0u;
    KAL_CHECK(run_until_delivered(&sim, 1, received, sizeof received, &size,
                                  SIM_MAX_STEPS));
    KAL_CHECK(size == sizeof message);
    KAL_CHECK(memcmp(received, message, size) == 0);
    sim_free(&sim);
}

static void check_no_sleep_in_source(void) {
    /* Guard the "virtual time only" property in the test source itself. */
    FILE *file = fopen("tests/test_link.c", "r");
    char line[512];
    int offenders = 0;
    KAL_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    while (fgets(line, (int)sizeof line, file) != NULL) {
        if (strstr(line, "sleep(") != NULL || strstr(line, "nanosleep") != NULL
                || strstr(line, "clock_gettime") != NULL) {
            if (strstr(line, "strstr") == NULL) {
                offenders++;
            }
        }
    }
    (void)fclose(file);
    KAL_CHECK(offenders == 0);
    KAL_GROUP("real-clock or sleep calls in scenarios", 0u, 0u);
}

int main(void) {
    printf("test_link — reliability matrix under virtual time\n");
    check_scenarios();
    check_no_double_delivery();
    check_retry_exhaustion();
    check_backoff_is_bounded_and_growing();
    check_cancel();
    check_simultaneous_initiation();
    check_persistent_session_ceremony();
    check_engine_repeat_suppressed();
    check_beacon();
    check_reset_and_reuse();
    check_no_sleep_in_source();
    return kal_test_report("test_link");
}
