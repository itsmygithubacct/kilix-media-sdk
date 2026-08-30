/* KAL1 frame codec. SPDX-License-Identifier: MIT */
#include "kal_frame.h"

#include <string.h>

static void put_u16_be(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)(value & UINT16_C(0xff));
}

static void put_u32_be(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)((value >> 16) & UINT32_C(0xff));
    destination[2] = (uint8_t)((value >> 8) & UINT32_C(0xff));
    destination[3] = (uint8_t)(value & UINT32_C(0xff));
}

static uint16_t get_u16_be(const uint8_t *source) {
    return (uint16_t)(((uint16_t)source[0] << 8) | (uint16_t)source[1]);
}

static uint32_t get_u32_be(const uint8_t *source) {
    return ((uint32_t)source[0] << 24)
        | ((uint32_t)source[1] << 16)
        | ((uint32_t)source[2] << 8)
        | (uint32_t)source[3];
}

uint32_t kal_crc32c(const uint8_t *data, size_t length) {
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;
    unsigned int bit;
    if (data == NULL) {
        return 0u;
    }
    for (index = 0u; index < length; ++index) {
        crc ^= (uint32_t)data[index];
        for (bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = UINT32_C(0) - (crc & UINT32_C(1));
            crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

uint16_t kal_capability_pack(uint8_t profile, uint16_t window_frames) {
    const uint32_t packed = ((uint32_t)profile << 8)
        | ((uint32_t)window_frames & UINT32_C(0xff));
    return (uint16_t)packed;
}

uint8_t kal_capability_profile(uint16_t capability) {
    return (uint8_t)(capability >> 8);
}

uint16_t kal_capability_window(uint16_t capability) {
    return (uint16_t)(capability & 0x00ffu);
}

enum kal_wire_status kal_segment_count(size_t application_bytes,
                                       uint16_t *segments_out) {
    size_t transfer_bytes;
    size_t segments;
    if (segments_out == NULL) {
        return KAL_WIRE_ERR_ARGUMENT;
    }
    if (application_bytes > KAL_MAX_MESSAGE_BYTES) {
        return KAL_WIRE_ERR_RANGE;
    }
    transfer_bytes = application_bytes + KAL_DIGEST_BYTES;
    segments = (transfer_bytes + KAL_PAYLOAD_BYTES - 1u) / KAL_PAYLOAD_BYTES;
    if (segments == 0u || segments > KAL_MAX_SEGMENTS) {
        return KAL_WIRE_ERR_RANGE;
    }
    *segments_out = (uint16_t)segments;
    return KAL_WIRE_OK;
}

/* Every byte between payload end and the CRC must be zero. */
static int padding_is_zero(const uint8_t *input, uint8_t payload_length) {
    size_t index;
    const size_t begin = KAL_HEADER_BYTES + (size_t)payload_length;
    const size_t end = KAL_FRAME_BYTES - KAL_CRC_BYTES;
    for (index = begin; index < end; ++index) {
        if (input[index] != 0u) {
            return 0;
        }
    }
    return 1;
}

/*
 * Canonical-form validation. A frame that carries the right bytes in the
 * wrong shape is rejected, so that one application state has exactly one
 * legal encoding and a mutated frame cannot be silently accepted.
 */
static enum kal_wire_status validate(const struct kal_frame *frame) {
    if (frame == NULL) {
        return KAL_WIRE_ERR_ARGUMENT;
    }
    if (frame->payload_length > KAL_PAYLOAD_BYTES) {
        return KAL_WIRE_ERR_RANGE;
    }
    switch (frame->type) {
    case KAL_TYPE_HELLO:
    case KAL_TYPE_HELLO_ACK: {
        const uint8_t profile = kal_capability_profile(frame->value);
        const uint16_t window = kal_capability_window(frame->value);
        if (frame->flags != 0u) {
            return KAL_WIRE_ERR_FLAGS;
        }
        if (frame->sequence != 0u) {
            return KAL_WIRE_ERR_CANONICAL;
        }
        if (frame->payload_length != KAL_CHALLENGE_BYTES) {
            return KAL_WIRE_ERR_CANONICAL;
        }
        if (profile >= (uint8_t)KAL_PROFILE_COUNT) {
            return KAL_WIRE_ERR_RANGE;
        }
        if (window < KAL_MIN_WINDOW_FRAMES || window > KAL_MAX_WINDOW_FRAMES) {
            return KAL_WIRE_ERR_RANGE;
        }
        return KAL_WIRE_OK;
    }
    case KAL_TYPE_DATA: {
        const int final_expected = frame->value != 0u
            && frame->sequence == (uint16_t)(frame->value - 1u);
        if (frame->value == 0u || frame->value > KAL_MAX_SEGMENTS) {
            return KAL_WIRE_ERR_RANGE;
        }
        if (frame->sequence >= frame->value) {
            return KAL_WIRE_ERR_RANGE;
        }
        if ((frame->flags & (uint8_t)~KAL_FLAG_DATA_FINAL) != 0u) {
            return KAL_WIRE_ERR_FLAGS;
        }
        if (((frame->flags & KAL_FLAG_DATA_FINAL) != 0u) != (final_expected != 0)) {
            return KAL_WIRE_ERR_CANONICAL;
        }
        if ((frame->flags & KAL_FLAG_DATA_FINAL) != 0u) {
            if (frame->payload_length == 0u) {
                return KAL_WIRE_ERR_CANONICAL;
            }
        } else if (frame->payload_length != KAL_PAYLOAD_BYTES) {
            return KAL_WIRE_ERR_CANONICAL;
        }
        return KAL_WIRE_OK;
    }
    case KAL_TYPE_ACK:
        if ((frame->flags & (uint8_t)~KAL_FLAG_ACK_DELIVERED) != 0u) {
            return KAL_WIRE_ERR_FLAGS;
        }
        if (frame->sequence > KAL_MAX_SEGMENTS) {
            return KAL_WIRE_ERR_RANGE;
        }
        if (frame->payload_length != 0u) {
            return KAL_WIRE_ERR_CANONICAL;
        }
        if ((frame->flags & KAL_FLAG_ACK_DELIVERED) != 0u) {
            /* A delivered ACK is folded FIN: it names the whole message and
             * carries no selective bitmap. */
            if (frame->sequence == 0u || frame->value != 0u) {
                return KAL_WIRE_ERR_CANONICAL;
            }
        }
        return KAL_WIRE_OK;
    case KAL_TYPE_CANCEL:
        if (frame->flags != 0u) {
            return KAL_WIRE_ERR_FLAGS;
        }
        if (frame->sequence != 0u || frame->payload_length != 0u) {
            return KAL_WIRE_ERR_CANONICAL;
        }
        if (frame->value > (uint16_t)KAL_CANCEL_REASON_MAX) {
            return KAL_WIRE_ERR_RANGE;
        }
        return KAL_WIRE_OK;
    case KAL_TYPE_BEACON:
        if (frame->flags != 0u) {
            return KAL_WIRE_ERR_FLAGS;
        }
        if (frame->sequence != 0u || frame->value != 0u) {
            return KAL_WIRE_ERR_CANONICAL;
        }
        if (frame->payload_length == 0u
                || frame->payload_length > KAL_MAX_BEACON_BYTES) {
            return KAL_WIRE_ERR_RANGE;
        }
        return KAL_WIRE_OK;
    default:
        return KAL_WIRE_ERR_TYPE;
    }
}

enum kal_wire_status kal_frame_encode(const struct kal_frame *frame,
                                      uint8_t output[KAL_FRAME_BYTES]) {
    enum kal_wire_status status;
    uint32_t crc;
    if (output == NULL) {
        return KAL_WIRE_ERR_ARGUMENT;
    }
    status = validate(frame);
    if (status != KAL_WIRE_OK) {
        return status;
    }
    memset(output, 0, KAL_FRAME_BYTES);
    output[0] = (uint8_t)'K';
    output[1] = (uint8_t)'A';
    output[2] = KAL_WIRE_VERSION;
    output[3] = frame->type;
    put_u32_be(output + 4u, frame->session_id);
    put_u16_be(output + 8u, frame->message_id);
    put_u16_be(output + 10u, frame->sequence);
    put_u16_be(output + 12u, frame->value);
    output[14] = frame->payload_length;
    output[15] = frame->flags;
    if (frame->payload_length != 0u) {
        memcpy(output + KAL_HEADER_BYTES, frame->payload, frame->payload_length);
    }
    crc = kal_crc32c(output, KAL_FRAME_BYTES - KAL_CRC_BYTES);
    put_u32_be(output + KAL_FRAME_BYTES - KAL_CRC_BYTES, crc);
    return KAL_WIRE_OK;
}

enum kal_wire_status kal_frame_decode(const uint8_t *input, size_t input_length,
                                      struct kal_frame *frame_out) {
    struct kal_frame candidate;
    enum kal_wire_status status;
    uint32_t expected_crc;
    uint32_t observed_crc;

    if (frame_out == NULL || input == NULL) {
        return KAL_WIRE_ERR_ARGUMENT;
    }
    if (input_length != KAL_FRAME_BYTES) {
        return KAL_WIRE_ERR_SIZE;
    }
    if (input[0] != (uint8_t)'K' || input[1] != (uint8_t)'A'
            || input[2] != KAL_WIRE_VERSION) {
        return KAL_WIRE_ERR_IDENTITY;
    }
    expected_crc = kal_crc32c(input, KAL_FRAME_BYTES - KAL_CRC_BYTES);
    observed_crc = get_u32_be(input + KAL_FRAME_BYTES - KAL_CRC_BYTES);
    if (expected_crc != observed_crc) {
        return KAL_WIRE_ERR_CRC;
    }
    if (input[14] > KAL_PAYLOAD_BYTES) {
        return KAL_WIRE_ERR_RANGE;
    }
    if (!padding_is_zero(input, input[14])) {
        return KAL_WIRE_ERR_PADDING;
    }

    memset(&candidate, 0, sizeof candidate);
    candidate.type = input[3];
    candidate.session_id = get_u32_be(input + 4u);
    candidate.message_id = get_u16_be(input + 8u);
    candidate.sequence = get_u16_be(input + 10u);
    candidate.value = get_u16_be(input + 12u);
    candidate.payload_length = input[14];
    candidate.flags = input[15];
    if (candidate.payload_length != 0u) {
        memcpy(candidate.payload, input + KAL_HEADER_BYTES,
               candidate.payload_length);
    }
    status = validate(&candidate);
    if (status != KAL_WIRE_OK) {
        return status;
    }
    *frame_out = candidate;
    return KAL_WIRE_OK;
}

const char *kal_wire_status_string(enum kal_wire_status status) {
    switch (status) {
    case KAL_WIRE_OK: return "ok";
    case KAL_WIRE_ERR_ARGUMENT: return "argument";
    case KAL_WIRE_ERR_SIZE: return "size";
    case KAL_WIRE_ERR_IDENTITY: return "identity";
    case KAL_WIRE_ERR_CRC: return "crc";
    case KAL_WIRE_ERR_FLAGS: return "flags";
    case KAL_WIRE_ERR_RANGE: return "range";
    case KAL_WIRE_ERR_CANONICAL: return "canonical";
    case KAL_WIRE_ERR_PADDING: return "padding";
    case KAL_WIRE_ERR_TYPE: return "type";
    default: return "unknown";
    }
}

const char *kal_frame_type_string(uint8_t type) {
    switch (type) {
    case KAL_TYPE_HELLO: return "HELLO";
    case KAL_TYPE_HELLO_ACK: return "HELLO_ACK";
    case KAL_TYPE_DATA: return "DATA";
    case KAL_TYPE_ACK: return "ACK";
    case KAL_TYPE_CANCEL: return "CANCEL";
    case KAL_TYPE_BEACON: return "BEACON";
    default: return "UNKNOWN";
    }
}
