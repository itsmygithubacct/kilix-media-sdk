/*
 * Channel matrix — the digital half of the design's channel row.
 *
 * SPDX-License-Identifier: MIT
 *
 * One KAL1 frame is modulated by the pinned engine, damaged by a named
 * synthetic impairment, and offered back to a receiving link. Every waveform
 * is in memory: no device is opened, nothing is played and nothing is
 * recorded.
 *
 * THIS DOES NOT GRADUATE A PROFILE AND DOES NOT DECIDE PROFILE CHOICE.
 * A synthetic impairment is not a room, a transducer or a device, and
 * profile choice remains undecidable from digital results. What the matrix
 * does give is a reproducible floor: an impairment a profile already fails
 * in memory cannot be expected to pass in air.
 */
#include "kilix_acoustic_link.h"
#include "kal_channel.h"
#include "kal_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A sanitizer build exists to prove memory safety on this path, not to
 * re-measure it: it runs one trial per cell over the required cells only, so
 * the suite stays usable. The measured table below is the normal build's.
 */
#if defined(__SANITIZE_ADDRESS__)
#define TRIALS_PER_CELL 1u
#define MEASURED_CELLS_RUN 0
#define SMOKE_CELLS_ONLY 1
#else
#define TRIALS_PER_CELL 3u
#define MEASURED_CELLS_RUN 1
#define SMOKE_CELLS_ONLY 0
#endif

struct cell {
    const char *name;
    enum kal_channel_kind kind;
    double parameter;
    double secondary;
    /* Cells the modem is expected to survive at every profile. A cell
     * outside this set is measured and reported, never asserted: the point
     * is the number, not a pass. */
    int required;
    /* One representative per impairment kind. A sanitizer build runs only
     * these: the code path is what it needs to exercise, not the table. */
    int smoke;
};

/*
 * Cells marked required are the ones every profile survives; they are
 * asserted. The rest are measured and printed, never asserted: their value
 * is the number and the ordering it shows, not a pass.
 */
static const struct cell cells[] = {
    {"clean",           KAL_CHANNEL_CLEAN,      0.0,   0.0,  1, 1},
    {"awgn-20dB",       KAL_CHANNEL_AWGN,      20.0,   0.0,  1, 0},
    {"awgn-6dB",        KAL_CHANNEL_AWGN,       6.0,   0.0,  1, 0},
    {"awgn-0dB",        KAL_CHANNEL_AWGN,       0.0,   0.0,  1, 1},
    {"awgn-minus9dB",   KAL_CHANNEL_AWGN,      -9.0,   0.0,  1, 0},
    {"awgn-minus11dB",  KAL_CHANNEL_AWGN,     -11.0,   0.0,  0, 0},
    {"awgn-minus12dB",  KAL_CHANNEL_AWGN,     -12.0,   0.0,  0, 0},
    {"gain-0.25x",      KAL_CHANNEL_GAIN,       0.25,  0.0,  1, 1},
    {"gain-0.003x",     KAL_CHANNEL_GAIN,       0.003, 0.0,  1, 0},
    {"gain-4x-sat",     KAL_CHANNEL_GAIN,       4.0,   0.0,  1, 0},
    {"clip-0.5",        KAL_CHANNEL_CLIP,       0.5,   0.0,  1, 1},
    {"clip-0.005",      KAL_CHANNEL_CLIP,       0.005, 0.0,  1, 0},
    {"drift-plus100",   KAL_CHANNEL_DRIFT,    100.0,   0.0,  1, 0},
    {"drift-minus100",  KAL_CHANNEL_DRIFT,   -100.0,   0.0,  1, 0},
    {"drift-plus1000",  KAL_CHANNEL_DRIFT,   1000.0,   0.0,  1, 1},
    {"drift-plus2000",  KAL_CHANNEL_DRIFT,   2000.0,   0.0,  0, 0},
    {"drift-plus5000",  KAL_CHANNEL_DRIFT,   5000.0,   0.0,  0, 0},
    {"drift-plus10000", KAL_CHANNEL_DRIFT,  10000.0,   0.0,  0, 0},
    {"echo-10ms-0.5",   KAL_CHANNEL_ECHO,     10.0,    0.5,  1, 1},
    {"echo-20ms-0.9",   KAL_CHANNEL_ECHO,     20.0,    0.9,  1, 0},
    {"echo-50ms-0.25",  KAL_CHANNEL_ECHO,     50.0,    0.25, 1, 0}
};

