/* Audio child adapter. SPDX-License-Identifier: MIT */
#include "kal_audio.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KAL_AUDIO_POLL_US 2000u

struct kal_audio_child {
    pid_t pid;
    int fd;          /* capture: read end; playback: write end */
    int direction;
    int reaped;
};

static int set_cloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int kal_audio_spawn(const char *const argv[], int direction,
                    struct kal_audio_child **out) {
    int pipefd[2];
    pid_t pid;
    struct kal_audio_child *child;

    if (argv == NULL || argv[0] == NULL || out == NULL) {
        return -1;
    }
    *out = NULL;
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: wire exactly one pipe end to the standard stream the
         * backend uses, then exec a fixed argv. No shell is involved. */
        if (direction == KAL_AUDIO_CAPTURE) {
            (void)close(pipefd[0]);
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                _exit(127);
            }
            (void)close(pipefd[1]);
        } else {
            (void)close(pipefd[1]);
            if (dup2(pipefd[0], STDIN_FILENO) < 0) {
                _exit(127);
            }
            (void)close(pipefd[0]);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    child = (struct kal_audio_child *)calloc(1u, sizeof *child);
    if (child == NULL) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    if (direction == KAL_AUDIO_CAPTURE) {
        (void)close(pipefd[1]);
        child->fd = pipefd[0];
    } else {
        (void)close(pipefd[0]);
        child->fd = pipefd[1];
    }
    (void)set_cloexec(child->fd);
    child->pid = pid;
    child->direction = direction;
    child->reaped = 0;
    *out = child;
    return 0;
}

ssize_t kal_audio_read(struct kal_audio_child *child, void *buffer, size_t size) {
    ssize_t got;
    if (child == NULL || buffer == NULL || child->direction != KAL_AUDIO_CAPTURE) {
        return -1;
    }
    do {
        got = read(child->fd, buffer, size);
    } while (got < 0 && errno == EINTR);
    return got;
}

ssize_t kal_audio_write(struct kal_audio_child *child, const void *buffer,
                        size_t size) {
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t written = 0u;

    if (child == NULL || buffer == NULL || child->direction != KAL_AUDIO_PLAYBACK) {
        return -1;
    }
    while (written < size) {
        const ssize_t put = write(child->fd, bytes + written, size - written);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)put;
    }
    return (ssize_t)written;
}

pid_t kal_audio_pid(const struct kal_audio_child *child) {
    if (child == NULL || child->reaped != 0) {
        return (pid_t)-1;
    }
    return child->pid;
}

int kal_audio_close(struct kal_audio_child *child, unsigned int timeout_ms,
                    int *exit_status) {
    unsigned int waited_us = 0u;
    const unsigned int limit_us = timeout_ms * 1000u;
    int status = 0;
    int reaped = 0;

    if (child == NULL) {
        return -1;
    }
    if (child->fd >= 0) {
        (void)close(child->fd);
        child->fd = -1;
    }
    if (child->reaped != 0) {
        free(child);
        return 0;
    }

    /* A closed pipe alone should end the child; escalate on a bounded wait
     * so teardown can never block the caller indefinitely. */
    while (waited_us < limit_us) {
        const pid_t result = waitpid(child->pid, &status, WNOHANG);
        if (result == child->pid) {
            reaped = 1;
            break;
        }
        if (result < 0) {
            break;
        }
        usleep(KAL_AUDIO_POLL_US);
        waited_us += KAL_AUDIO_POLL_US;
    }
    if (!reaped) {
        (void)kill(child->pid, SIGTERM);
        waited_us = 0u;
        while (waited_us < limit_us) {
            const pid_t result = waitpid(child->pid, &status, WNOHANG);
            if (result == child->pid) {
                reaped = 1;
                break;
            }
            if (result < 0) {
                break;
            }
            usleep(KAL_AUDIO_POLL_US);
            waited_us += KAL_AUDIO_POLL_US;
        }
    }
    if (!reaped) {
        (void)kill(child->pid, SIGKILL);
        if (waitpid(child->pid, &status, 0) == child->pid) {
            reaped = 1;
        }
    }
    child->reaped = 1;
    if (exit_status != NULL) {
        *exit_status = status;
    }
    free(child);
    return reaped ? 0 : -1;
}

/* ------------------------------------------------------------- backends */

static int have_binary(const char *name) {
    static const char *const roots[] = {"/usr/bin/", "/bin/", "/usr/local/bin/"};
    size_t index;
    for (index = 0u; index < sizeof roots / sizeof roots[0]; ++index) {
        char path[256];
        if (snprintf(path, sizeof path, "%s%s", roots[index], name) < 0) {
            continue;
        }
        if (access(path, X_OK) == 0) {
            return 1;
        }
    }
    return 0;
}

const char *const *kal_audio_backend_argv(int direction, unsigned int rate,
                                          const char *device, const char **name) {
    static char rate_text[16];
    static char rate_flag[32];
    static const char *argv[12];
    size_t count = 0u;

    (void)snprintf(rate_text, sizeof rate_text, "%u", rate);
    (void)snprintf(rate_flag, sizeof rate_flag, "--rate=%u", rate);

    /* Documented preference order: PipeWire, PulseAudio, ALSA, SoX. */
    if (have_binary("pw-cat")) {
        argv[count++] = "pw-cat";
        argv[count++] = direction == KAL_AUDIO_CAPTURE ? "--record" : "--playback";
        argv[count++] = "--rate";
        argv[count++] = rate_text;
        argv[count++] = "--channels";
        argv[count++] = "1";
        argv[count++] = "--format";
        argv[count++] = "s16";
        argv[count++] = "-";
        if (name != NULL) { *name = "pipewire"; }
    } else if (have_binary(direction == KAL_AUDIO_CAPTURE ? "parec" : "pacat")) {
        argv[count++] = direction == KAL_AUDIO_CAPTURE ? "parec" : "pacat";
        argv[count++] = "--format=s16le";
        argv[count++] = rate_flag;
        argv[count++] = "--channels=1";
        argv[count++] = "--raw";
        if (device != NULL) {
            argv[count++] = direction == KAL_AUDIO_CAPTURE ? "-d" : "-d";
            argv[count++] = device;
        }
        if (name != NULL) { *name = "pulseaudio"; }
    } else if (have_binary(direction == KAL_AUDIO_CAPTURE ? "arecord" : "aplay")) {
        argv[count++] = direction == KAL_AUDIO_CAPTURE ? "arecord" : "aplay";
        argv[count++] = "-q";
        argv[count++] = "-t";
        argv[count++] = "raw";
        argv[count++] = "-f";
        argv[count++] = "S16_LE";
        argv[count++] = "-c";
        argv[count++] = "1";
        argv[count++] = "-r";
        argv[count++] = rate_text;
        if (name != NULL) { *name = "alsa"; }
    } else if (have_binary("sox")) {
        /* Diagnostic backend only; never a default. */
        argv[count++] = "sox";
        argv[count++] = "-q";
        argv[count++] = direction == KAL_AUDIO_CAPTURE ? "-d" : "-t";
        argv[count++] = direction == KAL_AUDIO_CAPTURE ? "-t" : "raw";
        argv[count++] = direction == KAL_AUDIO_CAPTURE ? "raw" : "-r";
        argv[count++] = direction == KAL_AUDIO_CAPTURE ? "-" : rate_text;
        if (name != NULL) { *name = "sox"; }
    } else {
        if (name != NULL) { *name = "none"; }
        return NULL;
    }
    argv[count] = NULL;
    return argv;
}
