/* CSPRNG. SPDX-License-Identifier: MIT */
#include "kal_random.h"

#include <errno.h>
#include <stdio.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

int kal_random_bytes(void *buffer, size_t size) {
    unsigned char *out = (unsigned char *)buffer;
    size_t filled = 0u;

    if (buffer == NULL || size == 0u) {
        return -1;
    }
#if defined(__linux__)
    while (filled < size) {
        const ssize_t got = getrandom(out + filled, size - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        filled += (size_t)got;
    }
    if (filled == size) {
        return 0;
    }
#endif
    /* Fall back to the kernel device, then fail closed. */
    {
        FILE *source = fopen("/dev/urandom", "rb");
        if (source == NULL) {
            return -1;
        }
        while (filled < size) {
            const size_t got = fread(out + filled, 1u, size - filled, source);
            if (got == 0u) {
                (void)fclose(source);
                return -1;
            }
            filled += got;
        }
        (void)fclose(source);
    }
    return 0;
}
