/* KAL1 codec conformance. SPDX-License-Identifier: MIT */
#include "kal_frame.h"
#include "kal_test.h"

#include <string.h>

static void hexdump(const uint8_t *bytes, size_t size, char *out) {
    size_t index;
    for (index = 0u; index < size; ++index) {
        static const char digits[] = "0123456789abcdef";
        out[index * 2u] = digits[bytes[index] >> 4];
        out[index * 2u + 1u] = digits[bytes[index] & 0x0fu];
    }
    out[size * 2u] = '\0';
}

static struct kal_frame make_data(uint16_t sequence, uint16_t total) {
    struct kal_frame frame;
    size_t index;
    memset(&frame, 0, sizeof frame);
    frame.type = (uint8_t)KAL_TYPE_DATA;
    frame.session_id = UINT32_C(0x11223344);
    frame.message_id = 7u;
    frame.sequence = sequence;
    frame.value = total;
    frame.flags = (sequence == (uint16_t)(total - 1u)) ? KAL_FLAG_DATA_FINAL : 0u;
    frame.payload_length = (sequence == (uint16_t)(total - 1u))
        ? 20u : (uint8_t)KAL_PAYLOAD_BYTES;
    for (index = 0u; index < frame.payload_length; ++index) {
        frame.payload[index] = (uint8_t)(index + sequence);
    }
    return frame;
}

static struct kal_frame make_hello(uint8_t type) {
    struct kal_frame frame;
    size_t index;
    memset(&frame, 0, sizeof frame);
    frame.type = type;
    frame.session_id = UINT32_C(0xa1b2c3d4);
    frame.message_id = 0u;
    frame.value = kal_capability_pack((uint8_t)KAL_PROFILE_AUDIBLE_NORMAL, 4u);
    frame.payload_length = KAL_CHALLENGE_BYTES;
    for (index = 0u; index < KAL_CHALLENGE_BYTES; ++index) {
        frame.payload[index] = (uint8_t)(0xf0u - index);
    }
    return frame;
}

static struct kal_frame make_ack(int delivered) {
    struct kal_frame frame;
    memset(&frame, 0, sizeof frame);
    frame.type = (uint8_t)KAL_TYPE_ACK;
    frame.session_id = UINT32_C(0x11223344);
    frame.message_id = 7u;
    if (delivered) {
        frame.sequence = 4u;
        frame.value = 0u;
        frame.flags = KAL_FLAG_ACK_DELIVERED;
    } else {
        frame.sequence = 1u;
        frame.value = 0x000au;
    }
    return frame;
}

static struct kal_frame make_cancel(void) {
    struct kal_frame frame;
    memset(&frame, 0, sizeof frame);
    frame.type = (uint8_t)KAL_TYPE_CANCEL;
    frame.session_id = UINT32_C(0x11223344);
    frame.message_id = 7u;
    frame.value = (uint16_t)KAL_CANCEL_USER;
    return frame;
}

static struct kal_frame make_beacon(void) {
    struct kal_frame frame;
    size_t index;
    memset(&frame, 0, sizeof frame);
    frame.type = (uint8_t)KAL_TYPE_BEACON;
    frame.session_id = UINT32_C(0x0badc0de);
    frame.message_id = 3u;
    frame.payload_length = 12u;
    for (index = 0u; index < frame.payload_length; ++index) {
        frame.payload[index] = (uint8_t)(index * 3u + 1u);
    }
    return frame;
}

struct golden {
    const char *name;
    struct kal_frame frame;
};

static size_t build_golden(struct golden *out) {
    size_t count = 0u;
    out[count].name = "hello";            out[count].frame = make_hello((uint8_t)KAL_TYPE_HELLO); count++;
    out[count].name = "hello-ack";        out[count].frame = make_hello((uint8_t)KAL_TYPE_HELLO_ACK); count++;
    out[count].name = "data-first";       out[count].frame = make_data(0u, 4u); count++;
    out[count].name = "data-final";       out[count].frame = make_data(3u, 4u); count++;
    out[count].name = "ack-partial";      out[count].frame = make_ack(0); count++;
    out[count].name = "ack-delivered";    out[count].frame = make_ack(1); count++;
    out[count].name = "cancel";           out[count].frame = make_cancel(); count++;
    out[count].name = "beacon";           out[count].frame = make_beacon(); count++;
    return count;
}

