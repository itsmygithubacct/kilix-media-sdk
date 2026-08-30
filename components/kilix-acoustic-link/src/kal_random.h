/* CSPRNG. SPDX-License-Identifier: MIT */
#ifndef KAL_RANDOM_H
#define KAL_RANDOM_H

#include <stddef.h>
#include <stdint.h>

/*
 * Fill a buffer from the operating-system CSPRNG. Returns 0 on success and
 * -1 on failure; failure is fatal to the caller and is never substituted
 * with a timestamp, a hostname or rand().
 */
int kal_random_bytes(void *buffer, size_t size);

#endif /* KAL_RANDOM_H */
