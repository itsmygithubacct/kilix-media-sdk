/*
 * kilix-acoustic-link core: session, selective-repeat ARQ, bounded
 * reassembly and the public C ABI.
 *
 * SPDX-License-Identifier: MIT
 *
 * Every buffer this file needs is allocated in kal_link_create. Nothing on
 * the PCM or frame path allocates, so hostile input cannot grow memory.
 */
#include "kilix_acoustic_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kal_frame.h"
#include "kal_modem.h"
#include "kal_random.h"
#include "kal_sha256.h"

#define KAL_OUT_QUEUE_FRAMES 32u
#define KAL_DUP_CACHE_ENTRIES 8u
#define KAL_TRANSFER_MAX_BYTES (KAL_MAX_MESSAGE_BYTES + KAL_DIGEST_BYTES)

/* Timeout shape. Every term is derived from measured frame airtime; no
 * wall-clock constant is hard-coded, because a fixed ceiling is provably too
 * low for the slower profiles. */
#define KAL_TIMEOUT_SLACK_NUM 1u
#define KAL_TIMEOUT_SLACK_DEN 4u
#define KAL_TIMEOUT_CEILING_MULT 8u
#define KAL_DUP_CACHE_TTL_FRAMES 64u

struct kal_dup_entry {
    uint8_t  used;
    uint32_t session_id;
    uint16_t message_id;
    uint64_t expires_ms;
    uint8_t  ack[KAL_FRAME_BYTES];
};

struct kal_link {
    /* configuration, normalised at create */
    uint8_t  role;
    uint8_t  profile;
    uint8_t  modem_backend;
    uint8_t  max_retries;
    uint16_t window;
    uint32_t max_message_bytes;
    uint32_t sample_rate;

    struct kal_modem *modem;
    uint64_t frame_airtime_ms;
    uint64_t base_timeout_ms;
    uint64_t timeout_ceiling_ms;

    /* session */
    uint8_t  session_open;
    uint8_t  hello_pending;
    uint32_t session_id;
    uint8_t  challenge[KAL_CHALLENGE_BYTES];
    uint16_t next_message_id;
    uint8_t  hello_ack_cached[KAL_FRAME_BYTES];
    uint8_t  hello_ack_valid;
    uint8_t  hello_retries;

    /* transmit */
    uint8_t  tx_active;
    uint8_t  tx_started;          /* frames have been queued at least once */
    kal_result tx_status;
    uint16_t tx_message_id;
    uint16_t tx_total;
    uint16_t tx_base;
    uint8_t  tx_retries;
    uint64_t tx_timeout_ms;
    uint64_t deadline_ms;
    uint8_t  deadline_set;
    uint8_t  (*tx_payload)[KAL_PAYLOAD_BYTES];
    uint8_t  *tx_lengths;
    uint8_t  *tx_acked;
    uint8_t  *tx_inflight;

    /* receive/reassembly */
    uint8_t  rx_active;
    uint8_t  rx_complete;
    uint32_t rx_session_id;
    uint16_t rx_message_id;
    uint16_t rx_total;
    uint16_t rx_count;
    uint8_t  *rx_received;
    uint8_t  *rx_lengths;
    uint8_t  (*rx_payload)[KAL_PAYLOAD_BYTES];
    uint8_t  rx_ack_cached[KAL_FRAME_BYTES];
    uint8_t  rx_ack_valid;

    struct kal_dup_entry dup_cache[KAL_DUP_CACHE_ENTRIES];

    /* delivery staging */
    uint8_t  *delivered;
    size_t   delivered_length;
    uint8_t  delivered_ready;
    uint8_t  delivered_is_beacon;

    /* outbound frame queue */
    uint8_t  out_frames[KAL_OUT_QUEUE_FRAMES][KAL_FRAME_BYTES];
    uint8_t  out_head;
    uint8_t  out_count;

    /* PCM currently on air */
    int16_t  *tx_pcm;
    size_t   tx_pcm_capacity;
    size_t   tx_pcm_length;
    size_t   tx_pcm_position;

    /* scratch, allocated once */
    uint8_t  *transfer;

    /* Repeat-detection guard: the pinned engine can report one transmission
     * more than once while its waveform is still being consumed. A frame
     * cannot physically repeat faster than its own airtime, so a
     * byte-identical frame inside that window is an engine artefact, not a
     * peer retransmission. The window is two frame airtimes because a repeat
     * can straddle the next frame's chunks, while a genuine retransmission
     * cannot arrive sooner than the minimum timeout of 2.5 airtimes. */
    uint8_t  last_rx_frame[KAL_FRAME_BYTES];
    uint8_t  last_rx_valid;
    uint64_t last_rx_ms;

    uint64_t now_ms;
    uint8_t  cancel_seen;
    kal_stats stats;
};

/* ---------------------------------------------------------------- helpers */

static uint16_t clamp_window(uint16_t requested) {
    if (requested < KAL_MIN_WINDOW_FRAMES) {
        return KAL_MIN_WINDOW_FRAMES;
    }
    if (requested > KAL_MAX_WINDOW_FRAMES) {
        return KAL_MAX_WINDOW_FRAMES;
    }
    return requested;
}

static void recompute_timeouts(kal_link *link, uint16_t frames_in_flight) {
    const uint64_t air = link->frame_airtime_ms;
    const uint64_t flight = (uint64_t)(frames_in_flight == 0u ? 1u : frames_in_flight);
    uint64_t base = (air * flight) + air; /* data on air plus one ACK */
    base += (base * KAL_TIMEOUT_SLACK_NUM) / KAL_TIMEOUT_SLACK_DEN;
    if (base == 0u) {
        base = 1u;
    }
    link->base_timeout_ms = base;
    link->timeout_ceiling_ms = base * KAL_TIMEOUT_CEILING_MULT;
    link->tx_timeout_ms = base;
}

static void arm_deadline(kal_link *link, uint64_t delay_ms) {
    link->deadline_ms = link->now_ms + delay_ms;
    link->deadline_set = 1u;
}

static void clear_deadline(kal_link *link) {
    link->deadline_set = 0u;
    link->deadline_ms = 0u;
}

