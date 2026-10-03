/*
 * Bounds, hostile input and integrity.
 *
 * SPDX-License-Identifier: MIT
 *
 * Nothing here opens a device. The deterministic test modem lets a hostile
 * peer be simulated exactly at the frame boundary.
 */
#include "kilix_acoustic_link.h"
#include "kal_frame.h"
#include "kal_sha256.h"
#include "kal_test.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* These counters cover direct allocator calls from the linked objects,
 * including the core; they do not interpose inside shared libc/libstdc++.
 * Keep the RSS check as an independent observer. */
static int measuring_allocations;
static unsigned long allocation_calls;
static unsigned long deallocation_calls;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);
void __wrap_free(void *pointer);

void *__wrap_malloc(size_t size) {
    if (measuring_allocations) allocation_calls++;
    return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
    if (measuring_allocations) allocation_calls++;
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *pointer, size_t size) {
    if (measuring_allocations) allocation_calls++;
    return __real_realloc(pointer, size);
}
void __wrap_free(void *pointer) {
    if (measuring_allocations) deallocation_calls++;
    __real_free(pointer);
}

#define TEST_SYNC_SAMPLES 4u
#define TEST_FRAME_SAMPLES (TEST_SYNC_SAMPLES + KAL_FRAME_BYTES)

/* Mirrors the deterministic test backend's mapping so a test can put an
 * arbitrary frame on the wire without a modem. */
static size_t modulate(const uint8_t frame[KAL_FRAME_BYTES], int16_t *pcm) {
    size_t index;
    pcm[0] = (int16_t)-32768;
    pcm[1] = (int16_t)32767;
    pcm[2] = (int16_t)-32768;
    pcm[3] = (int16_t)32767;
    for (index = 0u; index < KAL_FRAME_BYTES; ++index) {
        pcm[TEST_SYNC_SAMPLES + index] = (int16_t)frame[index];
    }
    return TEST_FRAME_SAMPLES;
}

static kal_options options_for(uint8_t role) {
    kal_options options;
    memset(&options, 0, sizeof options);
    options.sample_rate = KAL_SAMPLE_RATE;
    options.window_frames = 4u;
    options.max_retries = 5u;
    options.profile = (uint8_t)KAL_PROFILE_AUDIBLE_NORMAL;
    options.role = role;
    options.modem = (uint8_t)KAL_MODEM_TEST_PASSTHROUGH;
    return options;
}

static long resident_kib(void) {
    FILE *file = fopen("/proc/self/smaps_rollup", "r");
    char line[256];
    long resident = -1;
    if (file == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, file) != NULL) {
        if (sscanf(line, "Rss: %ld kB", &resident) == 1) break;
    }
    (void)fclose(file);
    return resident;
}

static void check_rss_bound(long before, long after, const char *name) {
    KAL_CHECK(before > 0 && after > 0);
#if defined(__SANITIZE_ADDRESS__)
    /* Preserve the original separate ASan allowance of 1024 pages. */
    const long page_kib = sysconf(_SC_PAGESIZE) / 1024;
    KAL_CHECK(page_kib > 0);
    KAL_CHECK((after - before) < 1024 * page_kib);
    printf("  %s RSS delta (ASan accounting, KiB) %ld\n", name, after - before);
#else
    KAL_CHECK(after <= before);
    printf("  %s accurate RSS delta (KiB) %ld\n", name, after - before);
#endif
}