static int emit_vectors(void) {
    struct golden golden[8];
    const size_t count = build_golden(golden);
    size_t index;
    char hex[KAL_FRAME_BYTES * 2u + 1u];
    uint8_t encoded[KAL_FRAME_BYTES];

    printf("# KAL1 golden frame vectors — candidate wire, not frozen\n");
    printf("# name hex64\n");
    for (index = 0u; index < count; ++index) {
        if (kal_frame_encode(&golden[index].frame, encoded) != KAL_WIRE_OK) {
            fprintf(stderr, "encode failed for %s\n", golden[index].name);
            return 1;
        }
        hexdump(encoded, KAL_FRAME_BYTES, hex);
        printf("%s %s\n", golden[index].name, hex);
    }
    return 0;
}

static void check_golden_file(void) {
    struct golden golden[8];
    const size_t count = build_golden(golden);
    FILE *file = fopen("tests/vectors/kal1-golden.txt", "r");
    size_t matched = 0u;
    char line[256];

    KAL_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    while (fgets(line, (int)sizeof line, file) != NULL) {
        char name[64];
        char hex[KAL_FRAME_BYTES * 2u + 8u];
        size_t index;
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sscanf(line, "%63s %135s", name, hex) != 2) {
            continue;
        }
        for (index = 0u; index < count; ++index) {
            uint8_t encoded[KAL_FRAME_BYTES];
            char expected[KAL_FRAME_BYTES * 2u + 1u];
            if (strcmp(name, golden[index].name) != 0) {
                continue;
            }
            KAL_CHECK(kal_frame_encode(&golden[index].frame, encoded) == KAL_WIRE_OK);
            hexdump(encoded, KAL_FRAME_BYTES, expected);
            KAL_CHECK(strcmp(expected, hex) == 0);
            matched++;
        }
    }
    (void)fclose(file);
    KAL_GROUP("golden vectors matched", matched, count);
    KAL_CHECK(matched == count);
}

static void check_crc(void) {
    /* CRC-32C (Castagnoli) reference values. */
    static const uint8_t check[] = "123456789";
    KAL_CHECK(kal_crc32c(check, 9u) == UINT32_C(0xe3069283));
    KAL_CHECK(kal_crc32c((const uint8_t *)"", 0u) == UINT32_C(0x00000000));
    {
        uint8_t zeros[32];
        memset(zeros, 0, sizeof zeros);
        KAL_CHECK(kal_crc32c(zeros, 32u) == UINT32_C(0x8a9136aa));
    }
}

static void check_round_trips(void) {
    struct golden golden[8];
    const size_t count = build_golden(golden);
    size_t index;
    size_t ok = 0u;

    for (index = 0u; index < count; ++index) {
        uint8_t encoded[KAL_FRAME_BYTES];
        struct kal_frame decoded;
        if (kal_frame_encode(&golden[index].frame, encoded) == KAL_WIRE_OK
                && kal_frame_decode(encoded, KAL_FRAME_BYTES, &decoded) == KAL_WIRE_OK
                && memcmp(&decoded, &golden[index].frame, sizeof decoded) == 0) {
            ok++;
        }
    }
    KAL_GROUP("frame round trips", ok, count);
    KAL_CHECK(ok == count);
}

