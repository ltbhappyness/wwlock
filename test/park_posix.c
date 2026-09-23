/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "park_posix.h"

static int posix_wait(uint32_t *word, uint32_t expect, void *user)
{
    posix_park_t *park = (posix_park_t *)user;

    pthread_mutex_lock(&park->mu);
    while (atomic_load_explicit((_Atomic uint32_t *)word, memory_order_acquire) == expect) {
        pthread_cond_wait(&park->cv, &park->mu);
    }
    pthread_mutex_unlock(&park->mu);
    atomic_fetch_add_explicit(&park->waits, 1, memory_order_relaxed);
    return 0;
}

static int posix_wake(uint32_t *word, int n, void *user)
{
    posix_park_t *park = (posix_park_t *)user;

    (void)word;
    (void)n;
    pthread_mutex_lock(&park->mu);
    pthread_cond_broadcast(&park->cv);
    pthread_mutex_unlock(&park->mu);
    return 0;
}

void posix_park_init(posix_park_t *park)
{
    pthread_mutex_init(&park->mu, NULL);
    pthread_cond_init(&park->cv, NULL);
    atomic_store(&park->waits, 0);
}

void posix_park_fini(posix_park_t *park)
{
    pthread_cond_destroy(&park->cv);
    pthread_mutex_destroy(&park->mu);
}

void posix_park_export(posix_park_t *park, wwlock_park_t *out)
{
    out->wait = posix_wait;
    out->wake = posix_wake;
    out->user = park;
}

unsigned posix_park_waits(posix_park_t *park)
{
    return atomic_load_explicit(&park->waits, memory_order_relaxed);
}
