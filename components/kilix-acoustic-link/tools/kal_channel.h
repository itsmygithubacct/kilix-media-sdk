/*
 * Synthetic channel impairments — diagnostic, outside the library core.
 *
 * SPDX-License-Identifier: MIT
 *
 * WHAT THIS IS: a deterministic, in-memory model that damages a waveform in
 * named, measurable ways so the modem's behaviour under damage can be
 * observed without a device.
 *
 * WHAT THIS IS NOT: a room, a transducer, a microphone, a loudspeaker, an
 * audio server, or any device at all. Nothing here graduates a physical
 * profile, and nothing here decides which profile a product should use.
 * Profile choice remains undecidable from digital results.
 */
#ifndef KAL_CHANNEL_H
#define KAL_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

enum kal_channel_kind {
    KAL_CHANNEL_CLEAN = 0,
    KAL_CHANNEL_AWGN = 1,       /* additive white Gaussian noise at a target SNR */
    KAL_CHANNEL_GAIN = 2,       /* linear gain, with saturation at full scale   */
    KAL_CHANNEL_CLIP = 3,       /* hard clipping at a fraction of full scale    */
    KAL_CHANNEL_DRIFT = 4,      /* sample-clock mismatch, in parts per million  */
    KAL_CHANNEL_ECHO = 5        /* one delayed, attenuated reflection           */
};

struct kal_channel {
    enum kal_channel_kind kind;
    double parameter;           /* SNR dB / gain / clip fraction / ppm / delay ms */
    double secondary;           /* echo attenuation; unused otherwise            */
    uint64_t seed;              /* deterministic: the same seed gives the same
                                   damage, so a cell is reproducible            */
};

/* Root-mean-square level of a block, in full-scale units. */
double kal_channel_rms(const int16_t *pcm, size_t sample_count);

/*
 * Apply the impairment. Writes at most `capacity` samples to `out` and
 * reports how many were written; drift changes the sample count, every other
 * kind preserves it. Returns 0 on success and -1 when the output does not
 * fit.
 */
int kal_channel_apply(const struct kal_channel *channel, const int16_t *in,
                      size_t sample_count, int16_t *out, size_t capacity,
                      size_t *written);

/* Output samples the impairment will produce for a given input length. */
size_t kal_channel_output_samples(const struct kal_channel *channel,
                                  size_t sample_count);

const char *kal_channel_kind_string(enum kal_channel_kind kind);

#endif /* KAL_CHANNEL_H */