static void check_payload_lengths(void) {
    /* Every legal final-segment payload length, 1..44. */
    size_t ok = 0u;
    uint8_t length;
    for (length = 1u; length <= (uint8_t)KAL_PAYLOAD_BYTES; ++length) {
        struct kal_frame frame = make_data(2u, 3u);
        uint8_t encoded[KAL_FRAME_BYTES];
        struct kal_frame decoded;
        frame.payload_length = length;
        memset(frame.payload + length, 0, KAL_PAYLOAD_BYTES - length);
        if (kal_frame_encode(&frame, encoded) == KAL_WIRE_OK
                && kal_frame_decode(encoded, KAL_FRAME_BYTES, &decoded) == KAL_WIRE_OK
                && decoded.payload_length == length) {
            ok++;
        }
    }
    KAL_GROUP("final payload lengths 1..44", ok, KAL_PAYLOAD_BYTES);
    KAL_CHECK(ok == KAL_PAYLOAD_BYTES);
}

struct mutation {
    const char *name;
    size_t offset;
    uint8_t value;
};

static void check_mutations(void) {
    struct kal_frame frame = make_data(0u, 4u);
    uint8_t base[KAL_FRAME_BYTES];
    static const struct mutation mutations[] = {
        {"magic-0", 0u, 'X'},
        {"magic-1", 1u, 'X'},
        {"version", 2u, 2u},
        {"type-zero", 3u, 0u},
        {"type-unknown", 3u, 0x40u},
        {"payload-length-45", 14u, 45u},
        {"payload-length-255", 14u, 255u},
        {"reserved-flag", 15u, 0x02u},
        {"all-flags", 15u, 0xffu},
        {"padding", 62u, 1u},
        {"crc-low", 63u, 0xffu},
        {"crc-high", 60u, 0xffu},
        {"sequence-high", 10u, 0xffu},
        {"total-zero-high", 12u, 0u},
        {"session-byte", 4u, 0xffu}
    };
    const size_t count = sizeof mutations / sizeof mutations[0];
    size_t rejected = 0u;
    size_t index;

    KAL_CHECK(kal_frame_encode(&frame, base) == KAL_WIRE_OK);
    for (index = 0u; index < count; ++index) {
        uint8_t mutated[KAL_FRAME_BYTES];
        struct kal_frame decoded;
        memcpy(mutated, base, sizeof mutated);
        if (mutated[mutations[index].offset] == mutations[index].value) {
            mutated[mutations[index].offset] = (uint8_t)(mutations[index].value + 1u);
        } else {
            mutated[mutations[index].offset] = mutations[index].value;
        }
        if (kal_frame_decode(mutated, KAL_FRAME_BYTES, &decoded) != KAL_WIRE_OK) {
            rejected++;
        }
    }
    KAL_GROUP("targeted mutations rejected", rejected, count);
    KAL_CHECK(rejected == count);
}

static void check_single_bit_corruption(void) {
    /* CRC-32C must catch every single-bit error in a 64-byte frame. */
    struct kal_frame frame = make_data(1u, 4u);
    uint8_t base[KAL_FRAME_BYTES];
    size_t rejected = 0u;
    size_t byte;
    unsigned int bit;
    const size_t total = KAL_FRAME_BYTES * 8u;

    KAL_CHECK(kal_frame_encode(&frame, base) == KAL_WIRE_OK);
    for (byte = 0u; byte < KAL_FRAME_BYTES; ++byte) {
        for (bit = 0u; bit < 8u; ++bit) {
            uint8_t mutated[KAL_FRAME_BYTES];
            struct kal_frame decoded;
            memcpy(mutated, base, sizeof mutated);
            mutated[byte] = (uint8_t)(mutated[byte] ^ (uint8_t)(1u << bit));
            if (kal_frame_decode(mutated, KAL_FRAME_BYTES, &decoded) != KAL_WIRE_OK) {
                rejected++;
            }
        }
    }
    KAL_GROUP("single-bit corruptions rejected", rejected, total);
    KAL_CHECK(rejected == total);
}