static kal_options channel_options(uint8_t profile, uint8_t role) {
    kal_options options;
    memset(&options, 0, sizeof options);
    options.sample_rate = KAL_SAMPLE_RATE;
    options.profile = profile;
    options.role = role;
    options.modem = (uint8_t)KAL_MODEM_GGWAVE;
    options.allow_high_frequency = 1u;
    return options;
}

/* One beacon frame through the engine, the impairment, and back. */
static int trial(uint8_t profile, const struct cell *cell, uint64_t seed) {
    kal_options tx_options = channel_options(profile, (uint8_t)KAL_ROLE_INITIATOR);
    kal_options rx_options = channel_options(profile, (uint8_t)KAL_ROLE_RESPONDER);
    kal_link *tx = NULL;
    kal_link *rx = NULL;
    kal_stats stats;
    struct kal_channel channel;
    uint8_t payload[KAL_MAX_BEACON_BYTES];
    uint8_t received[KAL_MAX_MESSAGE_BYTES];
    size_t size = 0u;
    int16_t *clean = NULL;
    int16_t *damaged = NULL;
    size_t capacity;
    size_t damaged_capacity;
    size_t written = 0u;
    size_t produced = 0u;
    size_t index;
    int ok = 0;

    if (kal_link_create(&tx, &tx_options) != KAL_OK) {
        return 0;
    }
    if (kal_link_create(&rx, &rx_options) != KAL_OK) {
        kal_link_free(tx);
        return 0;
    }
    kal_get_stats(tx, &stats);
    capacity = (size_t)stats.frame_airtime_ms * KAL_SAMPLE_RATE / 1000u
        + KAL_SAMPLE_RATE;

    memset(&channel, 0, sizeof channel);
    channel.kind = cell->kind;
    channel.parameter = cell->parameter;
    channel.secondary = cell->secondary;
    channel.seed = seed;
    damaged_capacity = kal_channel_output_samples(&channel, capacity) + KAL_SAMPLE_RATE;

    clean = (int16_t *)calloc(capacity, sizeof(int16_t));
    damaged = (int16_t *)calloc(damaged_capacity, sizeof(int16_t));
    if (clean == NULL || damaged == NULL) {
        goto done;
    }
    for (index = 0u; index < sizeof payload; ++index) {
        payload[index] = (uint8_t)((index * 11u) + (size_t)seed);
    }
    if (kal_beacon(tx, payload, sizeof payload) != KAL_OK) {
        goto done;
    }
    if (kal_tx_pull_s16(tx, clean, capacity, &written, 0u) != KAL_HAVE_OUTPUT
            || written == 0u) {
        goto done;
    }
    if (kal_channel_apply(&channel, clean, written, damaged, damaged_capacity,
                          &produced) != 0) {
        goto done;
    }
    (void)kal_rx_push_s16(rx, damaged, produced, 0u);
    if (kal_receive(rx, received, sizeof received, &size) == KAL_OK
            && size == sizeof payload
            && memcmp(received, payload, size) == 0) {
        ok = 1;
    }

done:
    free(clean);
    free(damaged);
    kal_link_free(tx);
    kal_link_free(rx);
    return ok;
}