static int queue_frame(kal_link *link, const struct kal_frame *frame) {
    uint8_t slot;
    if (link->out_count >= (uint8_t)KAL_OUT_QUEUE_FRAMES) {
        return 0;
    }
    slot = (uint8_t)((link->out_head + link->out_count) % KAL_OUT_QUEUE_FRAMES);
    if (kal_frame_encode(frame, link->out_frames[slot]) != KAL_WIRE_OK) {
        return 0;
    }
    link->out_count++;
    return 1;
}

static int queue_encoded(kal_link *link, const uint8_t frame[KAL_FRAME_BYTES]) {
    uint8_t slot;
    if (link->out_count >= (uint8_t)KAL_OUT_QUEUE_FRAMES) {
        return 0;
    }
    slot = (uint8_t)((link->out_head + link->out_count) % KAL_OUT_QUEUE_FRAMES);
    memcpy(link->out_frames[slot], frame, KAL_FRAME_BYTES);
    link->out_count++;
    return 1;
}

static void clear_out_queue(kal_link *link) {
    link->out_head = 0u;
    link->out_count = 0u;
    memset(link->out_frames, 0, sizeof link->out_frames);
    link->tx_pcm_length = 0u;
    link->tx_pcm_position = 0u;
}

/* --------------------------------------------------------- duplicate cache */

static struct kal_dup_entry *dup_lookup(kal_link *link, uint32_t session_id,
                                        uint16_t message_id) {
    size_t index;
    for (index = 0u; index < KAL_DUP_CACHE_ENTRIES; ++index) {
        struct kal_dup_entry *entry = &link->dup_cache[index];
        if (entry->used == 0u) {
            continue;
        }
        if (entry->expires_ms <= link->now_ms) {
            memset(entry, 0, sizeof *entry);
            continue;
        }
        if (entry->session_id == session_id && entry->message_id == message_id) {
            return entry;
        }
    }
    return NULL;
}

static void dup_store(kal_link *link, uint32_t session_id, uint16_t message_id,
                      const uint8_t ack[KAL_FRAME_BYTES]) {
    size_t index;
    struct kal_dup_entry *victim = NULL;
    uint64_t earliest = UINT64_MAX;

    for (index = 0u; index < KAL_DUP_CACHE_ENTRIES; ++index) {
        struct kal_dup_entry *entry = &link->dup_cache[index];
        if (entry->used == 0u || entry->expires_ms <= link->now_ms) {
            victim = entry;
            break;
        }
        if (entry->session_id == session_id && entry->message_id == message_id) {
            victim = entry;
            break;
        }
        if (entry->expires_ms < earliest) {
            earliest = entry->expires_ms;
            victim = entry;
        }
    }
    if (victim == NULL) {
        return;
    }
    memset(victim, 0, sizeof *victim);
    victim->used = 1u;
    victim->session_id = session_id;
    victim->message_id = message_id;
    victim->expires_ms = link->now_ms
        + (link->frame_airtime_ms * KAL_DUP_CACHE_TTL_FRAMES);
    memcpy(victim->ack, ack, KAL_FRAME_BYTES);
}

/* --------------------------------------------------------------- lifecycle */

static void clear_rx_message(kal_link *link) {
    link->rx_active = 0u;
    link->rx_complete = 0u;
    link->rx_message_id = 0u;
    link->rx_total = 0u;
    link->rx_count = 0u;
    link->rx_ack_valid = 0u;
    memset(link->rx_ack_cached, 0, sizeof link->rx_ack_cached);
    if (link->rx_received != NULL) {
        memset(link->rx_received, 0, KAL_MAX_SEGMENTS);
        memset(link->rx_lengths, 0, KAL_MAX_SEGMENTS);
        memset(link->rx_payload, 0, (size_t)KAL_MAX_SEGMENTS * KAL_PAYLOAD_BYTES);
    }
}

static void clear_tx_message(kal_link *link) {
    link->tx_active = 0u;
    link->tx_started = 0u;
    link->tx_message_id = 0u;
    link->tx_total = 0u;
    link->tx_base = 0u;
    link->tx_retries = 0u;
    if (link->tx_acked != NULL) {
        memset(link->tx_acked, 0, KAL_MAX_SEGMENTS);
        memset(link->tx_inflight, 0, KAL_MAX_SEGMENTS);
        memset(link->tx_lengths, 0, KAL_MAX_SEGMENTS);
        memset(link->tx_payload, 0, (size_t)KAL_MAX_SEGMENTS * KAL_PAYLOAD_BYTES);
    }
}

