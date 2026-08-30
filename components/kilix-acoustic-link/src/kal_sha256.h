/* SHA-256 for whole-message validation. SPDX-License-Identifier: MIT */
#ifndef KAL_SHA256_H
#define KAL_SHA256_H

#include <stddef.h>
#include <stdint.h>

#include "kilix_acoustic_link.h"

int kal_sha256(const uint8_t *input, size_t input_length,
               uint8_t output[KAL_DIGEST_BYTES]);

/* Constant-time-ish equality over a fixed 32-byte digest. */
int kal_digest_equal(const uint8_t left[KAL_DIGEST_BYTES],
                     const uint8_t right[KAL_DIGEST_BYTES]);

#endif /* KAL_SHA256_H */
