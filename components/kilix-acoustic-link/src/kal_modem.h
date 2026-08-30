/*
 * Modem adapter — internal.
 *
 * SPDX-License-Identifier: MIT
 *
 * The link core talks only to this interface. No upstream modem type crosses
 * it, and nothing here opens a device, spawns a process or emits sound: it
 * converts between one 64-byte KAL1 frame and caller-owned PCM.
 */
#ifndef KAL_MODEM_H
#define KAL_MODEM_H

#include <stddef.h>
#include <stdint.h>

#include "kilix_acoustic_link.h"

struct kal_modem;

/*
 * Fix process-global engine configuration exactly once. Called from
 * kal_link_create before the first instance exists and never again while
 * instances run.
 */
void kal_modem_global_init(void);

kal_result kal_modem_create(struct kal_modem **out, uint8_t backend,
                            uint8_t profile, uint32_t sample_rate);
void kal_modem_free(struct kal_modem *modem);

/* Samples one encoded KAL1 frame occupies. Constant for the lifetime. */
size_t kal_modem_frame_samples(const struct kal_modem *modem);

/* Airtime of one frame in milliseconds at the operating sample rate. */
uint64_t kal_modem_frame_airtime_ms(const struct kal_modem *modem);

/*
 * Modulate one frame. Writes exactly kal_modem_frame_samples() samples.
 * Performs no allocation: the buffer was sized at create.
 */
kal_result kal_modem_encode(struct kal_modem *modem,
                            const uint8_t frame[KAL_FRAME_BYTES],
                            int16_t *pcm, size_t capacity,
                            size_t *samples_written);

/*
 * Consume PCM up to the next internal chunk boundary.
 *
 * *consumed receives the number of samples taken from pcm. The return is 1
 * when frame_out holds a decoded 64-byte frame, 0 when more input is needed,
 * and negative on a hard modem error. Decoding never grows
 * memory: the chunk buffer is fixed at create.
 */
int kal_modem_rx_feed(struct kal_modem *modem, const int16_t *pcm,
                      size_t sample_count, size_t *consumed,
                      uint8_t frame_out[KAL_FRAME_BYTES]);

/* Best-effort carrier sense: non-zero while a decode is in progress. */
int kal_modem_rx_busy(const struct kal_modem *modem);

/* Drop any partially accumulated chunk. */
void kal_modem_rx_reset(struct kal_modem *modem);

#endif /* KAL_MODEM_H */
