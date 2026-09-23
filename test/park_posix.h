/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WWLOCK_PARK_POSIX_H
#define WWLOCK_PARK_POSIX_H

#include "wwlock.h"

#include <pthread.h>
#include <stdatomic.h>

/**
 * @brief pthread condition variable behind the wait and wake callbacks.
 *
 * One park may be shared by every lock in a class. Wake broadcasts. Each
 * sleeper rechecks its own sequence word.
 */
typedef struct posix_park {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    atomic_uint waits;
} posix_park_t;

/** @brief Prepare the mutex and the condition variable. */
void posix_park_init(posix_park_t *park);

/** @brief Destroy the mutex and the condition variable. */
void posix_park_fini(posix_park_t *park);

/** @brief Fill callbacks that use @p park. */
void posix_park_export(posix_park_t *park, wwlock_park_t *out);

/** @brief How many times the wait callback has returned. */
unsigned posix_park_waits(posix_park_t *park);

#endif
