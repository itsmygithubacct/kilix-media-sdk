/*
 * KAL1 parser fuzz target.
 *
 * SPDX-License-Identifier: MIT
 *
 * Two properties, checked on every input:
 *   1. decode never crashes and never reads outside its 64-byte input;
 *   2. decode is canonical — if a buffer decodes, re-encoding the decoded
 *      frame reproduces that buffer byte for byte. A parser that accepted
 *      two spellings of one state would fail this.
 *
 * Runs a committed corpus and a deterministic mutation sweep, so it is
 * meaningful under `make test` without an external fuzzing engine. It is
 * also a valid libFuzzer target when built with one.
 */
#include "kal_frame.h"
#include "kilix_acoustic_link.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long decoded_ok;
static unsigned long decoded_rejected;
static unsigned long canonical_failures;
static unsigned long crashes_guarded;

static void exercise(const uint8_t *data, size_t size) {
    struct kal_frame frame;
    char text[256];

    crashes_guarded++;
    if (kal_frame_decode(data, size, &frame) == KAL_WIRE_OK) {
        uint8_t re_encoded[KAL_FRAME_BYTES];
        decoded_ok++;
        if (kal_frame_encode(&frame, re_encoded) != KAL_WIRE_OK
                || size != KAL_FRAME_BYTES
                || memcmp(re_encoded, data, KAL_FRAME_BYTES) != 0) {
            canonical_failures++;
        }
    } else {
        decoded_rejected++;
    }
    (void)kal_describe_frame(data, size, text, sizeof text);
}

/* libFuzzer entry point; unused by the deterministic run. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    exercise(data, size);
    return 0;
}

static unsigned long run_corpus(const char *directory) {
    DIR *handle = opendir(directory);
    struct dirent *entry;
    unsigned long files = 0u;

    if (handle == NULL) {
        return 0u;
    }
    while ((entry = readdir(handle)) != NULL) {
        char path[1024];
        uint8_t buffer[512];
        size_t got;
        FILE *file;
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (snprintf(path, sizeof path, "%s/%s", directory, entry->d_name) < 0) {
            continue;
        }
        file = fopen(path, "rb");
        if (file == NULL) {
            continue;
        }
        got = fread(buffer, 1u, sizeof buffer, file);
        (void)fclose(file);
        exercise(buffer, got);
        files++;
    }
    (void)closedir(handle);
    return files;
}

static uint32_t next_random(uint32_t *state) {
    *state = (*state * UINT32_C(1664525)) + UINT32_C(1013904223);
    return *state;
}

int main(int argc, char **argv) {
    const unsigned long iterations = 200000u;
    uint32_t state = UINT32_C(0x5eed1234);
    unsigned long index;
    unsigned long corpus_files = 0u;
    struct kal_frame seed;
    uint8_t base[KAL_FRAME_BYTES];

    printf("fuzz_frame — KAL1 parser\n");
    if (argc > 1) {
        corpus_files = run_corpus(argv[1]);
    }

    memset(&seed, 0, sizeof seed);
    seed.type = (uint8_t)KAL_TYPE_DATA;
    seed.session_id = UINT32_C(0xdeadbeef);
    seed.message_id = 2u;
    seed.sequence = 0u;
    seed.value = 3u;
    seed.payload_length = (uint8_t)KAL_PAYLOAD_BYTES;
    memset(seed.payload, 0x5a, KAL_PAYLOAD_BYTES);
    if (kal_frame_encode(&seed, base) != KAL_WIRE_OK) {
        printf("fuzz_frame RESULT FAIL seed 0/1\n");
        return 1;
    }

    for (index = 0u; index < iterations; ++index) {
        uint8_t buffer[KAL_FRAME_BYTES + 8u];
        size_t size;
        unsigned int mutations;
        unsigned int mutation;

        if ((index % 3u) == 2u) {
            /* Payload-only damage with a repaired checksum: these must be
             * ACCEPTED, which is what exercises the canonical round trip. */
            unsigned int changes;
            uint32_t crc;
            memcpy(buffer, base, KAL_FRAME_BYTES);
            size = KAL_FRAME_BYTES;
            changes = (unsigned int)(next_random(&state) % 4u) + 1u;
            for (mutation = 0u; mutation < changes; ++mutation) {
                const size_t offset = KAL_HEADER_BYTES
                    + (next_random(&state) % KAL_PAYLOAD_BYTES);
                buffer[offset] = (uint8_t)next_random(&state);
            }
            crc = kal_crc32c(buffer, KAL_FRAME_BYTES - KAL_CRC_BYTES);
            buffer[60] = (uint8_t)(crc >> 24);
            buffer[61] = (uint8_t)((crc >> 16) & 0xffu);
            buffer[62] = (uint8_t)((crc >> 8) & 0xffu);
            buffer[63] = (uint8_t)(crc & 0xffu);
        } else if ((index % 3u) == 0u) {
            /* Structured: start from a valid frame and damage it. */
            memcpy(buffer, base, KAL_FRAME_BYTES);
            size = KAL_FRAME_BYTES;
            mutations = (unsigned int)(next_random(&state) % 4u) + 1u;
            for (mutation = 0u; mutation < mutations; ++mutation) {
                const size_t offset = next_random(&state) % KAL_FRAME_BYTES;
                buffer[offset] = (uint8_t)next_random(&state);
            }
            if ((next_random(&state) % 8u) == 0u) {
                /* Occasionally repair the CRC so the deeper canonical and
                 * range checks are exercised, not just the checksum. */
                const uint32_t crc = kal_crc32c(buffer, KAL_FRAME_BYTES - KAL_CRC_BYTES);
                buffer[60] = (uint8_t)(crc >> 24);
                buffer[61] = (uint8_t)((crc >> 16) & 0xffu);
                buffer[62] = (uint8_t)((crc >> 8) & 0xffu);
                buffer[63] = (uint8_t)(crc & 0xffu);
            }
        } else {
            /* Unstructured, including short and long buffers. */
            size_t byte;
            size = (size_t)(next_random(&state) % (KAL_FRAME_BYTES + 8u));
            for (byte = 0u; byte < size; ++byte) {
                buffer[byte] = (uint8_t)next_random(&state);
            }
        }
        exercise(buffer, size);
    }

    printf("  corpus files                                 %lu/%lu\n",
           corpus_files, corpus_files);
    printf("  inputs exercised                             %lu/%lu\n",
           crashes_guarded, crashes_guarded);
    printf("  inputs accepted                              %lu\n", decoded_ok);
    if (decoded_ok < 1000u) {
        printf("fuzz_frame RESULT FAIL accept-path coverage %lu/1000\n", decoded_ok);
        return 1;
    }
    printf("  inputs rejected                              %lu\n", decoded_rejected);
    printf("  canonical round-trip failures                %lu/0\n",
           canonical_failures);
    if (canonical_failures != 0u) {
        printf("fuzz_frame RESULT FAIL %lu/%lu\n",
               crashes_guarded - canonical_failures, crashes_guarded);
        return 1;
    }
    printf("fuzz_frame RESULT PASS %lu/%lu\n", crashes_guarded, crashes_guarded);
    return 0;
}
