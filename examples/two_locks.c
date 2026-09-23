/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Two threads lock two locks in opposite order.
 */
#include "park_posix.h"

#include <pthread.h>
#include <stdio.h>

static wwlock_class_t *g_class;
static wwlock_t *g_a;
static wwlock_t *g_b;

struct job {
    wwlock_t *first;
    wwlock_t *second;
};

static int lock_both(wwlock_t *first, wwlock_t *second)
{
    for (;;) {
        int rc = wwlock_wrlock(first);

        if (rc == WWLOCK_DIE) {
            continue;
        }
        if (rc != WWLOCK_OK && rc != WWLOCK_ALREADY) {
            return rc;
        }
        rc = wwlock_wrlock(second);
        if (rc == WWLOCK_DIE) {
            continue;
        }
        if (rc != WWLOCK_OK && rc != WWLOCK_ALREADY) {
            return rc;
        }
        return WWLOCK_OK;
    }
}

static void *worker(void *raw)
{
    struct job *job = (struct job *)raw;

    if (lock_both(job->first, job->second) != WWLOCK_OK) {
        return (void *)1;
    }
    wwlock_unlock(job->second);
    wwlock_unlock(job->first);
    return NULL;
}

int main(void)
{
    posix_park_t park_state;
    wwlock_park_t park;
    struct job jobs[2];
    pthread_t threads[2];
    int i;

    posix_park_init(&park_state);
    posix_park_export(&park_state, &park);
    if (wwlock_class_init(&g_class, &park) != WWLOCK_OK) {
        return 1;
    }
    if (wwlock_init(&g_a, g_class) != WWLOCK_OK || wwlock_init(&g_b, g_class) != WWLOCK_OK) {
        return 1;
    }
    jobs[0].first = g_a;
    jobs[0].second = g_b;
    jobs[1].first = g_b;
    jobs[1].second = g_a;
    for (i = 0; i < 2; i++) {
        pthread_create(&threads[i], NULL, worker, &jobs[i]);
    }
    for (i = 0; i < 2; i++) {
        void *status = NULL;

        pthread_join(threads[i], &status);
        if (status != NULL) {
            fprintf(stderr, "worker failed\n");
            return 1;
        }
    }
    wwlock_deinit(g_a);
    wwlock_deinit(g_b);
    wwlock_class_deinit(g_class);
    posix_park_fini(&park_state);
    printf("two locks acquired in opposite order\n");
    return 0;
}
