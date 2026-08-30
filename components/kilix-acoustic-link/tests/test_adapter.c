/*
 * Audio child adapter lifecycle.
 *
 * SPDX-License-Identifier: MIT
 *
 * Every child here is an ordinary system binary standing in for a capture or
 * playback backend. No audio device is opened and no sound is produced: the
 * property under test is process and pipe lifetime, not audio.
 */
#include "kal_audio.h"
#include "kal_test.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned long open_descriptors(void) {
    DIR *directory = opendir("/proc/self/fd");
    unsigned long count = 0u;
    struct dirent *entry;
    if (directory == NULL) {
        return 0u;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    (void)closedir(directory);
    return count;
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000)
        + (uint64_t)(now.tv_nsec / 1000000);
}

static void check_capture_child(void) {
    static const char *const argv[] = {"/bin/echo", "kal-adapter-probe", NULL};
    struct kal_audio_child *child = NULL;
    char buffer[64];
    ssize_t got;
    int status = 0;

    KAL_CHECK(kal_audio_spawn(argv, KAL_AUDIO_CAPTURE, &child) == 0);
    KAL_CHECK(child != NULL);
    if (child == NULL) {
        return;
    }
    KAL_CHECK(kal_audio_pid(child) > 0);
    memset(buffer, 0, sizeof buffer);
    got = kal_audio_read(child, buffer, sizeof buffer - 1u);
    KAL_CHECK(got > 0);
    KAL_CHECK(strncmp(buffer, "kal-adapter-probe", 17u) == 0);
    KAL_CHECK(kal_audio_close(child, 500u, &status) == 0);
    KAL_CHECK(WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0);
}

static void check_playback_child(void) {
    static const char *const argv[] = {"/bin/dd", "of=/dev/null", "status=none", NULL};
    struct kal_audio_child *child = NULL;
    unsigned char block[4096];
    int status = 0;

    memset(block, 0x41, sizeof block);
    KAL_CHECK(kal_audio_spawn(argv, KAL_AUDIO_PLAYBACK, &child) == 0);
    if (child == NULL) {
        return;
    }
    KAL_CHECK(kal_audio_write(child, block, sizeof block) == (ssize_t)sizeof block);
    KAL_CHECK(kal_audio_close(child, 1000u, &status) == 0);
    KAL_CHECK(WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0);
}

static void check_missing_backend(void) {
    static const char *const argv[] = {"/nonexistent/kal-no-such-backend", NULL};
    struct kal_audio_child *child = NULL;
    char buffer[16];
    int status = 0;

    /* A backend that is not installed must surface as a clean failure. */
    KAL_CHECK(kal_audio_spawn(argv, KAL_AUDIO_CAPTURE, &child) == 0);
    if (child == NULL) {
        return;
    }
    KAL_CHECK(kal_audio_read(child, buffer, sizeof buffer) == 0);
    KAL_CHECK(kal_audio_close(child, 500u, &status) == 0);
    KAL_CHECK(WIFEXITED(status) != 0 && WEXITSTATUS(status) == 127);
}

static void check_failing_child(void) {
    static const char *const argv[] = {"/bin/false", NULL};
    struct kal_audio_child *child = NULL;
    int status = 0;

    KAL_CHECK(kal_audio_spawn(argv, KAL_AUDIO_CAPTURE, &child) == 0);
    if (child == NULL) {
        return;
    }
    KAL_CHECK(kal_audio_close(child, 500u, &status) == 0);
    KAL_CHECK(WIFEXITED(status) != 0 && WEXITSTATUS(status) == 1);
}

static void check_bounded_teardown(void) {
    /* A backend that ignores its closed pipe must still be terminated and
     * reaped inside the caller's bound. */
    static const char *const argv[] = {"/bin/sleep", "30", NULL};
    struct kal_audio_child *child = NULL;
    uint64_t started;
    uint64_t elapsed;
    int status = 0;

    KAL_CHECK(kal_audio_spawn(argv, KAL_AUDIO_CAPTURE, &child) == 0);
    if (child == NULL) {
        return;
    }
    started = monotonic_ms();
    KAL_CHECK(kal_audio_close(child, 200u, &status) == 0);
    elapsed = monotonic_ms() - started;
    KAL_CHECK(elapsed < UINT64_C(3000));
    KAL_CHECK(WIFSIGNALED(status) != 0 || WIFEXITED(status) != 0);
    printf("  bounded teardown of an unresponsive child  %llums (bound 3000ms)\n",
           (unsigned long long)elapsed);
}

static void check_repeated_sessions(void) {
    static const char *const argv[] = {"/bin/true", NULL};
    const unsigned int rounds = 200u;
    unsigned long before;
    unsigned long after;
    unsigned int index;
    unsigned int reaped = 0u;

    before = open_descriptors();
    for (index = 0u; index < rounds; ++index) {
        struct kal_audio_child *child = NULL;
        int status = 0;
        if (kal_audio_spawn(argv, KAL_AUDIO_CAPTURE, &child) != 0) {
            continue;
        }
        if (kal_audio_close(child, 500u, &status) == 0) {
            reaped++;
        }
    }
    after = open_descriptors();
    KAL_GROUP("repeated adapter sessions reaped", reaped, rounds);
    KAL_CHECK(reaped == rounds);
    /* No descriptor is left behind by a session. */
    KAL_CHECK(after == before);
    KAL_GROUP("descriptors leaked over 200 sessions", after - before, 0u);
}

static void check_backend_argv_is_fixed(void) {
    const char *name = NULL;
    const char *const *argv =
        kal_audio_backend_argv(KAL_AUDIO_CAPTURE, 48000u, NULL, &name);
    size_t count = 0u;
    size_t clean = 0u;

    KAL_CHECK(name != NULL);
    if (argv == NULL) {
        /* No backend on this host is a legitimate result, not a failure. */
        printf("  capture backend on this host              none\n");
        KAL_CHECK(strcmp(name, "none") == 0);
        return;
    }
    while (argv[count] != NULL) {
        const char *argument = argv[count];
        /* A fixed argv never needs shell quoting; the presence of a shell
         * metacharacter would mean a string was being assembled. */
        if (strpbrk(argument, ";|&$`<>()\\\"'*?") == NULL) {
            clean++;
        }
        count++;
    }
    printf("  capture backend on this host              %s\n", name);
    KAL_GROUP("backend argv elements free of shell syntax", clean, count);
    KAL_CHECK(count >= 1u);
    KAL_CHECK(clean == count);
}

int main(void) {
    printf("test_adapter — audio child lifecycle (no device opened)\n");
    check_capture_child();
    check_playback_child();
    check_missing_backend();
    check_failing_child();
    check_bounded_teardown();
    check_repeated_sessions();
    check_backend_argv_is_fixed();
    return kal_test_report("test_adapter");
}