kal_result kal_link_create(kal_link **out, const kal_options *options) {
    kal_link *link;
    kal_result result;
    kal_options normalised;

    if (out == NULL || options == NULL) {
        return KAL_ERR_INVALID;
    }
    *out = NULL;
    normalised = *options;
    if (normalised.reserved != 0u) {
        return KAL_ERR_INVALID;
    }
    if (normalised.sample_rate == 0u) {
        normalised.sample_rate = KAL_SAMPLE_RATE;
    }
    if (normalised.sample_rate != KAL_SAMPLE_RATE) {
        return KAL_ERR_UNSUPPORTED;
    }
    if (normalised.max_message_bytes == 0u) {
        normalised.max_message_bytes = KAL_MAX_MESSAGE_BYTES;
    }
    if (normalised.max_message_bytes > KAL_MAX_MESSAGE_BYTES) {
        return KAL_ERR_INVALID;
    }
    if (normalised.window_frames == 0u) {
        normalised.window_frames = 4u;
    }
    if (normalised.window_frames < KAL_MIN_WINDOW_FRAMES
            || normalised.window_frames > KAL_MAX_WINDOW_FRAMES) {
        return KAL_ERR_INVALID;
    }
    if (normalised.max_retries == 0u) {
        normalised.max_retries = 5u;
    }
    if (normalised.profile >= (uint8_t)KAL_PROFILE_COUNT) {
        return KAL_ERR_INVALID;
    }
    if (normalised.profile == (uint8_t)KAL_PROFILE_HIGH_NORMAL
            && normalised.allow_high_frequency == 0u) {
        /* High-frequency transmission is off unless the consumer opts in:
         * hardware response varies and some people and animals hear it. */
        return KAL_ERR_UNSUPPORTED;
    }
    if (normalised.role != (uint8_t)KAL_ROLE_INITIATOR
            && normalised.role != (uint8_t)KAL_ROLE_RESPONDER) {
        return KAL_ERR_INVALID;
    }

    link = (kal_link *)calloc(1u, sizeof *link);
    if (link == NULL) {
        return KAL_ERR_MEMORY;
    }
    link->role = normalised.role;
    link->profile = normalised.profile;
    link->modem_backend = normalised.modem;
    link->max_retries = normalised.max_retries;
    link->window = clamp_window(normalised.window_frames);
    link->max_message_bytes = normalised.max_message_bytes;
    link->sample_rate = normalised.sample_rate;
    link->tx_status = KAL_OK;

    kal_modem_global_init();
    result = kal_modem_create(&link->modem, normalised.modem,
                              normalised.profile, normalised.sample_rate);
    if (result != KAL_OK) {
        kal_link_free(link);
        return result;
    }
    link->frame_airtime_ms = kal_modem_frame_airtime_ms(link->modem);
    if (link->frame_airtime_ms == 0u) {
        link->frame_airtime_ms = 1u;
    }
    recompute_timeouts(link, link->window);

    link->tx_pcm_capacity = kal_modem_frame_samples(link->modem);
    link->tx_pcm = (int16_t *)calloc(link->tx_pcm_capacity, sizeof(int16_t));
    link->tx_payload = calloc(KAL_MAX_SEGMENTS, KAL_PAYLOAD_BYTES);
    link->tx_lengths = (uint8_t *)calloc(KAL_MAX_SEGMENTS, 1u);
    link->tx_acked = (uint8_t *)calloc(KAL_MAX_SEGMENTS, 1u);
    link->tx_inflight = (uint8_t *)calloc(KAL_MAX_SEGMENTS, 1u);
    link->rx_payload = calloc(KAL_MAX_SEGMENTS, KAL_PAYLOAD_BYTES);
    link->rx_lengths = (uint8_t *)calloc(KAL_MAX_SEGMENTS, 1u);
    link->rx_received = (uint8_t *)calloc(KAL_MAX_SEGMENTS, 1u);
    link->delivered = (uint8_t *)calloc(KAL_MAX_MESSAGE_BYTES, 1u);
    link->transfer = (uint8_t *)calloc(KAL_TRANSFER_MAX_BYTES, 1u);
    if (link->tx_pcm == NULL || link->tx_payload == NULL
            || link->tx_lengths == NULL || link->tx_acked == NULL
            || link->tx_inflight == NULL
            || link->rx_payload == NULL || link->rx_lengths == NULL
            || link->rx_received == NULL || link->delivered == NULL
            || link->transfer == NULL) {
        kal_link_free(link);
        return KAL_ERR_MEMORY;
    }

    /* Initiator message ids are even, responder ids odd: one persistent
     * session carries both directions without an id collision. */
    link->next_message_id =
        (uint16_t)((link->role == (uint8_t)KAL_ROLE_INITIATOR) ? 0u : 1u);
    link->stats.negotiated_profile = link->profile;
    link->stats.negotiated_window = link->window;
    link->stats.frame_airtime_ms = link->frame_airtime_ms;
    *out = link;
    return KAL_OK;
}

void kal_link_free(kal_link *link) {
    if (link == NULL) {
        return;
    }
    /* Message material is wiped, not merely released. */
    if (link->delivered != NULL) {
        memset(link->delivered, 0, KAL_MAX_MESSAGE_BYTES);
    }
    if (link->transfer != NULL) {
        memset(link->transfer, 0, KAL_TRANSFER_MAX_BYTES);
    }
    if (link->rx_payload != NULL) {
        memset(link->rx_payload, 0, (size_t)KAL_MAX_SEGMENTS * KAL_PAYLOAD_BYTES);
    }
    if (link->tx_payload != NULL) {
        memset(link->tx_payload, 0, (size_t)KAL_MAX_SEGMENTS * KAL_PAYLOAD_BYTES);
    }
    kal_modem_free(link->modem);
    free(link->tx_pcm);
    free(link->tx_payload);
    free(link->tx_lengths);
    free(link->tx_acked);
    free(link->tx_inflight);
    free(link->rx_payload);
    free(link->rx_lengths);
    free(link->rx_received);
    free(link->delivered);
    free(link->transfer);
    free(link);
}

void kal_link_reset(kal_link *link) {
    if (link == NULL) {
        return;
    }
    clear_tx_message(link);
    clear_rx_message(link);
    clear_out_queue(link);
    clear_deadline(link);
    memset(link->dup_cache, 0, sizeof link->dup_cache);
    memset(link->challenge, 0, sizeof link->challenge);
    memset(link->hello_ack_cached, 0, sizeof link->hello_ack_cached);
    memset(link->last_rx_frame, 0, sizeof link->last_rx_frame);
    link->last_rx_valid = 0u;
    link->last_rx_ms = 0u;
    memset(link->delivered, 0, KAL_MAX_MESSAGE_BYTES);
    link->hello_ack_valid = 0u;
    link->hello_pending = 0u;
    link->hello_retries = 0u;
    link->session_open = 0u;
    link->session_id = 0u;
    link->delivered_ready = 0u;
    link->delivered_length = 0u;
    link->delivered_is_beacon = 0u;
    link->cancel_seen = 0u;
    link->tx_status = KAL_OK;
    link->next_message_id =
        (uint16_t)((link->role == (uint8_t)KAL_ROLE_INITIATOR) ? 0u : 1u);
    kal_modem_rx_reset(link->modem);
    recompute_timeouts(link, link->window);
    link->stats.session_open = 0u;
    link->stats.session_id = 0u;
}

/* ----------------------------------------------------------- session open */

static void build_hello(kal_link *link, struct kal_frame *frame, uint8_t type) {
    memset(frame, 0, sizeof *frame);
    frame->type = type;
    frame->session_id = link->session_id;
    frame->message_id = link->next_message_id;
    frame->sequence = 0u;
    frame->value = kal_capability_pack(link->profile, link->window);
    frame->payload_length = KAL_CHALLENGE_BYTES;
    memcpy(frame->payload, link->challenge, KAL_CHALLENGE_BYTES);
}