static void check_option_validation(void) {
    kal_link *link = NULL;
    kal_options options;
    size_t accepted = 0u;
    size_t rejected = 0u;
    uint16_t window;

    options = options_for((uint8_t)KAL_ROLE_INITIATOR);
    options.sample_rate = 44100u;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_ERR_UNSUPPORTED);
    KAL_CHECK(link == NULL);

    options = options_for((uint8_t)KAL_ROLE_INITIATOR);
    options.max_message_bytes = KAL_MAX_MESSAGE_BYTES + 1u;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_ERR_INVALID);

    options = options_for((uint8_t)KAL_ROLE_INITIATOR);
    options.profile = (uint8_t)KAL_PROFILE_COUNT;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_ERR_INVALID);

    /* The high-frequency profile is refused unless the consumer opts in. */
    options = options_for((uint8_t)KAL_ROLE_INITIATOR);
    options.profile = (uint8_t)KAL_PROFILE_HIGH_NORMAL;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_ERR_UNSUPPORTED);
    options.allow_high_frequency = 1u;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_OK);
    kal_link_free(link);
    link = NULL;

    options = options_for((uint8_t)KAL_ROLE_INITIATOR);
    options.reserved = 1u;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_ERR_INVALID);

    options = options_for(9u);
    KAL_CHECK(kal_link_create(&link, &options) == KAL_ERR_INVALID);

    for (window = 0u; window <= 20u; ++window) {
        kal_options candidate = options_for((uint8_t)KAL_ROLE_INITIATOR);
        kal_link *created = NULL;
        candidate.window_frames = window;
        if (kal_link_create(&created, &candidate) == KAL_OK) {
            accepted++;
            kal_link_free(created);
        } else {
            rejected++;
        }
    }
    /* 0 means "default", 1..16 are legal, 17..20 are refused. */
    KAL_GROUP("window values accepted", accepted, 17u);
    KAL_GROUP("window values refused", rejected, 4u);
    KAL_CHECK(accepted == 17u);
    KAL_CHECK(rejected == 4u);
}

static void check_message_size_limits(void) {
    kal_options options = options_for((uint8_t)KAL_ROLE_INITIATOR);
    kal_link *link = NULL;
    uint8_t message[KAL_MAX_MESSAGE_BYTES + 1u];

    memset(message, 0x5a, sizeof message);
    KAL_CHECK(kal_link_create(&link, &options) == KAL_OK);
    KAL_CHECK(kal_send(link, message, KAL_MAX_MESSAGE_BYTES + 1u) == KAL_ERR_INVALID);
    KAL_CHECK(kal_send(link, message, KAL_MAX_MESSAGE_BYTES) == KAL_OK);
    /* One transfer at a time. */
    KAL_CHECK(kal_send(link, message, 16u) == KAL_ERR_BUSY);
    kal_link_free(link);

    options.max_message_bytes = 64u;
    link = NULL;
    KAL_CHECK(kal_link_create(&link, &options) == KAL_OK);
    KAL_CHECK(kal_send(link, message, 65u) == KAL_ERR_INVALID);
    KAL_CHECK(kal_send(link, message, 64u) == KAL_OK);
    kal_link_free(link);
}

/* Open a session against a responder, then hand it arbitrary frames. */
static kal_link *open_responder(uint32_t *session_id, uint16_t *hello_message) {
    kal_options initiator = options_for((uint8_t)KAL_ROLE_INITIATOR);
    kal_options responder = options_for((uint8_t)KAL_ROLE_RESPONDER);
    kal_link *a = NULL;
    kal_link *b = NULL;
    int16_t pcm[512];
    size_t written = 0u;
    uint8_t probe[16];
    kal_stats stats;

    if (kal_link_create(&a, &initiator) != KAL_OK) {
        return NULL;
    }
    if (kal_link_create(&b, &responder) != KAL_OK) {
        kal_link_free(a);
        return NULL;
    }
    memset(probe, 0, sizeof probe);
    (void)kal_send(a, probe, sizeof probe);
    (void)kal_tx_pull_s16(a, pcm, sizeof pcm / sizeof pcm[0], &written, 0u);
    (void)kal_rx_push_s16(b, pcm, written, 0u);
    kal_get_stats(b, &stats);
    *session_id = stats.session_id;
    *hello_message = 0u;
    kal_link_free(a);
    return b;
}

