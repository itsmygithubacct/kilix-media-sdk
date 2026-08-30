/* Modem adapter. SPDX-License-Identifier: MIT */
#include "kal_modem.h"

#include <stdlib.h>
#include <string.h>

#include "ggwave/ggwave.h"

#define KAL_GGWAVE_VOLUME 25
#define KAL_TEST_SYNC_SAMPLES 4u
#define KAL_TEST_FRAME_SAMPLES (KAL_TEST_SYNC_SAMPLES + KAL_FRAME_BYTES)

struct kal_modem {
    uint8_t backend;
    uint8_t profile;
    uint32_t sample_rate;
    size_t frame_samples;
    uint64_t frame_airtime_ms;

    /* ggwave backend */
    int instance;                /* ggwave_Instance, -1 when unused */
    int protocol_id;
    size_t chunk_samples;
    int16_t *chunk;
    size_t chunk_fill;
    int16_t *encode_scratch;

    /* test passthrough backend */
    int16_t window[KAL_TEST_FRAME_SAMPLES];
    size_t window_fill;
};

static int profile_to_protocol(uint8_t profile, int *out) {
    switch (profile) {
    case KAL_PROFILE_AUDIBLE_NORMAL:  *out = GGWAVE_PROTOCOL_AUDIBLE_NORMAL;  return 1;
    case KAL_PROFILE_AUDIBLE_FAST:    *out = GGWAVE_PROTOCOL_AUDIBLE_FAST;    return 1;
    case KAL_PROFILE_AUDIBLE_FASTEST: *out = GGWAVE_PROTOCOL_AUDIBLE_FASTEST; return 1;
    case KAL_PROFILE_DT_NORMAL:       *out = GGWAVE_PROTOCOL_DT_NORMAL;       return 1;
    case KAL_PROFILE_DT_FAST:         *out = GGWAVE_PROTOCOL_DT_FAST;         return 1;
    case KAL_PROFILE_HIGH_NORMAL:     *out = GGWAVE_PROTOCOL_ULTRASOUND_NORMAL; return 1;
    default: return 0;
    }
}

void kal_modem_global_init(void) {
    static int done = 0;
    if (done == 0) {
        /* Payloads must never reach a log stream. */
        ggwave_setLogFile(NULL);
        done = 1;
    }
}

/*
 * Frame size for one 64-byte payload, in samples.
 *
 * Measured on a THROWAWAY instance. In the pinned ggwave v0.4.3 the
 * memory-safe size-query form of ggwave_encode (query != 0) permanently
 * disables the same instance's decoder: a link that sized itself on its own
 * instance would transmit correctly and never receive again. A real encode
 * (query == 0) does not have this effect. The throwaway is freed before the
 * working instance is created, so the process instance budget is unchanged.
 */
static size_t query_frame_samples(uint8_t profile, int protocol_id,
                                  uint32_t sample_rate) {
    static size_t cache[KAL_PROFILE_COUNT];
    ggwave_Parameters parameters;
    ggwave_Instance probe_instance;
    uint8_t probe[KAL_FRAME_BYTES];
    int query_bytes;

    if (profile >= (uint8_t)KAL_PROFILE_COUNT) {
        return 0u;
    }
    if (cache[profile] != 0u) {
        return cache[profile];
    }
    parameters = ggwave_getDefaultParameters();
    parameters.payloadLength = (int)KAL_FRAME_BYTES;
    parameters.sampleRateInp = (float)sample_rate;
    parameters.sampleRateOut = (float)sample_rate;
    parameters.sampleRate = (float)sample_rate;
    parameters.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
    parameters.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
    parameters.operatingMode = GGWAVE_OPERATING_MODE_TX;

    probe_instance = ggwave_init(parameters);
    if (probe_instance < 0) {
        return 0u;
    }
    memset(probe, 0, sizeof probe);
    query_bytes = ggwave_encode(probe_instance, probe, (int)KAL_FRAME_BYTES,
                                (ggwave_ProtocolId)protocol_id,
                                KAL_GGWAVE_VOLUME, NULL, 1);
    ggwave_free(probe_instance);
    if (query_bytes <= 0 || (query_bytes % 2) != 0) {
        return 0u;
    }
    cache[profile] = (size_t)query_bytes / 2u;
    return cache[profile];
}

