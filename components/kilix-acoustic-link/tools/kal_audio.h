/*
 * Long-lived capture/playback child adapter for the diagnostic tool.
 *
 * SPDX-License-Identifier: MIT
 *
 * This lives outside the library core: the library itself never opens a
 * device or spawns a process. Every child is launched with a fixed argv and
 * no shell, its pipes are closed on teardown, and it is terminated with a
 * bounded wait and then reaped.
 */
#ifndef KAL_AUDIO_H
#define KAL_AUDIO_H

#include <stddef.h>
#include <sys/types.h>

struct kal_audio_child;

enum kal_audio_direction {
    KAL_AUDIO_CAPTURE = 0,   /* read the child's stdout  */
    KAL_AUDIO_PLAYBACK = 1   /* write to the child's stdin */
};

/*
 * Spawn a child. argv must be NULL-terminated and is passed verbatim to
 * execvp; no string is ever handed to a shell. Returns 0 on success.
 */
int kal_audio_spawn(const char *const argv[], int direction,
                    struct kal_audio_child **out);

/* Read captured bytes. Returns bytes read, 0 at end of stream, -1 on error. */
ssize_t kal_audio_read(struct kal_audio_child *child, void *buffer, size_t size);

/* Write playback bytes. Returns bytes written or -1. */
ssize_t kal_audio_write(struct kal_audio_child *child, const void *buffer,
                        size_t size);

/*
 * Close pipes, terminate the child with a bounded wait and reap it.
 * *exit_status receives the child's wait status when it is not NULL.
 * Returns 0 when the child was reaped.
 */
int kal_audio_close(struct kal_audio_child *child, unsigned int timeout_ms,
                    int *exit_status);

/* Diagnostics: the pid, or -1 once the child has been reaped. */
pid_t kal_audio_pid(const struct kal_audio_child *child);

/*
 * Backend selection for the first Linux implementation, in the documented
 * order PipeWire, PulseAudio, ALSA, then SoX as an explicit diagnostic
 * backend. Returns a NULL-terminated argv owned by the callee, or NULL when
 * no backend is present. rate/channels are formatted into the argv.
 */
const char *const *kal_audio_backend_argv(int direction, unsigned int rate,
                                          const char *device, const char **name);

#endif /* KAL_AUDIO_H */