static void check_corrupted_complete_message_is_never_delivered(void) {
    uint32_t session = 0u;
    uint16_t hello_message = 0u;
    kal_link *b = open_responder(&session, &hello_message);
    uint8_t application[80];
    uint8_t transfer[80 + KAL_DIGEST_BYTES];
    uint16_t segments = 0u;
    uint16_t sequence;
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    kal_stats stats;
    uint64_t now = 100u;

    KAL_CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    memset(application, 0x7e, sizeof application);
    memcpy(transfer, application, sizeof application);
    KAL_CHECK(kal_sha256(application, sizeof application,
                         transfer + sizeof application) == 0);
    /* One bit of the digest trailer is wrong: every frame is individually
     * valid and the whole message must still never be delivered. */
    transfer[sizeof application + 7u] ^= 0x01u;
    KAL_CHECK(kal_segment_count(sizeof application, &segments) == KAL_WIRE_OK);

    for (sequence = 0u; sequence < segments; ++sequence) {
        struct kal_frame frame;
        uint8_t encoded[KAL_FRAME_BYTES];
        int16_t pcm[TEST_FRAME_SAMPLES];
        const size_t offset = (size_t)sequence * KAL_PAYLOAD_BYTES;
        size_t chunk = sizeof transfer - offset;
        if (chunk > KAL_PAYLOAD_BYTES) {
            chunk = KAL_PAYLOAD_BYTES;
        }
        memset(&frame, 0, sizeof frame);
        frame.type = (uint8_t)KAL_TYPE_DATA;
        frame.session_id = session;
        frame.message_id = 0u;
        frame.sequence = sequence;
        frame.value = segments;
        frame.payload_length = (uint8_t)chunk;
        frame.flags = (sequence == (uint16_t)(segments - 1u))
            ? KAL_FLAG_DATA_FINAL : 0u;
        memcpy(frame.payload, transfer + offset, chunk);
        KAL_CHECK(kal_frame_encode(&frame, encoded) == KAL_WIRE_OK);
        (void)modulate(encoded, pcm);
        (void)kal_rx_push_s16(b, pcm, TEST_FRAME_SAMPLES, now);
        now += 100u;
    }
    KAL_CHECK(kal_receive(b, received, sizeof received, &size) == KAL_WANT_INPUT);
    kal_get_stats(b, &stats);
    KAL_CHECK(stats.messages_delivered == 0u);
    KAL_GROUP("corrupted complete messages delivered", 0u, 0u);
    kal_link_free(b);
}