static kal_result open_session(kal_link *link) {
    struct kal_frame hello;
    uint32_t session_id = 0u;

    if (link->role != (uint8_t)KAL_ROLE_INITIATOR) {
        return KAL_ERR_PROTOCOL;
    }
    if (kal_random_bytes(&session_id, sizeof session_id) != 0
            || kal_random_bytes(link->challenge, sizeof link->challenge) != 0) {
        /* A CSPRNG failure is fatal. It is never replaced with a timestamp. */
        return KAL_ERR_MODEM;
    }
    if (session_id == 0u) {
        session_id = 1u;
    }
    link->session_id = session_id;
    build_hello(link, &hello, (uint8_t)KAL_TYPE_HELLO);
    if (!queue_frame(link, &hello)) {
        return KAL_ERR_BUSY;
    }
    link->hello_pending = 1u;
    link->hello_retries = 0u;
    recompute_timeouts(link, 1u);
    arm_deadline(link, link->tx_timeout_ms);
    return KAL_OK;
}

/* ------------------------------------------------------------- transmit */

/* Queue every unacknowledged frame inside the window. */
static uint16_t queue_window(kal_link *link, int retransmission) {
    struct kal_frame frame;
    uint16_t queued = 0u;
    uint16_t sequence;

    for (sequence = link->tx_base;
         sequence < link->tx_total && queued < link->window;
         ++sequence) {
        if (link->tx_acked[sequence] != 0u || link->tx_inflight[sequence] != 0u) {
            continue;
        }
        memset(&frame, 0, sizeof frame);
        frame.type = (uint8_t)KAL_TYPE_DATA;
        frame.session_id = link->session_id;
        frame.message_id = link->tx_message_id;
        frame.sequence = sequence;
        frame.value = link->tx_total;
        frame.payload_length = link->tx_lengths[sequence];
        frame.flags = (sequence == (uint16_t)(link->tx_total - 1u))
            ? KAL_FLAG_DATA_FINAL : 0u;
        memcpy(frame.payload, link->tx_payload[sequence], frame.payload_length);
        if (!queue_frame(link, &frame)) {
            break;
        }
        link->tx_inflight[sequence] = 1u;
        queued++;
        link->stats.frames_sent++;
        if (retransmission) {
            link->stats.frames_retransmitted++;
        }
    }
    return queued;
}

static void start_or_resume_tx(kal_link *link, int retransmission) {
    const uint16_t queued = queue_window(link, retransmission);
    if (queued == 0u) {
        return;
    }
    link->tx_started = 1u;
    if (!retransmission) {
        recompute_timeouts(link, queued);
    }
    arm_deadline(link, link->tx_timeout_ms);
}

static void fail_tx(kal_link *link, kal_result status, uint16_t cancel_reason) {
    struct kal_frame cancel;
    memset(&cancel, 0, sizeof cancel);
    cancel.type = (uint8_t)KAL_TYPE_CANCEL;
    cancel.session_id = link->session_id;
    cancel.message_id = link->tx_message_id;
    cancel.value = cancel_reason;
    (void)queue_frame(link, &cancel);
    clear_tx_message(link);
    link->tx_status = status;
    clear_deadline(link);
}

kal_result kal_send(kal_link *link, const uint8_t *message, size_t message_size) {
    uint16_t segments = 0u;
    size_t transfer_length;
    size_t offset;
    uint16_t sequence;

    if (link == NULL || (message == NULL && message_size != 0u)) {
        return KAL_ERR_INVALID;
    }
    if (message_size > (size_t)link->max_message_bytes) {
        return KAL_ERR_INVALID;
    }
    if (link->tx_active != 0u) {
        return KAL_ERR_BUSY;
    }
    if (kal_segment_count(message_size, &segments) != KAL_WIRE_OK) {
        return KAL_ERR_INVALID;
    }

    /* transfer = message || SHA-256(message). Segmenting the digest with the
     * message is what removes the separate END frame from KAL1. */
    if (message_size != 0u) {
        memcpy(link->transfer, message, message_size);
    }
    if (kal_sha256(message, message_size, link->transfer + message_size) != 0) {
        return KAL_ERR_INTEGRITY;
    }
    transfer_length = message_size + KAL_DIGEST_BYTES;

    clear_tx_message(link);
    offset = 0u;
    for (sequence = 0u; sequence < segments; ++sequence) {
        size_t chunk = transfer_length - offset;
        if (chunk > KAL_PAYLOAD_BYTES) {
            chunk = KAL_PAYLOAD_BYTES;
        }
        memcpy(link->tx_payload[sequence], link->transfer + offset, chunk);
        link->tx_lengths[sequence] = (uint8_t)chunk;
        offset += chunk;
    }
    link->tx_total = segments;
    link->tx_base = 0u;
    link->tx_retries = 0u;
    link->tx_active = 1u;
    link->tx_status = KAL_ERR_BUSY;
    link->tx_message_id = link->next_message_id;

    if (link->session_open == 0u) {
        if (link->role == (uint8_t)KAL_ROLE_INITIATOR) {
            if (link->hello_pending == 0u) {
                const kal_result opened = open_session(link);
                if (opened != KAL_OK) {
                    clear_tx_message(link);
                    link->tx_status = opened;
                    return opened;
                }
            }
        }
        /* A responder waits for the initiator's HELLO; the queued message
         * leaves as soon as the session opens. */
        return KAL_OK;
    }
    start_or_resume_tx(link, 0);
    return KAL_OK;
}

kal_result kal_beacon(kal_link *link, const uint8_t *payload, size_t payload_size) {
    struct kal_frame frame;

    if (link == NULL || payload == NULL) {
        return KAL_ERR_INVALID;
    }
    if (payload_size == 0u || payload_size > KAL_MAX_BEACON_BYTES) {
        return KAL_ERR_INVALID;
    }
    memset(&frame, 0, sizeof frame);
    frame.type = (uint8_t)KAL_TYPE_BEACON;
    frame.session_id = link->session_id;
    frame.message_id = link->next_message_id;
    frame.payload_length = (uint8_t)payload_size;
    memcpy(frame.payload, payload, payload_size);
    if (!queue_frame(link, &frame)) {
        return KAL_ERR_BUSY;
    }
    link->stats.frames_sent++;
    return KAL_OK;
}

