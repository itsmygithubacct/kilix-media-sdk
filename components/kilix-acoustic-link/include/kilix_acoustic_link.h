/*
 * kilix-acoustic-link — bounded reliable short-message transfer over
 * caller-owned PCM.
 *
 * SPDX-License-Identifier: MIT
 *
 * This header is valid C11 and exposes no upstream modem type. The caller
 * owns every buffer at this boundary; the library performs no device access,
 * no subprocess creation, no network access and no callback into application
 * code. One thread owns one kal_link instance.
 *
 * KAL1 wire version 1 is a CANDIDATE. It is not frozen. See
 * docs/KAL1-WIRE.md for the exact frame layout and for the joint F111/F113
 * items that must settle before any freeze.
 */
#ifndef KILIX_ACOUSTIC_LINK_H
#define KILIX_ACOUSTIC_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KAL_ABI_VERSION_MAJOR 0
#define KAL_ABI_VERSION_MINOR 1
#define KAL_ABI_VERSION_PATCH 0

/* Wire constants. Fixed for KAL1 version 1. */
#define KAL_FRAME_BYTES          64u
#define KAL_HEADER_BYTES         16u
#define KAL_PAYLOAD_BYTES        44u
#define KAL_CRC_BYTES             4u
#define KAL_DIGEST_BYTES         32u
#define KAL_MAX_SEGMENTS         94u

/* Policy bounds. */
#define KAL_MAX_MESSAGE_BYTES  4096u
#define KAL_MAX_BEACON_BYTES     44u
#define KAL_MIN_WINDOW_FRAMES     1u
#define KAL_MAX_WINDOW_FRAMES    16u
#define KAL_SAMPLE_RATE       48000u

typedef struct kal_link kal_link;

typedef enum {
    KAL_OK = 0,
    KAL_WANT_INPUT,      /* nothing to emit; more PCM needed        */
    KAL_HAVE_OUTPUT,     /* PCM is queued for transmission          */
    KAL_DELIVERED,       /* a complete message is ready to collect  */
    KAL_ERR_INVALID,
    KAL_ERR_PROTOCOL,
    KAL_ERR_INTEGRITY,
    KAL_ERR_TIMEOUT,
    KAL_ERR_BUSY,
    KAL_ERR_MEMORY,
    KAL_ERR_MODEM,
    KAL_ERR_UNSUPPORTED,
    KAL_ERR_CANCELLED
} kal_result;

/*
 * Physical profiles. Version 1 exposes a Kilix enum, never an upstream
 * numeric id. No profile in this enum is qualified: profile graduation is a
 * physical-measurement result and none has been produced.
 */
typedef enum {
    KAL_PROFILE_AUDIBLE_NORMAL = 0,
    KAL_PROFILE_AUDIBLE_FAST = 1,
    KAL_PROFILE_AUDIBLE_FASTEST = 2,
    KAL_PROFILE_DT_NORMAL = 3,
    KAL_PROFILE_DT_FAST = 4,
    KAL_PROFILE_HIGH_NORMAL = 5,   /* high-frequency, NOT inaudible; opt-in */
    KAL_PROFILE_COUNT = 6
} kal_profile;

typedef enum {
    KAL_ROLE_INITIATOR = 0,
    KAL_ROLE_RESPONDER = 1
} kal_role;

/*
 * Modem backend.
 *
 * KAL_MODEM_GGWAVE is the only backend built into a release library.
 * KAL_MODEM_TEST_PASSTHROUGH exists so the link state machine can be driven
 * deterministically under virtual time without emitting or decoding sound; it
 * is compiled in only when KAL_ENABLE_TEST_MODEM is defined, and is rejected
 * with KAL_ERR_UNSUPPORTED otherwise. It applies no modulation whatsoever and
 * must never be selected by a product consumer.
 */
typedef enum {
    KAL_MODEM_GGWAVE = 0,
    KAL_MODEM_TEST_PASSTHROUGH = 1
} kal_modem_backend;

