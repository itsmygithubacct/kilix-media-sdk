/* Synthetic channel impairments. SPDX-License-Identifier: MIT */
#include "kal_channel.h"

#include <math.h>
#include <string.h>

#define KAL_FULL_SCALE 32767.0

/* Deterministic 64-bit LCG plus Box-Muller, so every cell reproduces. */
static double next_uniform(uint64_t *state) {
    *state = (*state * UINT64_C(6364136223846793005)) + UINT64_C(1442695040888963407);
    /* Use the high 53 bits and keep the value strictly inside (0, 1). */
    return (double)((*state >> 11) + 1u) / 9007199254740994.0;
}

static double next_gaussian(uint64_t *state) {
    const double u1 = next_uniform(state);
    const double u2 = next_uniform(state);
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

static int16_t saturate(double value) {
    if (value > KAL_FULL_SCALE) {
        return (int16_t)32767;
    }
    if (value < -KAL_FULL_SCALE - 1.0) {
        return (int16_t)-32768;
    }
    return (int16_t)lround(value);
}

double kal_channel_rms(const int16_t *pcm, size_t sample_count) {
    double sum = 0.0;
    size_t index;
    if (pcm == NULL || sample_count == 0u) {
        return 0.0;
    }
    for (index = 0u; index < sample_count; ++index) {
        const double sample = (double)pcm[index];
        sum += sample * sample;
    }
    return sqrt(sum / (double)sample_count);
}

size_t kal_channel_output_samples(const struct kal_channel *channel,
                                  size_t sample_count) {
    if (channel == NULL) {
        return sample_count;
    }
    if (channel->kind == KAL_CHANNEL_DRIFT) {
        const double ratio = 1.0 + (channel->parameter / 1.0e6);
        if (ratio <= 0.0) {
            return 0u;
        }
        /* One guard sample so rounding never truncates the last frame. */
        return (size_t)((double)sample_count / ratio) + 2u;
    }
    if (channel->kind == KAL_CHANNEL_ECHO) {
        return sample_count + (size_t)(channel->parameter * 48.0) + 1u;
    }
    return sample_count;
}

int kal_channel_apply(const struct kal_channel *channel, const int16_t *in,
                      size_t sample_count, int16_t *out, size_t capacity,
                      size_t *written) {
    size_t index;
    uint64_t state;

    if (channel == NULL || in == NULL || out == NULL || written == NULL) {
        return -1;
    }
    *written = 0u;
    state = channel->seed == 0u ? UINT64_C(0x9e3779b97f4a7c15) : channel->seed;

    switch (channel->kind) {
    case KAL_CHANNEL_CLEAN:
        if (capacity < sample_count) {
            return -1;
        }
        memcpy(out, in, sample_count * sizeof(int16_t));
        *written = sample_count;
        return 0;

    case KAL_CHANNEL_AWGN: {
        /* Noise power is set from the signal's own RMS, so the requested SNR
         * is a property of this waveform rather than of full scale. */
        const double signal_rms = kal_channel_rms(in, sample_count);
        const double noise_rms = signal_rms / pow(10.0, channel->parameter / 20.0);
        if (capacity < sample_count) {
            return -1;
        }
        for (index = 0u; index < sample_count; ++index) {
            out[index] = saturate((double)in[index]
                                  + (noise_rms * next_gaussian(&state)));
        }
        *written = sample_count;
        return 0;
    }

    case KAL_CHANNEL_GAIN:
        if (capacity < sample_count) {
            return -1;
        }
        for (index = 0u; index < sample_count; ++index) {
            out[index] = saturate((double)in[index] * channel->parameter);
        }
        *written = sample_count;
        return 0;

    case KAL_CHANNEL_CLIP: {
        const double limit = KAL_FULL_SCALE * channel->parameter;
        if (capacity < sample_count) {
            return -1;
        }
        for (index = 0u; index < sample_count; ++index) {
            double sample = (double)in[index];
            if (sample > limit) {
                sample = limit;
            } else if (sample < -limit) {
                sample = -limit;
            }
            out[index] = saturate(sample);
        }
        *written = sample_count;
        return 0;
    }

    case KAL_CHANNEL_DRIFT: {
        /* Two devices never share a sample clock exactly. Linear resampling
         * at (1 + ppm/1e6) models that mismatch. */
        const double ratio = 1.0 + (channel->parameter / 1.0e6);
        const size_t produced = kal_channel_output_samples(channel, sample_count);
        if (ratio <= 0.0 || capacity < produced) {
            return -1;
        }
        for (index = 0u; index < produced; ++index) {
            const double position = (double)index * ratio;
            const size_t base = (size_t)position;
            const double fraction = position - (double)base;
            double value;
            if (base + 1u < sample_count) {
                value = ((double)in[base] * (1.0 - fraction))
                    + ((double)in[base + 1u] * fraction);
            } else if (base < sample_count) {
                value = (double)in[base];
            } else {
                value = 0.0;
            }
            out[index] = saturate(value);
        }
        *written = produced;
        return 0;
    }

    case KAL_CHANNEL_ECHO: {
        /* One reflection: the crudest possible room, and deliberately so. A
         * real impulse response is a measurement, not a model. */
        const size_t delay = (size_t)(channel->parameter * 48.0);
        const size_t produced = sample_count + delay + 1u;
        if (capacity < produced) {
            return -1;
        }
        for (index = 0u; index < produced; ++index) {
            double value = index < sample_count ? (double)in[index] : 0.0;
            if (index >= delay && (index - delay) < sample_count) {
                value += channel->secondary * (double)in[index - delay];
            }
            out[index] = saturate(value);
        }
        *written = produced;
        return 0;
    }

    default:
        return -1;
    }
}

const char *kal_channel_kind_string(enum kal_channel_kind kind) {
    switch (kind) {
    case KAL_CHANNEL_CLEAN: return "clean";
    case KAL_CHANNEL_AWGN: return "awgn";
    case KAL_CHANNEL_GAIN: return "gain";
    case KAL_CHANNEL_CLIP: return "clip";
    case KAL_CHANNEL_DRIFT: return "drift";
    case KAL_CHANNEL_ECHO: return "echo";
    default: return "unknown";
    }
}
