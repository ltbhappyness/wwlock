/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WWLOCK_FUTEX_H
#define WWLOCK_FUTEX_H

#include "wwlock.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill @p park with a per-word futex wait and wake.
 *
 * Each lock sleeps on its own sequence word. Wake wakes every sleeper on
 * that word. This is the park to use in a real process. The condition-variable
 * park under test/ is for tests and will not scale.
 *
 * @param[out] park Park object
 * @return WWLOCK_OK, or WWLOCK_INVAL when futex is not available.
 */
int wwlock_futex_fill(wwlock_park_t *park);

#ifdef __cplusplus
}
#endif

#endif
