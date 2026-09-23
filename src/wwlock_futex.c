/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "wwlock_futex.h"

#if defined(__linux__)

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

static int futex_wait(uint32_t *word, uint32_t expect, void *user)
{
    (void)user;
    syscall(SYS_futex, word, (int)(FUTEX_WAIT | FUTEX_PRIVATE_FLAG), expect, NULL, NULL, 0);
    return 0;
}

static int futex_wake(uint32_t *word, int n, void *user)
{
    (void)user;
    if (n < 1) {
        n = WWLOCK_WAKE_ALL;
    }
    syscall(SYS_futex, word, (int)(FUTEX_WAKE | FUTEX_PRIVATE_FLAG), n, NULL, NULL, 0);
    return 0;
}

int wwlock_futex_fill(wwlock_park_t *park)
{
    if (park == NULL) {
        return WWLOCK_INVAL;
    }
    park->wait = futex_wait;
    park->wake = futex_wake;
    park->user = NULL;
    return WWLOCK_OK;
}

#else

int wwlock_futex_fill(wwlock_park_t *park)
{
    (void)park;
    return WWLOCK_INVAL;
}

#endif
