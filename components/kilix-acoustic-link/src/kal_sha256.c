/* SHA-256. SPDX-License-Identifier: MIT
 *
 * Carried forward from the F111 R51 reassembly spike unchanged in behaviour;
 * renamed into the kal_ namespace and given constant-time digest equality.
 */
#include "kal_sha256.h"

#include <string.h>


struct sha256_context {
    uint32_t state[8];
    uint8_t block[64];
    size_t block_length;
    uint64_t total_bytes;
};


static uint32_t rotate_right(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32u - count));
}


static void sha256_transform(
    struct sha256_context *context,
    const uint8_t block[64]) {
    static const uint32_t constants[64] = {
        UINT32_C(0x428a2f98), UINT32_C(0x71374491),
        UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
        UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
        UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
        UINT32_C(0xd807aa98), UINT32_C(0x12835b01),
        UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
        UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe),
        UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
        UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
        UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
        UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa),
        UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
        UINT32_C(0x983e5152), UINT32_C(0xa831c66d),
        UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
        UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
        UINT32_C(0x06ca6351), UINT32_C(0x14292967),
        UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138),
        UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
        UINT32_C(0x650a7354), UINT32_C(0x766a0abb),
        UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
        UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
        UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
        UINT32_C(0xd192e819), UINT32_C(0xd6990624),
        UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
        UINT32_C(0x19a4c116), UINT32_C(0x1e376c08),
        UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
        UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
        UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
        UINT32_C(0x748f82ee), UINT32_C(0x78a5636f),
        UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
        UINT32_C(0x90befffa), UINT32_C(0xa4506ceb),
        UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)
    };
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0u; index < 16u; ++index) {
        const size_t offset = index * 4u;
        words[index] = ((uint32_t)block[offset] << 24)
            | ((uint32_t)block[offset + 1u] << 16)
            | ((uint32_t)block[offset + 2u] << 8)
            | (uint32_t)block[offset + 3u];
    }
    for (index = 16u; index < 64u; ++index) {
        const uint32_t x = words[index - 15u];
        const uint32_t y = words[index - 2u];
        const uint32_t sigma0 = rotate_right(x, 7u)
            ^ rotate_right(x, 18u) ^ (x >> 3);
        const uint32_t sigma1 = rotate_right(y, 17u)
            ^ rotate_right(y, 19u) ^ (y >> 10);
        words[index] = words[index - 16u] + sigma0
            + words[index - 7u] + sigma1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (index = 0u; index < 64u; ++index) {
        const uint32_t sum1 = rotate_right(e, 6u)
            ^ rotate_right(e, 11u) ^ rotate_right(e, 25u);
        const uint32_t choice = (e & f) ^ ((~e) & g);
        const uint32_t temporary1 = h + sum1 + choice
            + constants[index] + words[index];
        const uint32_t sum0 = rotate_right(a, 2u)
            ^ rotate_right(a, 13u) ^ rotate_right(a, 22u);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}


static void sha256_init(struct sha256_context *context) {
    static const uint32_t initial[8] = {
        UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
        UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
        UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
        UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
    };
    memcpy(context->state, initial, sizeof initial);
    memset(context->block, 0, sizeof context->block);
    context->block_length = 0u;
    context->total_bytes = UINT64_C(0);
}


static void sha256_update(
    struct sha256_context *context,
    const uint8_t *input,
    size_t input_length) {
    size_t index;
    for (index = 0u; index < input_length; ++index) {
        context->block[context->block_length] = input[index];
        context->block_length += 1u;
        context->total_bytes += UINT64_C(1);
        if (context->block_length == sizeof context->block) {
            sha256_transform(context, context->block);
            context->block_length = 0u;
        }
    }
}


static void sha256_final(
    struct sha256_context *context,
    uint8_t output[KAL_DIGEST_BYTES]) {
    const uint64_t bit_length = context->total_bytes * UINT64_C(8);
    size_t index;
    context->block[context->block_length] = UINT8_C(0x80);
    context->block_length += 1u;
    if (context->block_length > 56u) {
        while (context->block_length < sizeof context->block) {
            context->block[context->block_length] = 0u;
            context->block_length += 1u;
        }
        sha256_transform(context, context->block);
        context->block_length = 0u;
    }
    while (context->block_length < 56u) {
        context->block[context->block_length] = 0u;
        context->block_length += 1u;
    }
    for (index = 0u; index < 8u; ++index) {
        const unsigned int shift = (unsigned int)((7u - index) * 8u);
        context->block[56u + index] = (uint8_t)(bit_length >> shift);
    }
    sha256_transform(context, context->block);
    for (index = 0u; index < 8u; ++index) {
        const uint32_t value = context->state[index];
        output[index * 4u] = (uint8_t)(value >> 24);
        output[index * 4u + 1u] = (uint8_t)(value >> 16);
        output[index * 4u + 2u] = (uint8_t)(value >> 8);
        output[index * 4u + 3u] = (uint8_t)value;
    }
}


int kal_sha256(
    const uint8_t *input,
    size_t input_length,
    uint8_t output[KAL_DIGEST_BYTES]) {
    struct sha256_context context;
    if (output == NULL || (input == NULL && input_length != 0u)) {
        return -1;
    }
    sha256_init(&context);
    sha256_update(&context, input, input_length);
    sha256_final(&context, output);
    return 0;
}


int kal_digest_equal(const uint8_t left[KAL_DIGEST_BYTES],
                     const uint8_t right[KAL_DIGEST_BYTES]) {
    uint8_t difference = 0u;
    size_t index;
    for (index = 0u; index < KAL_DIGEST_BYTES; ++index) {
        difference = (uint8_t)(difference | (uint8_t)(left[index] ^ right[index]));
    }
    return difference == 0u;
}