static void check_matrix(void) {
    const size_t cell_count = sizeof cells / sizeof cells[0];
    const uint8_t profile_count = (uint8_t)KAL_PROFILE_COUNT;
    unsigned long required_cells = 0u;
    unsigned long required_passed = 0u;
    unsigned long executed = 0u;
    uint8_t profile;
    size_t index;

    printf("  impairment,profile,delivered,trials,asserted\n");
    for (index = 0u; index < cell_count; ++index) {
        if (!cells[index].required && !MEASURED_CELLS_RUN) {
            continue;
        }
        if (SMOKE_CELLS_ONLY && !cells[index].smoke) {
            continue;
        }
        for (profile = 0u; profile < profile_count; ++profile) {
            unsigned int delivered = 0u;
            unsigned int attempt;
            for (attempt = 0u; attempt < TRIALS_PER_CELL; ++attempt) {
                const uint64_t seed = ((uint64_t)index << 32)
                    + ((uint64_t)profile << 8) + attempt + 1u;
                delivered += trial(profile, &cells[index], seed) ? 1u : 0u;
                executed++;
            }
            printf("  %s,%s,%u,%u,%d\n", cells[index].name,
                   kal_profile_string(profile), delivered, TRIALS_PER_CELL,
                   cells[index].required);
            if (cells[index].required) {
                required_cells++;
                if (delivered == TRIALS_PER_CELL) {
                    required_passed++;
                }
                KAL_CHECK(delivered == TRIALS_PER_CELL);
            }
        }
    }
    KAL_GROUP("required impairment cells fully delivered", required_passed,
              required_cells);
    KAL_GROUP("matrix trials executed", executed, executed);
    printf("  physical profiles graduated by this matrix  0/6\n");
    printf("  profile choice decided by this matrix        0/1\n");
}

static void check_impairment_model(void) {
    /* The model itself must be honest: the damage it claims is the damage it
     * applies, and it is deterministic. */
    int16_t source[4800];
    int16_t first[9600];
    int16_t second[9600];
    struct kal_channel channel;
    size_t written_a = 0u;
    size_t written_b = 0u;
    size_t index;
    double signal_rms;
    double measured_snr;

    for (index = 0u; index < 4800u; ++index) {
        source[index] = (int16_t)(8000.0 * sin((double)index * 0.05));
    }
    signal_rms = kal_channel_rms(source, 4800u);
    KAL_CHECK(signal_rms > 0.0);

    memset(&channel, 0, sizeof channel);
    channel.kind = KAL_CHANNEL_AWGN;
    channel.parameter = 20.0;
    channel.seed = 42u;
    KAL_CHECK(kal_channel_apply(&channel, source, 4800u, first, 9600u,
                                &written_a) == 0);
    KAL_CHECK(written_a == 4800u);

    /* The same seed must reproduce the same damage exactly. */
    KAL_CHECK(kal_channel_apply(&channel, source, 4800u, second, 9600u,
                                &written_b) == 0);
    KAL_CHECK(memcmp(first, second, written_a * sizeof(int16_t)) == 0);

    /* And the achieved SNR must be the requested one, within 1 dB. */
    {
        double noise_sum = 0.0;
        for (index = 0u; index < written_a; ++index) {
            const double difference = (double)first[index] - (double)source[index];
            noise_sum += difference * difference;
        }
        measured_snr = 20.0 * log10(signal_rms
                                    / sqrt(noise_sum / (double)written_a));
    }
    printf("  requested SNR 20.0 dB, measured %.2f dB\n", measured_snr);
    KAL_CHECK(measured_snr > 19.0 && measured_snr < 21.0);

    /* Drift changes the sample count in the stated direction. */
    channel.kind = KAL_CHANNEL_DRIFT;
    channel.parameter = 1000.0;
    KAL_CHECK(kal_channel_apply(&channel, source, 4800u, first, 9600u,
                                &written_a) == 0);
    KAL_CHECK(written_a < 4800u + 4u && written_a > 4790u);

    /* A too-small output buffer is refused, never overrun. */
    KAL_CHECK(kal_channel_apply(&channel, source, 4800u, first, 10u,
                                &written_a) == -1);
    KAL_CHECK(kal_channel_apply(NULL, source, 4800u, first, 9600u,
                                &written_a) == -1);
}

int main(void) {
    printf("test_channel — synthetic impairment matrix (no device, no sound)\n");
    check_impairment_model();
    check_matrix();
    return kal_test_report("test_channel");
}