static void check_hostile_flood_is_bounded(void) {
    /*
     * A hostile peer feeds junk frames, beacons for endless message ids and
     * partial sessions that never complete. Steady memory is the property:
     * the PCM and frame paths allocate nothing.
     */
    kal_options options = options_for((uint8_t)KAL_ROLE_RESPONDER);
    kal_link *link = NULL;
    long before;
    long after;
    unsigned int round;
    unsigned int index;
    kal_stats stats;
    uint64_t now = 0u;

    KAL_CHECK(kal_link_create(&link, &options) == KAL_OK);
    if (link == NULL) {
        return;
    }
    /* Warm up so first-touch page faults are not counted as growth. */
    for (round = 0u; round < 2u; ++round) {
        for (index = 0u; index < 2000u; ++index) {
            struct kal_frame frame;
            uint8_t encoded[KAL_FRAME_BYTES];
            int16_t pcm[TEST_FRAME_SAMPLES];
            memset(&frame, 0, sizeof frame);
            frame.type = (uint8_t)KAL_TYPE_BEACON;
            frame.session_id = 0x1000u + index;
            frame.message_id = (uint16_t)index;
            frame.payload_length = 8u;
            memset(frame.payload, (int)(index & 0xffu), 8u);
            if (kal_frame_encode(&frame, encoded) == KAL_WIRE_OK) {
                (void)modulate(encoded, pcm);
                (void)kal_rx_push_s16(link, pcm, TEST_FRAME_SAMPLES, now);
            }
            now += 10u;
        }
    }
    /* Warm the observer too. statm's asynchronous RSS accounting cannot
     * enforce an exact zero-growth bound. smaps_rollup reads accurate RSS. */
    (void)resident_kib();
    (void)resident_kib();
    before = resident_kib();
    for (round = 0u; round < 4u; ++round) {
        for (index = 0u; index < 2000u; ++index) {
            struct kal_frame frame;
            uint8_t encoded[KAL_FRAME_BYTES];
            int16_t pcm[TEST_FRAME_SAMPLES];
            /* Alternate junk, incomplete DATA and unaccepted beacons. */
            memset(&frame, 0, sizeof frame);
            if ((index % 3u) == 0u) {
                frame.type = (uint8_t)KAL_TYPE_DATA;
                frame.session_id = 0x2000u + index;
                frame.message_id = (uint16_t)index;
                frame.sequence = 0u;
                frame.value = 90u;
                frame.payload_length = (uint8_t)KAL_PAYLOAD_BYTES;
            } else if ((index % 3u) == 1u) {
                frame.type = (uint8_t)KAL_TYPE_BEACON;
                frame.session_id = 0x3000u + index;
                frame.message_id = (uint16_t)index;
                frame.payload_length = (uint8_t)KAL_MAX_BEACON_BYTES;
            } else {
                frame.type = (uint8_t)KAL_TYPE_ACK;
                frame.session_id = 0x4000u + index;
                frame.message_id = (uint16_t)index;
                frame.sequence = 3u;
            }
            memset(frame.payload, (int)(index & 0xffu), frame.payload_length);
            if (kal_frame_encode(&frame, encoded) == KAL_WIRE_OK) {
                (void)modulate(encoded, pcm);
                (void)kal_rx_push_s16(link, pcm, TEST_FRAME_SAMPLES, now);
            }
            now += 10u;
        }
    }
    after = resident_kib();
    kal_get_stats(link, &stats);
    check_rss_bound(before, after, "8000 hostile frames");
    printf("  hostile frames pushed                        %u/%u\n", 8000u, 8000u);
    kal_link_free(link);
}

static kal_result push_frame(kal_link *link, struct kal_frame *frame,
                             uint64_t *now) {
    uint8_t encoded[KAL_FRAME_BYTES];
    int16_t pcm[TEST_FRAME_SAMPLES];
    if (kal_frame_encode(frame, encoded) != KAL_WIRE_OK) return KAL_ERR_INVALID;
    (void)modulate(encoded, pcm);
    *now += 10u;
    return kal_rx_push_s16(link, pcm, TEST_FRAME_SAMPLES, *now);
}

static unsigned int partial_messages(kal_link *link, uint64_t *now,
                                      unsigned int count, unsigned int start) {
    unsigned int index;
    unsigned int accepted = 0u;
    for (index = 0u; index < count; ++index) {
        struct kal_frame frame;
        memset(&frame, 0, sizeof frame);
        frame.type = (uint8_t)KAL_TYPE_DATA;
        frame.session_id = 0x1234u;
        frame.message_id = (uint16_t)(start + index / 89u);
        frame.sequence = (uint16_t)(index % 89u);
        frame.value = 90u; /* Replace each message before its final frame. */
        frame.payload_length = (uint8_t)KAL_PAYLOAD_BYTES;
        memset(frame.payload, (int)(index & 0xffu), sizeof frame.payload);
        if (push_frame(link, &frame, now) == KAL_OK) accepted++;
    }
    return accepted;
}