static kal_result create_ggwave(struct kal_modem *modem) {
    ggwave_Parameters parameters = ggwave_getDefaultParameters();

    if (!profile_to_protocol(modem->profile, &modem->protocol_id)) {
        return KAL_ERR_INVALID;
    }
    parameters.payloadLength = (int)KAL_FRAME_BYTES;
    parameters.sampleRateInp = (float)modem->sample_rate;
    parameters.sampleRateOut = (float)modem->sample_rate;
    parameters.sampleRate = (float)modem->sample_rate;
    parameters.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
    parameters.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
    parameters.operatingMode = GGWAVE_OPERATING_MODE_RX_AND_TX;

    if (parameters.samplesPerFrame <= 0) {
        return KAL_ERR_MODEM;
    }
    modem->chunk_samples = (size_t)parameters.samplesPerFrame;

    modem->frame_samples = query_frame_samples(modem->profile,
                                               modem->protocol_id,
                                               modem->sample_rate);
    if (modem->frame_samples == 0u) {
        return KAL_ERR_MODEM;
    }

    /* The engine holds at most GGWAVE_MAX_INSTANCES live instances per
     * process; exhaustion is reported, never worked around. */
    modem->instance = ggwave_init(parameters);
    if (modem->instance < 0) {
        return KAL_ERR_MEMORY;
    }

    /* Every buffer the PCM path can ever need is allocated here, so neither
     * transmission nor hostile input can grow memory later. */
    modem->chunk = (int16_t *)calloc(modem->chunk_samples, sizeof(int16_t));
    modem->encode_scratch = (int16_t *)calloc(modem->frame_samples, sizeof(int16_t));
    if (modem->chunk == NULL || modem->encode_scratch == NULL) {
        return KAL_ERR_MEMORY;
    }
    return KAL_OK;
}

kal_result kal_modem_create(struct kal_modem **out, uint8_t backend,
                            uint8_t profile, uint32_t sample_rate) {
    struct kal_modem *modem;
    kal_result result;

    if (out == NULL) {
        return KAL_ERR_INVALID;
    }
    *out = NULL;
    if (sample_rate != KAL_SAMPLE_RATE) {
        return KAL_ERR_INVALID;
    }
    if (profile >= (uint8_t)KAL_PROFILE_COUNT) {
        return KAL_ERR_INVALID;
    }
    modem = (struct kal_modem *)calloc(1u, sizeof *modem);
    if (modem == NULL) {
        return KAL_ERR_MEMORY;
    }
    modem->backend = backend;
    modem->profile = profile;
    modem->sample_rate = sample_rate;
    modem->instance = -1;

    if (backend == KAL_MODEM_GGWAVE) {
        result = create_ggwave(modem);
        if (result != KAL_OK) {
            kal_modem_free(modem);
            return result;
        }
    } else if (backend == KAL_MODEM_TEST_PASSTHROUGH) {
#ifdef KAL_ENABLE_TEST_MODEM
        modem->frame_samples = KAL_TEST_FRAME_SAMPLES;
        modem->chunk_samples = 1u;
#else
        kal_modem_free(modem);
        return KAL_ERR_UNSUPPORTED;
#endif
    } else {
        kal_modem_free(modem);
        return KAL_ERR_INVALID;
    }

    modem->frame_airtime_ms =
        (uint64_t)((modem->frame_samples * 1000u) / modem->sample_rate);
    *out = modem;
    return KAL_OK;
}

void kal_modem_free(struct kal_modem *modem) {
    if (modem == NULL) {
        return;
    }
    if (modem->instance >= 0) {
        ggwave_free(modem->instance);
        modem->instance = -1;
    }
    free(modem->chunk);
    free(modem->encode_scratch);
    free(modem);
}

size_t kal_modem_frame_samples(const struct kal_modem *modem) {
    return modem == NULL ? 0u : modem->frame_samples;
}

uint64_t kal_modem_frame_airtime_ms(const struct kal_modem *modem) {
    return modem == NULL ? 0u : modem->frame_airtime_ms;
}

#ifdef KAL_ENABLE_TEST_MODEM
static const int16_t kal_test_sync[KAL_TEST_SYNC_SAMPLES] = {
    (int16_t)-32768, (int16_t)32767, (int16_t)-32768, (int16_t)32767
};

static kal_result test_encode(struct kal_modem *modem,
                              const uint8_t frame[KAL_FRAME_BYTES],
                              int16_t *pcm, size_t capacity,
                              size_t *samples_written) {
    size_t index;
    (void)modem;
    if (capacity < KAL_TEST_FRAME_SAMPLES) {
        return KAL_ERR_INVALID;
    }
    memcpy(pcm, kal_test_sync, sizeof kal_test_sync);
    for (index = 0u; index < KAL_FRAME_BYTES; ++index) {
        pcm[KAL_TEST_SYNC_SAMPLES + index] = (int16_t)frame[index];
    }
    *samples_written = KAL_TEST_FRAME_SAMPLES;
    return KAL_OK;
}

/* One sample at a time through a sliding window, so a test may cut, reorder
 * or corrupt the stream at any sample boundary. */