static void check_sizes(void) {
    struct kal_frame frame = make_data(0u, 4u);
    uint8_t base[KAL_FRAME_BYTES];
    struct kal_frame decoded;
    size_t rejected = 0u;
    size_t size;
    const size_t population = 66u; /* 0..65 with 64 excluded below */

    KAL_CHECK(kal_frame_encode(&frame, base) == KAL_WIRE_OK);
    for (size = 0u; size <= 65u; ++size) {
        const enum kal_wire_status status =
            kal_frame_decode(base, size, &decoded);
        if (size == KAL_FRAME_BYTES) {
            if (status == KAL_WIRE_OK) {
                rejected++;
            }
        } else if (status != KAL_WIRE_OK) {
            rejected++;
        }
    }
    KAL_GROUP("truncated/oversize lengths handled", rejected, population);
    KAL_CHECK(rejected == population);
}

static void check_canonical(void) {
    size_t rejected = 0u;
    const size_t total = 9u;
    struct kal_frame frame;
    uint8_t encoded[KAL_FRAME_BYTES];

    frame = make_data(0u, 4u); frame.value = 0u;
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;
    frame = make_data(0u, 4u); frame.value = (uint16_t)(KAL_MAX_SEGMENTS + 1u);
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;
    frame = make_data(0u, 4u); frame.sequence = 4u;
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;
    frame = make_data(0u, 4u); frame.flags = KAL_FLAG_DATA_FINAL;
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;
    frame = make_data(3u, 4u); frame.flags = 0u;
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;
    frame = make_data(0u, 4u); frame.payload_length = 10u;
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;
    frame = make_ack(0); frame.payload_length = 4u;
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;
    frame = make_ack(1); frame.value = 3u;
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;
    frame = make_cancel(); frame.value = (uint16_t)(KAL_CANCEL_REASON_MAX + 1u);
    if (kal_frame_encode(&frame, encoded) != KAL_WIRE_OK) rejected++;

    KAL_GROUP("non-canonical encodes refused", rejected, total);
    KAL_CHECK(rejected == total);
}

static void check_segment_equation(void) {
    size_t ok = 0u;
    size_t size;
    uint16_t segments = 0u;
    const size_t population = KAL_MAX_MESSAGE_BYTES + 1u;

    for (size = 0u; size <= KAL_MAX_MESSAGE_BYTES; ++size) {
        const size_t transfer = size + KAL_DIGEST_BYTES;
        const size_t expected = (transfer + KAL_PAYLOAD_BYTES - 1u) / KAL_PAYLOAD_BYTES;
        if (kal_segment_count(size, &segments) == KAL_WIRE_OK
                && (size_t)segments == expected
                && segments >= 1u && segments <= KAL_MAX_SEGMENTS) {
            ok++;
        }
    }
    KAL_GROUP("segment equation sizes 0..4096", ok, population);
    KAL_CHECK(ok == population);
    KAL_CHECK(kal_segment_count(KAL_MAX_MESSAGE_BYTES + 1u, &segments) != KAL_WIRE_OK);
    KAL_CHECK(kal_segment_count(4096u, &segments) == KAL_WIRE_OK && segments == 94u);
}

static void check_describe(void) {
    struct kal_frame frame = make_data(0u, 4u);
    uint8_t encoded[KAL_FRAME_BYTES];
    char text[192];

    KAL_CHECK(kal_frame_encode(&frame, encoded) == KAL_WIRE_OK);
    KAL_CHECK(kal_describe_frame(encoded, KAL_FRAME_BYTES, text, sizeof text) == KAL_OK);
    KAL_CHECK(strstr(text, "type=DATA") != NULL);
    /* Payload bytes must never appear in a diagnostic line. */
    KAL_CHECK(strstr(text, "payload_len=44") != NULL);
    encoded[63] ^= 0xffu;
    KAL_CHECK(kal_describe_frame(encoded, KAL_FRAME_BYTES, text, sizeof text)
              == KAL_ERR_PROTOCOL);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--emit-vectors") == 0) {
        return emit_vectors();
    }
    printf("test_frame — KAL1 codec conformance\n");
    check_crc();
    check_golden_file();
    check_round_trips();
    check_payload_lengths();
    check_mutations();
    check_single_bit_corruption();
    check_sizes();
    check_canonical();
    check_segment_equation();
    check_describe();
    return kal_test_report("test_frame");
}