kal_result kal_cancel(kal_link *link, uint16_t reason) {
    if (link == NULL) {
        return KAL_ERR_INVALID;
    }
    if (reason > (uint16_t)KAL_CANCEL_REASON_MAX) {
        return KAL_ERR_INVALID;
    }
    fail_tx(link, KAL_ERR_CANCELLED, reason);
    clear_rx_message(link);
    return KAL_OK;
}

kal_result kal_tx_status(const kal_link *link) {
    if (link == NULL) {
        return KAL_ERR_INVALID;
    }
    return link->tx_status;
}

/* -------------------------------------------------------------- receive */

static uint16_t first_missing(const kal_link *link) {
    uint16_t sequence;
    for (sequence = 0u; sequence < link->rx_total; ++sequence) {
        if (link->rx_received[sequence] == 0u) {
            return sequence;
        }
    }
    return link->rx_total;
}

/*
 * Partial ACK: sequence is the cumulative base (the first missing segment)
 * and value is a selective bitmap in which bit i means segment base+1+i has
 * been stored.
 */
static void queue_partial_ack(kal_link *link) {
    struct kal_frame ack;
    const uint16_t base = first_missing(link);
    uint16_t bitmap = 0u;
    unsigned int bit;

    for (bit = 0u; bit < 16u; ++bit) {
        const uint32_t sequence = (uint32_t)base + 1u + bit;
        if (sequence >= (uint32_t)link->rx_total) {
            break;
        }
        if (link->rx_received[sequence] != 0u) {
            bitmap = (uint16_t)(bitmap | (uint16_t)(1u << bit));
        }
    }
    memset(&ack, 0, sizeof ack);
    ack.type = (uint8_t)KAL_TYPE_ACK;
    ack.session_id = link->rx_session_id;
    ack.message_id = link->rx_message_id;
    ack.sequence = base;
    ack.value = bitmap;
    if (kal_frame_encode(&ack, link->rx_ack_cached) != KAL_WIRE_OK) {
        return;
    }
    link->rx_ack_valid = 1u;
    (void)queue_encoded(link, link->rx_ack_cached);
    link->stats.frames_sent++;
}

static kal_result finish_rx_message(kal_link *link) {
    struct kal_frame ack;
    uint8_t observed[KAL_DIGEST_BYTES];
    size_t transfer_length = 0u;
    size_t application_length;
    uint16_t expected_total = 0u;
    uint16_t sequence;

    for (sequence = 0u; sequence < link->rx_total; ++sequence) {
        const size_t length = link->rx_lengths[sequence];
        if (link->rx_received[sequence] == 0u
                || transfer_length + length > KAL_TRANSFER_MAX_BYTES) {
            clear_rx_message(link);
            return KAL_ERR_INTEGRITY;
        }
        memcpy(link->transfer + transfer_length, link->rx_payload[sequence], length);
        transfer_length += length;
    }
    if (transfer_length < KAL_DIGEST_BYTES) {
        clear_rx_message(link);
        return KAL_ERR_INTEGRITY;
    }
    application_length = transfer_length - KAL_DIGEST_BYTES;
    if (application_length > (size_t)link->max_message_bytes
            || kal_segment_count(application_length, &expected_total) != KAL_WIRE_OK
            || expected_total != link->rx_total) {
        clear_rx_message(link);
        return KAL_ERR_INTEGRITY;
    }
    if (kal_sha256(link->transfer, application_length, observed) != 0
            || !kal_digest_equal(observed, link->transfer + application_length)) {
        /* A corrupted complete message is destroyed, never delivered. */
        memset(link->transfer, 0, KAL_TRANSFER_MAX_BYTES);
        clear_rx_message(link);
        link->stats.frames_rejected++;
        return KAL_ERR_INTEGRITY;
    }

    memcpy(link->delivered, link->transfer, application_length);
    link->delivered_length = application_length;
    link->delivered_ready = 1u;
    link->delivered_is_beacon = 0u;
    memset(link->transfer, 0, KAL_TRANSFER_MAX_BYTES);

    memset(&ack, 0, sizeof ack);
    ack.type = (uint8_t)KAL_TYPE_ACK;
    ack.session_id = link->rx_session_id;
    ack.message_id = link->rx_message_id;
    ack.sequence = link->rx_total;
    ack.value = 0u;
    ack.flags = KAL_FLAG_ACK_DELIVERED;
    if (kal_frame_encode(&ack, link->rx_ack_cached) != KAL_WIRE_OK) {
        clear_rx_message(link);
        return KAL_ERR_INTEGRITY;
    }
    link->rx_ack_valid = 1u;
    link->rx_complete = 1u;
    (void)queue_encoded(link, link->rx_ack_cached);
    link->stats.frames_sent++;
    link->stats.messages_delivered++;
    dup_store(link, link->rx_session_id, link->rx_message_id, link->rx_ack_cached);
    clear_rx_message(link);
    return KAL_DELIVERED;
}

