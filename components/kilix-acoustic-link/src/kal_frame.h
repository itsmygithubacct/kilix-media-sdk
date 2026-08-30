/*
 * KAL1 frame codec — internal.
 *
 * SPDX-License-Identifier: MIT
 *
 * Exact 64-byte fixed frame. Every decode rejects a malformed frame before
 * any state mutation or allocation in the caller.
 */
#ifndef KAL_FRAME_H
#define KAL_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "kilix_acoustic_link.h"

#define KAL_WIRE_VERSION 1u

enum kal_frame_type {
    KAL_TYPE_HELLO = 0x01,
    KAL_TYPE_HELLO_ACK = 0x02,
    KAL_TYPE_DATA = 0x03,
    KAL_TYPE_ACK = 0x04,
    KAL_TYPE_CANCEL = 0x05,
    KAL_TYPE_BEACON = 0x06
};

/* Flags are per-type. Bit 0 only in version 1; every other bit is reserved. */
#define KAL_FLAG_DATA_FINAL   0x01u
#define KAL_FLAG_ACK_DELIVERED 0x01u

#define KAL_CHALLENGE_BYTES 16u

enum kal_wire_status {
    KAL_WIRE_OK = 0,
    KAL_WIRE_ERR_ARGUMENT = -1,
    KAL_WIRE_ERR_SIZE = -2,
    KAL_WIRE_ERR_IDENTITY = -3,
    KAL_WIRE_ERR_CRC = -4,
    KAL_WIRE_ERR_FLAGS = -5,
    KAL_WIRE_ERR_RANGE = -6,
    KAL_WIRE_ERR_CANONICAL = -7,
    KAL_WIRE_ERR_PADDING = -8,
    KAL_WIRE_ERR_TYPE = -9
};

struct kal_frame {
    uint8_t  type;
    uint32_t session_id;
    uint16_t message_id;
    uint16_t sequence;       /* DATA sequence, ACK cumulative base       */
    uint16_t value;          /* DATA total, ACK bitmap, CANCEL reason,
                                HELLO/HELLO_ACK capability word          */
    uint8_t  payload_length;
    uint8_t  flags;
    uint8_t  payload[KAL_PAYLOAD_BYTES];
};

uint32_t kal_crc32c(const uint8_t *data, size_t length);

/* Capability word helpers: (profile << 8) | window_frames. */
uint16_t kal_capability_pack(uint8_t profile, uint16_t window_frames);
uint8_t  kal_capability_profile(uint16_t capability);
uint16_t kal_capability_window(uint16_t capability);

/*
 * Number of DATA segments for an application message. The 32-byte SHA-256
 * trailer is segmented with the message, which is what removes the separate
 * END frame from the KAL1 candidate.
 */
enum kal_wire_status kal_segment_count(size_t application_bytes,
                                       uint16_t *segments_out);

enum kal_wire_status kal_frame_encode(const struct kal_frame *frame,
                                      uint8_t output[KAL_FRAME_BYTES]);
enum kal_wire_status kal_frame_decode(const uint8_t *input, size_t input_length,
                                      struct kal_frame *frame_out);

const char *kal_wire_status_string(enum kal_wire_status status);
const char *kal_frame_type_string(uint8_t type);

#endif /* KAL_FRAME_H */