static void check_partial_sessions_have_bounded_rss(void) {
    kal_options options = options_for((uint8_t)KAL_ROLE_RESPONDER);
    kal_link *link = NULL;
    struct kal_frame hello;
    kal_stats baseline, final;
    uint64_t now = 0u;
    long before, after;
    unsigned int accepted = 0u;
    unsigned int round;

    KAL_CHECK(kal_link_create(&link, &options) == KAL_OK);
    if (link == NULL) return;
    memset(&hello, 0, sizeof hello);
    hello.type = (uint8_t)KAL_TYPE_HELLO;
    hello.session_id = 0x1234u;
    hello.value = kal_capability_pack(options.profile, options.window_frames);
    hello.payload_length = KAL_CHALLENGE_BYTES;
    memset(hello.payload, 0x56, KAL_CHALLENGE_BYTES);
    KAL_CHECK(push_frame(link, &hello, &now) == KAL_OK);
    /* Warm actual receive-buffer clearing and partial-message replacement. */
    KAL_CHECK(partial_messages(link, &now, 4000u, 1u) == 4000u);
    kal_get_stats(link, &baseline);
    KAL_CHECK(baseline.session_open == 1u);
    KAL_CHECK(baseline.frames_rejected == 0u);
    KAL_CHECK(baseline.messages_delivered == 0u);
    (void)resident_kib();
    (void)resident_kib();
    before = resident_kib();
    allocation_calls = deallocation_calls = 0u;
    measuring_allocations = 1;
    for (round = 0u; round < 4u; ++round) {
        accepted += partial_messages(link, &now, 2000u, 1000u + round * 1000u);
    }
    measuring_allocations = 0;
    after = resident_kib();
    kal_get_stats(link, &final);
    KAL_CHECK(accepted == 8000u);
    KAL_CHECK(final.session_open == 1u);
    KAL_CHECK(final.frames_received - baseline.frames_received == 8000u);
    KAL_CHECK(final.frames_rejected == baseline.frames_rejected);
    KAL_CHECK(final.messages_delivered == baseline.messages_delivered);
    KAL_CHECK(allocation_calls == 0u);
    KAL_CHECK(deallocation_calls == 0u);
    check_rss_bound(before, after, "8000 accepted partial DATA frames");
    KAL_GROUP("accepted incomplete-message frames", accepted, 8000u);
    printf("  partial DATA alloc/free calls                 %lu/%lu\n",
           allocation_calls, deallocation_calls);
    kal_link_free(link);
}

static void check_allocation_observer_detects_control(void) {
    /* Volatile pointers keep -O2 from eliding these ordinary allocator calls;
     * the linker must route them through the same hooks as the core. */
    void *(*volatile allocate)(size_t) = malloc;
    void *(*volatile allocate_zeroed)(size_t, size_t) = calloc;
    void *(*volatile resize)(void *, size_t) = realloc;
    void (*volatile release)(void *) = free;
    void *control, *zeroed, *resized;
    allocation_calls = deallocation_calls = 0u;
    measuring_allocations = 1;
    control = allocate(4096u);
    if (control != NULL) memset(control, 0xab, 4096u);
    KAL_CHECK(control != NULL);
    KAL_CHECK(allocation_calls == 1u);
    zeroed = allocate_zeroed(1u, 4096u);
    KAL_CHECK(zeroed != NULL);
    KAL_CHECK(allocation_calls == 2u);
    resized = resize(control, 8192u);
    KAL_CHECK(resized != NULL);
    KAL_CHECK(allocation_calls == 3u);
    release(resized != NULL ? resized : control);
    release(zeroed);
    KAL_CHECK(deallocation_calls == 2u);
    measuring_allocations = 0;
}

static void check_random_pcm_never_delivers(void) {
    kal_options options = options_for((uint8_t)KAL_ROLE_RESPONDER);
    kal_link *link = NULL;
    uint32_t state = 0x12345678u;
    unsigned int block;
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    kal_stats stats;

    KAL_CHECK(kal_link_create(&link, &options) == KAL_OK);
    if (link == NULL) {
        return;
    }
    for (block = 0u; block < 2000u; ++block) {
        int16_t pcm[256];
        size_t index;
        for (index = 0u; index < 256u; ++index) {
            state = state * 1103515245u + 12345u;
            pcm[index] = (int16_t)(state >> 16);
        }
        (void)kal_rx_push_s16(link, pcm, 256u, (uint64_t)block);
    }
    KAL_CHECK(kal_receive(link, received, sizeof received, &size) == KAL_WANT_INPUT);
    kal_get_stats(link, &stats);
    KAL_CHECK(stats.messages_delivered == 0u);
    KAL_GROUP("messages delivered from 512,000 random samples", 0u, 0u);
    kal_link_free(link);
}