static kal_result handle_data(kal_link *link, const struct kal_frame *frame) {
    struct kal_dup_entry *cached;
    const uint16_t sequence = frame->sequence;

    if (link->session_open == 0u || frame->session_id != link->session_id) {
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    cached = dup_lookup(link, frame->session_id, frame->message_id);
    if (cached != NULL) {
        /* Already delivered once: replay the folded FIN and deliver nothing. */
        (void)queue_encoded(link, cached->ack);
        link->stats.duplicates_suppressed++;
        link->stats.frames_sent++;
        return KAL_OK;
    }
    if (link->rx_active != 0u && frame->message_id != link->rx_message_id) {
        /* A new message on a persistent session replaces the abandoned one. */
        clear_rx_message(link);
    }
    if (link->rx_active == 0u) {
        if (link->delivered_ready != 0u) {
            /* Back-pressure: refuse to start a second message while the
             * first is still uncollected. The peer retransmits. */
            return KAL_OK;
        }
        link->rx_active = 1u;
        link->rx_session_id = frame->session_id;
        link->rx_message_id = frame->message_id;
        link->rx_total = frame->value;
    } else if (link->rx_total != frame->value) {
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    if (sequence >= link->rx_total) {
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    if (link->rx_received[sequence] != 0u) {
        if (link->rx_lengths[sequence] != frame->payload_length
                || memcmp(link->rx_payload[sequence], frame->payload,
                          frame->payload_length) != 0) {
            /* Two different bodies for one sequence: keep the first. */
            link->stats.frames_rejected++;
            return KAL_ERR_PROTOCOL;
        }
        link->stats.duplicates_suppressed++;
        if (link->rx_ack_valid != 0u) {
            (void)queue_encoded(link, link->rx_ack_cached);
            link->stats.frames_sent++;
        } else {
            queue_partial_ack(link);
        }
        return KAL_OK;
    }

    link->rx_received[sequence] = 1u;
    link->rx_lengths[sequence] = frame->payload_length;
    memcpy(link->rx_payload[sequence], frame->payload, frame->payload_length);
    link->rx_count++;

    if (link->rx_count == link->rx_total) {
        return finish_rx_message(link);
    }
    if ((link->rx_count % link->window) == 0u) {
        queue_partial_ack(link);
    }
    return KAL_OK;
}

static kal_result handle_ack(kal_link *link, const struct kal_frame *frame) {
    uint16_t sequence;
    unsigned int bit;

    if (link->tx_active == 0u || frame->session_id != link->session_id
            || frame->message_id != link->tx_message_id) {
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    if ((frame->flags & KAL_FLAG_ACK_DELIVERED) != 0u) {
        if (frame->sequence != link->tx_total) {
            link->stats.frames_rejected++;
            return KAL_ERR_PROTOCOL;
        }
        clear_tx_message(link);
        clear_deadline(link);
        link->tx_status = KAL_OK;
        link->stats.messages_sent++;
        link->next_message_id = (uint16_t)(link->next_message_id + 2u);
        return KAL_OK;
    }
    if (frame->sequence > link->tx_total) {
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    for (sequence = 0u; sequence < frame->sequence; ++sequence) {
        link->tx_acked[sequence] = 1u;
        link->tx_inflight[sequence] = 0u;
    }
    for (bit = 0u; bit < 16u; ++bit) {
        const uint32_t marked = (uint32_t)frame->sequence + 1u + bit;
        if (marked >= (uint32_t)link->tx_total) {
            break;
        }
        if ((frame->value & (uint16_t)(1u << bit)) != 0u) {
            link->tx_acked[marked] = 1u;
            link->tx_inflight[marked] = 0u;
        }
    }
    while (link->tx_base < link->tx_total && link->tx_acked[link->tx_base] != 0u) {
        link->tx_base++;
    }
    link->tx_retries = 0u;
    recompute_timeouts(link, link->window);
    start_or_resume_tx(link, 0);
    if (link->tx_active != 0u && link->deadline_set == 0u) {
        arm_deadline(link, link->tx_timeout_ms);
    }
    return KAL_OK;
}

static kal_result handle_hello(kal_link *link, const struct kal_frame *frame) {
    struct kal_frame reply;
    const uint8_t peer_profile = kal_capability_profile(frame->value);
    const uint16_t peer_window = kal_capability_window(frame->value);

    if (peer_profile != link->profile) {
        /* Version 1 never switches profile in air: this is a configuration
         * error surfaced as a diagnostic, not a negotiation. */
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    if (link->role == (uint8_t)KAL_ROLE_INITIATOR) {
        if (link->hello_pending != 0u) {
            /* Simultaneous initiation: the larger session id yields. */
            if (link->session_id <= frame->session_id) {
                return KAL_OK;
            }
            link->hello_pending = 0u;
            clear_tx_message(link);
        } else if (link->session_open != 0u
                   && frame->session_id == link->session_id) {
            if (link->hello_ack_valid != 0u) {
                (void)queue_encoded(link, link->hello_ack_cached);
                link->stats.frames_sent++;
            }
            return KAL_OK;
        } else if (link->session_open != 0u) {
            link->stats.frames_rejected++;
            return KAL_ERR_PROTOCOL;
        }
    } else if (link->session_open != 0u && frame->session_id == link->session_id) {
        if (link->hello_ack_valid != 0u) {
            (void)queue_encoded(link, link->hello_ack_cached);
            link->stats.frames_sent++;
        }
        return KAL_OK;
    } else if (link->session_open != 0u) {
        /* A different session replaces the old one only after its state is
         * discarded, so no half-open session survives. */
        clear_rx_message(link);
        memset(link->dup_cache, 0, sizeof link->dup_cache);
    }

    link->session_id = frame->session_id;
    link->window = clamp_window(peer_window < link->window ? peer_window : link->window);
    memcpy(link->challenge, frame->payload, KAL_CHALLENGE_BYTES);

    memset(&reply, 0, sizeof reply);
    reply.type = (uint8_t)KAL_TYPE_HELLO_ACK;
    reply.session_id = link->session_id;
    reply.message_id = frame->message_id;
    reply.value = kal_capability_pack(link->profile, link->window);
    reply.payload_length = KAL_CHALLENGE_BYTES;
    memcpy(reply.payload, link->challenge, KAL_CHALLENGE_BYTES);
    if (kal_frame_encode(&reply, link->hello_ack_cached) != KAL_WIRE_OK) {
        return KAL_ERR_PROTOCOL;
    }
    link->hello_ack_valid = 1u;
    (void)queue_encoded(link, link->hello_ack_cached);
    link->stats.frames_sent++;

    link->session_open = 1u;
    link->stats.session_open = 1u;
    link->stats.session_id = link->session_id;
    link->stats.negotiated_window = link->window;
    recompute_timeouts(link, link->window);
    if (link->tx_active != 0u && link->tx_started == 0u) {
        start_or_resume_tx(link, 0);
    }
    return KAL_OK;
}

static kal_result handle_hello_ack(kal_link *link, const struct kal_frame *frame) {
    const uint8_t peer_profile = kal_capability_profile(frame->value);
    const uint16_t peer_window = kal_capability_window(frame->value);

    if (link->hello_pending == 0u || frame->session_id != link->session_id) {
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    if (memcmp(frame->payload, link->challenge, KAL_CHALLENGE_BYTES) != 0) {
        /* The echoed challenge is a liveness check on this exchange only. It
         * is not authentication and the module never claims otherwise. */
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    if (peer_profile != link->profile) {
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    link->hello_pending = 0u;
    link->session_open = 1u;
    link->window = clamp_window(peer_window < link->window ? peer_window : link->window);
    link->stats.session_open = 1u;
    link->stats.session_id = link->session_id;
    link->stats.negotiated_window = link->window;
    recompute_timeouts(link, link->window);
    clear_deadline(link);
    if (link->tx_active != 0u) {
        start_or_resume_tx(link, 0);
    }
    return KAL_OK;
}

static kal_result handle_cancel(kal_link *link, const struct kal_frame *frame) {
    if (link->session_open == 0u || frame->session_id != link->session_id) {
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    clear_rx_message(link);
    clear_tx_message(link);
    clear_deadline(link);
    link->tx_status = KAL_ERR_CANCELLED;
    link->cancel_seen = 1u;
    return KAL_ERR_CANCELLED;
}

static kal_result handle_beacon(kal_link *link, const struct kal_frame *frame) {
    if (link->delivered_ready != 0u) {
        return KAL_OK;
    }
    if (dup_lookup(link, frame->session_id, frame->message_id) != NULL) {
        link->stats.duplicates_suppressed++;
        return KAL_OK;
    }
    memcpy(link->delivered, frame->payload, frame->payload_length);
    link->delivered_length = frame->payload_length;
    link->delivered_ready = 1u;
    link->delivered_is_beacon = 1u;
    link->stats.messages_delivered++;
    {
        uint8_t empty[KAL_FRAME_BYTES];
        memset(empty, 0, sizeof empty);
        dup_store(link, frame->session_id, frame->message_id, empty);
    }
    return KAL_DELIVERED;
}

static kal_result handle_frame(kal_link *link, const uint8_t *raw) {
    struct kal_frame frame;
    const enum kal_wire_status status = kal_frame_decode(raw, KAL_FRAME_BYTES, &frame);

    if (status != KAL_WIRE_OK) {
        if (status == KAL_WIRE_ERR_CRC) {
            link->stats.crc_failures++;
        }
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
    link->stats.frames_received++;
    switch (frame.type) {
    case KAL_TYPE_HELLO: return handle_hello(link, &frame);
    case KAL_TYPE_HELLO_ACK: return handle_hello_ack(link, &frame);
    case KAL_TYPE_DATA: return handle_data(link, &frame);
    case KAL_TYPE_ACK: return handle_ack(link, &frame);
    case KAL_TYPE_CANCEL: return handle_cancel(link, &frame);
    case KAL_TYPE_BEACON: return handle_beacon(link, &frame);
    default:
        link->stats.frames_rejected++;
        return KAL_ERR_PROTOCOL;
    }
}

/* ---------------------------------------------------------------- timers */

static void advance_time(kal_link *link, uint64_t monotonic_ms) {
    if (monotonic_ms > link->now_ms) {
        link->now_ms = monotonic_ms;
    }
    if (link->deadline_set == 0u || link->now_ms < link->deadline_ms) {
        return;
    }
    clear_deadline(link);
    link->stats.timeouts++;

    if (link->hello_pending != 0u) {
        struct kal_frame hello;
        if (link->hello_retries >= link->max_retries) {
            link->hello_pending = 0u;
            clear_tx_message(link);
            link->tx_status = KAL_ERR_TIMEOUT;
            return;
        }
        link->hello_retries++;
        build_hello(link, &hello, (uint8_t)KAL_TYPE_HELLO);
        (void)queue_frame(link, &hello);
        link->stats.frames_retransmitted++;
        link->tx_timeout_ms = link->tx_timeout_ms * 2u;
        if (link->tx_timeout_ms > link->timeout_ceiling_ms) {
            link->tx_timeout_ms = link->timeout_ceiling_ms;
        }
        arm_deadline(link, link->tx_timeout_ms);
        return;
    }
    if (link->tx_active != 0u && link->tx_started != 0u) {
        if (link->tx_retries >= link->max_retries) {
            fail_tx(link, KAL_ERR_TIMEOUT, (uint16_t)KAL_CANCEL_TIMEOUT);
            return;
        }
        link->tx_retries++;
        link->tx_timeout_ms = link->tx_timeout_ms * 2u;
        if (link->tx_timeout_ms > link->timeout_ceiling_ms) {
            link->tx_timeout_ms = link->timeout_ceiling_ms;
        }
        /* Nothing outstanding is on air any more: the whole unacknowledged
         * window becomes eligible for retransmission again. */
        memset(link->tx_inflight, 0, KAL_MAX_SEGMENTS);
        start_or_resume_tx(link, 1);
    }
}

kal_result kal_tick(kal_link *link, uint64_t monotonic_ms) {
    if (link == NULL) {
        return KAL_ERR_INVALID;
    }
    advance_time(link, monotonic_ms);
    if (link->delivered_ready != 0u) {
        return KAL_DELIVERED;
    }
    return link->out_count > 0u || link->tx_pcm_position < link->tx_pcm_length
        ? KAL_HAVE_OUTPUT : KAL_WANT_INPUT;
}

/* ------------------------------------------------------------ PCM inbound */

kal_result kal_rx_push_s16(kal_link *link, const int16_t *pcm,
                           size_t sample_count, uint64_t monotonic_ms) {
    size_t offset = 0u;
    kal_result outcome = KAL_OK;

    if (link == NULL || (pcm == NULL && sample_count != 0u)) {
        return KAL_ERR_INVALID;
    }
    advance_time(link, monotonic_ms);
    while (offset < sample_count) {
        uint8_t raw[KAL_FRAME_BYTES];
        size_t consumed = 0u;
        const int got = kal_modem_rx_feed(link->modem, pcm + offset,
                                          sample_count - offset, &consumed, raw);
        if (got < 0) {
            return KAL_ERR_MODEM;
        }
        if (consumed == 0u) {
            break;
        }
        offset += consumed;
        if (got == 1) {
            kal_result handled;
            if (link->last_rx_valid != 0u
                    && (link->now_ms - link->last_rx_ms) < (link->frame_airtime_ms * 2u)
                    && memcmp(raw, link->last_rx_frame, KAL_FRAME_BYTES) == 0) {
                link->stats.duplicates_suppressed++;
                continue;
            }
            memcpy(link->last_rx_frame, raw, KAL_FRAME_BYTES);
            link->last_rx_valid = 1u;
            link->last_rx_ms = link->now_ms;
            handled = handle_frame(link, raw);
            if (handled == KAL_DELIVERED || handled == KAL_ERR_CANCELLED) {
                outcome = handled;
            }
        }
    }
    if (outcome == KAL_OK && link->delivered_ready != 0u) {
        outcome = KAL_DELIVERED;
    }
    return outcome;
}

/* ----------------------------------------------------------- PCM outbound */

kal_result kal_tx_pull_s16(kal_link *link, int16_t *pcm, size_t capacity,
                           size_t *samples_written, uint64_t monotonic_ms) {
    size_t remaining;
    size_t take;

    if (link == NULL || pcm == NULL || samples_written == NULL) {
        return KAL_ERR_INVALID;
    }
    *samples_written = 0u;
    advance_time(link, monotonic_ms);
    if (capacity == 0u) {
        return KAL_WANT_INPUT;
    }

    if (link->tx_pcm_position >= link->tx_pcm_length) {
        kal_result encoded;
        size_t produced = 0u;
        if (link->out_count == 0u) {
            return KAL_WANT_INPUT;
        }
        /* Listen before transmitting. Carrier sense is best effort on an
         * acoustic channel; the collision rule, not sensing, is what makes
         * simultaneous initiation correct. */
        if (kal_modem_rx_busy(link->modem)) {
            return KAL_WANT_INPUT;
        }
        encoded = kal_modem_encode(link->modem, link->out_frames[link->out_head],
                                   link->tx_pcm, link->tx_pcm_capacity, &produced);
        if (encoded != KAL_OK) {
            return encoded;
        }
        link->out_head = (uint8_t)((link->out_head + 1u) % KAL_OUT_QUEUE_FRAMES);
        link->out_count--;
        link->tx_pcm_length = produced;
        link->tx_pcm_position = 0u;
    }

    remaining = link->tx_pcm_length - link->tx_pcm_position;
    take = remaining < capacity ? remaining : capacity;
    memcpy(pcm, link->tx_pcm + link->tx_pcm_position, take * sizeof(int16_t));
    link->tx_pcm_position += take;
    *samples_written = take;
    return KAL_HAVE_OUTPUT;
}

/* ---------------------------------------------------------------- collect */

kal_result kal_receive(kal_link *link, uint8_t *message, size_t capacity,
                       size_t *message_size) {
    if (link == NULL || message == NULL || message_size == NULL) {
        return KAL_ERR_INVALID;
    }
    *message_size = 0u;
    if (link->delivered_ready == 0u) {
        return KAL_WANT_INPUT;
    }
    if (capacity < link->delivered_length) {
        return KAL_ERR_INVALID;
    }
    memcpy(message, link->delivered, link->delivered_length);
    *message_size = link->delivered_length;
    memset(link->delivered, 0, KAL_MAX_MESSAGE_BYTES);
    link->delivered_length = 0u;
    link->delivered_ready = 0u;
    link->delivered_is_beacon = 0u;
    return KAL_OK;
}

int kal_next_deadline(const kal_link *link, uint64_t *monotonic_ms) {
    if (link == NULL || monotonic_ms == NULL || link->deadline_set == 0u) {
        return 0;
    }
    *monotonic_ms = link->deadline_ms;
    return 1;
}

void kal_get_stats(const kal_link *link, kal_stats *stats) {
    if (link == NULL || stats == NULL) {
        return;
    }
    *stats = link->stats;
}

/* ---------------------------------------------------------------- strings */

const char *kal_result_string(kal_result result) {
    switch (result) {
    case KAL_OK: return "ok";
    case KAL_WANT_INPUT: return "want-input";
    case KAL_HAVE_OUTPUT: return "have-output";
    case KAL_DELIVERED: return "delivered";
    case KAL_ERR_INVALID: return "invalid";
    case KAL_ERR_PROTOCOL: return "protocol";
    case KAL_ERR_INTEGRITY: return "integrity";
    case KAL_ERR_TIMEOUT: return "timeout";
    case KAL_ERR_BUSY: return "busy";
    case KAL_ERR_MEMORY: return "memory";
    case KAL_ERR_MODEM: return "modem";
    case KAL_ERR_UNSUPPORTED: return "unsupported";
    case KAL_ERR_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

const char *kal_profile_string(uint8_t profile) {
    switch (profile) {
    case KAL_PROFILE_AUDIBLE_NORMAL: return "audible-normal";
    case KAL_PROFILE_AUDIBLE_FAST: return "audible-fast";
    case KAL_PROFILE_AUDIBLE_FASTEST: return "audible-fastest";
    case KAL_PROFILE_DT_NORMAL: return "dt-normal";
    case KAL_PROFILE_DT_FAST: return "dt-fast";
    case KAL_PROFILE_HIGH_NORMAL: return "high-normal";
    default: return "unknown";
    }
}

const char *kal_version_string(void) {
    return KAL_VERSION_STRING;
}

kal_result kal_describe_frame(const uint8_t *frame, size_t frame_size,
                              char *out, size_t capacity) {
    struct kal_frame decoded;
    enum kal_wire_status status;
    int written;

    if (frame == NULL || out == NULL || capacity == 0u) {
        return KAL_ERR_INVALID;
    }
    status = kal_frame_decode(frame, frame_size, &decoded);
    if (status != KAL_WIRE_OK) {
        written = snprintf(out, capacity, "invalid frame: %s",
                           kal_wire_status_string(status));
        return (written < 0 || (size_t)written >= capacity)
            ? KAL_ERR_INVALID : KAL_ERR_PROTOCOL;
    }
    /* Counts, identifiers and lengths only. Payload bytes are never printed. */
    written = snprintf(out, capacity,
                       "type=%s session=%08x message=%u sequence=%u value=%u "
                       "payload_len=%u flags=%02x",
                       kal_frame_type_string(decoded.type),
                       (unsigned int)decoded.session_id,
                       (unsigned int)decoded.message_id,
                       (unsigned int)decoded.sequence,
                       (unsigned int)decoded.value,
                       (unsigned int)decoded.payload_length,
                       (unsigned int)decoded.flags);
    if (written < 0 || (size_t)written >= capacity) {
        return KAL_ERR_INVALID;
    }
    return KAL_OK;
}