/* Bounded CANCEL reason codes. No free-form diagnostic string is ever sent. */
typedef enum {
    KAL_CANCEL_UNSPECIFIED = 0,
    KAL_CANCEL_USER = 1,
    KAL_CANCEL_TIMEOUT = 2,
    KAL_CANCEL_POLICY = 3,
    KAL_CANCEL_SHUTDOWN = 4,
    KAL_CANCEL_REASON_MAX = 4
} kal_cancel_reason;

typedef struct {
    uint32_t sample_rate;        /* must be KAL_SAMPLE_RATE in v1          */
    uint32_t max_message_bytes;  /* 0 => KAL_MAX_MESSAGE_BYTES             */
    uint16_t window_frames;      /* 0 => 4; clamped range 1..16            */
    uint8_t  max_retries;        /* 0 => 5                                 */
    uint8_t  profile;            /* kal_profile                            */
    uint8_t  role;               /* kal_role                               */
    uint8_t  modem;              /* kal_modem_backend                      */
    uint8_t  allow_high_frequency; /* required to select HIGH_NORMAL       */
    uint8_t  reserved;           /* must be 0                              */
} kal_options;

typedef struct {
    uint8_t  negotiated_profile;
    uint8_t  session_open;
    uint16_t negotiated_window;
    uint32_t session_id;
    uint32_t messages_sent;
    uint32_t messages_delivered;
    uint32_t frames_sent;
    uint32_t frames_received;
    uint32_t frames_retransmitted;
    uint32_t crc_failures;
    uint32_t frames_rejected;
    uint32_t timeouts;
    uint32_t duplicates_suppressed;
    uint64_t frame_airtime_ms;   /* measured airtime of one KAL1 frame     */
} kal_stats;

/* Lifecycle. */
kal_result kal_link_create(kal_link **out, const kal_options *options);
void       kal_link_free(kal_link *link);
void       kal_link_reset(kal_link *link);

/* Application submission. */
kal_result kal_send(kal_link *link, const uint8_t *message, size_t message_size);
kal_result kal_beacon(kal_link *link, const uint8_t *payload, size_t payload_size);
kal_result kal_cancel(kal_link *link, uint16_t reason);

/* PCM boundary. Both buffers are caller-owned. */
kal_result kal_rx_push_s16(kal_link *link, const int16_t *pcm,
                           size_t sample_count, uint64_t monotonic_ms);
kal_result kal_tx_pull_s16(kal_link *link, int16_t *pcm, size_t capacity,
                           size_t *samples_written, uint64_t monotonic_ms);

/* Advance timers without moving PCM. */
kal_result kal_tick(kal_link *link, uint64_t monotonic_ms);

/*
 * Outbound transfer state:
 *   KAL_OK            no transfer in flight; the last one completed or none ran
 *   KAL_ERR_BUSY      a transfer is in flight
 *   KAL_ERR_TIMEOUT   the last transfer exhausted its retry budget
 *   KAL_ERR_CANCELLED the last transfer was cancelled by either side
 */
kal_result kal_tx_status(const kal_link *link);

/* Collect a delivered message or beacon. */
kal_result kal_receive(kal_link *link, uint8_t *message, size_t capacity,
                       size_t *message_size);

/* 1 = deadline written, 0 = idle. */
int  kal_next_deadline(const kal_link *link, uint64_t *monotonic_ms);
void kal_get_stats(const kal_link *link, kal_stats *stats);

const char *kal_result_string(kal_result result);
const char *kal_profile_string(uint8_t profile);
const char *kal_version_string(void);

/*
 * Frame inspection for the diagnostic tool. Decodes one 64-byte KAL1 frame
 * into a human-readable single line. Never prints payload bytes.
 */
kal_result kal_describe_frame(const uint8_t *frame, size_t frame_size,
                              char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* KILIX_ACOUSTIC_LINK_H */