static void check_receive_capacity(void) {
    kal_options initiator = options_for((uint8_t)KAL_ROLE_INITIATOR);
    kal_options responder = options_for((uint8_t)KAL_ROLE_RESPONDER);
    kal_link *a = NULL;
    kal_link *b = NULL;
    uint8_t payload[40];
    uint8_t small[8];
    uint8_t big[64];
    size_t size = 0u;
    int16_t pcm[512];
    size_t written = 0u;

    KAL_CHECK(kal_link_create(&a, &initiator) == KAL_OK);
    KAL_CHECK(kal_link_create(&b, &responder) == KAL_OK);
    memset(payload, 0x33, sizeof payload);
    KAL_CHECK(kal_beacon(a, payload, sizeof payload) == KAL_OK);
    KAL_CHECK(kal_tx_pull_s16(a, pcm, sizeof pcm / sizeof pcm[0], &written, 0u)
              == KAL_HAVE_OUTPUT);
    KAL_CHECK(kal_rx_push_s16(b, pcm, written, 0u) == KAL_DELIVERED);
    /* A short buffer is refused without consuming the message. */
    KAL_CHECK(kal_receive(b, small, sizeof small, &size) == KAL_ERR_INVALID);
    KAL_CHECK(kal_receive(b, big, sizeof big, &size) == KAL_OK);
    KAL_CHECK(size == sizeof payload);
    KAL_CHECK(kal_receive(b, big, sizeof big, &size) == KAL_WANT_INPUT);
    kal_link_free(a);
    kal_link_free(b);
}

static void check_null_arguments(void) {
    kal_options options = options_for((uint8_t)KAL_ROLE_INITIATOR);
    kal_link *link = NULL;
    size_t size = 0u;
    uint8_t buffer[8];
    int16_t pcm[8];
    uint64_t deadline = 0u;

    KAL_CHECK(kal_link_create(NULL, &options) == KAL_ERR_INVALID);
    KAL_CHECK(kal_link_create(&link, NULL) == KAL_ERR_INVALID);
    KAL_CHECK(kal_link_create(&link, &options) == KAL_OK);
    KAL_CHECK(kal_send(NULL, buffer, 1u) == KAL_ERR_INVALID);
    KAL_CHECK(kal_send(link, NULL, 1u) == KAL_ERR_INVALID);
    KAL_CHECK(kal_beacon(link, NULL, 1u) == KAL_ERR_INVALID);
    KAL_CHECK(kal_rx_push_s16(link, NULL, 1u, 0u) == KAL_ERR_INVALID);
    KAL_CHECK(kal_tx_pull_s16(link, pcm, 8u, NULL, 0u) == KAL_ERR_INVALID);
    KAL_CHECK(kal_receive(link, NULL, 8u, &size) == KAL_ERR_INVALID);
    KAL_CHECK(kal_next_deadline(NULL, &deadline) == 0);
    KAL_CHECK(kal_cancel(link, 99u) == KAL_ERR_INVALID);
    KAL_CHECK(kal_tx_status(NULL) == KAL_ERR_INVALID);
    kal_get_stats(NULL, NULL);
    kal_link_free(NULL);
    kal_link_reset(NULL);
    kal_link_free(link);
}

int main(void) {
    printf("test_bounds — bounds, hostile input and integrity\n");
    check_option_validation();
    check_message_size_limits();
    check_corrupted_complete_message_is_never_delivered();
    check_hostile_flood_is_bounded();
    check_partial_sessions_have_bounded_rss();
    check_allocation_observer_detects_control();
    check_random_pcm_never_delivers();
    check_receive_capacity();
    check_null_arguments();
    return kal_test_report("test_bounds");
}