static int test_feed(struct kal_modem *modem, const int16_t *pcm,
                     size_t sample_count, size_t *consumed,
                     uint8_t frame_out[KAL_FRAME_BYTES]) {
    size_t index;
    if (sample_count == 0u) {
        *consumed = 0u;
        return 0;
    }
    if (modem->window_fill == KAL_TEST_FRAME_SAMPLES) {
        memmove(modem->window, modem->window + 1,
                (KAL_TEST_FRAME_SAMPLES - 1u) * sizeof(int16_t));
        modem->window_fill = KAL_TEST_FRAME_SAMPLES - 1u;
    }
    modem->window[modem->window_fill++] = pcm[0];
    *consumed = 1u;
    if (modem->window_fill < KAL_TEST_FRAME_SAMPLES) {
        return 0;
    }
    if (memcmp(modem->window, kal_test_sync, sizeof kal_test_sync) != 0) {
        return 0;
    }
    for (index = 0u; index < KAL_FRAME_BYTES; ++index) {
        const int16_t sample = modem->window[KAL_TEST_SYNC_SAMPLES + index];
        if (sample < 0 || sample > 255) {
            return 0;
        }
        frame_out[index] = (uint8_t)sample;
    }
    modem->window_fill = 0u;
    return 1;
}
#endif /* KAL_ENABLE_TEST_MODEM */

kal_result kal_modem_encode(struct kal_modem *modem,
                            const uint8_t frame[KAL_FRAME_BYTES],
                            int16_t *pcm, size_t capacity,
                            size_t *samples_written) {
    int encoded_bytes;

    if (modem == NULL || frame == NULL || pcm == NULL || samples_written == NULL) {
        return KAL_ERR_INVALID;
    }
    *samples_written = 0u;
#ifdef KAL_ENABLE_TEST_MODEM
    if (modem->backend == KAL_MODEM_TEST_PASSTHROUGH) {
        return test_encode(modem, frame, pcm, capacity, samples_written);
    }
#endif
    if (capacity < modem->frame_samples) {
        return KAL_ERR_INVALID;
    }
    encoded_bytes = ggwave_encode(modem->instance, frame, (int)KAL_FRAME_BYTES,
                                  (ggwave_ProtocolId)modem->protocol_id,
                                  KAL_GGWAVE_VOLUME, modem->encode_scratch, 0);
    if (encoded_bytes <= 0
            || (size_t)encoded_bytes != modem->frame_samples * 2u) {
        return KAL_ERR_MODEM;
    }
    memcpy(pcm, modem->encode_scratch, (size_t)encoded_bytes);
    *samples_written = modem->frame_samples;
    return KAL_OK;
}

int kal_modem_rx_feed(struct kal_modem *modem, const int16_t *pcm,
                      size_t sample_count, size_t *consumed,
                      uint8_t frame_out[KAL_FRAME_BYTES]) {
    size_t take;
    int decoded;

    if (modem == NULL || pcm == NULL || consumed == NULL || frame_out == NULL) {
        return -1;
    }
    *consumed = 0u;
#ifdef KAL_ENABLE_TEST_MODEM
    if (modem->backend == KAL_MODEM_TEST_PASSTHROUGH) {
        return test_feed(modem, pcm, sample_count, consumed, frame_out);
    }
#endif
    if (sample_count == 0u) {
        return 0;
    }
    take = modem->chunk_samples - modem->chunk_fill;
    if (take > sample_count) {
        take = sample_count;
    }
    memcpy(modem->chunk + modem->chunk_fill, pcm, take * sizeof(int16_t));
    modem->chunk_fill += take;
    *consumed = take;
    if (modem->chunk_fill < modem->chunk_samples) {
        return 0;
    }
    modem->chunk_fill = 0u;
    decoded = ggwave_ndecode(modem->instance, modem->chunk,
                             (int)(modem->chunk_samples * 2u), frame_out,
                             (int)KAL_FRAME_BYTES);
    if (decoded == (int)KAL_FRAME_BYTES) {
        return 1;
    }
    /* -1 is ordinary noise, not a fault; -2 cannot occur at this fixed size. */
    return 0;
}

int kal_modem_rx_busy(const struct kal_modem *modem) {
    if (modem == NULL || modem->instance < 0) {
        return 0;
    }
    return ggwave_rxDurationFrames(modem->instance) > 0 ? 1 : 0;
}

void kal_modem_rx_reset(struct kal_modem *modem) {
    if (modem == NULL) {
        return;
    }
    modem->chunk_fill = 0u;
    modem->window_fill = 0u;
    if (modem->chunk != NULL) {
        memset(modem->chunk, 0, modem->chunk_samples * sizeof(int16_t));
    }
    memset(modem->window, 0, sizeof modem->window);
}
