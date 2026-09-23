/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * One lock. Several readers at once. A writer waits for them.
 * A reader waits for a writer. The same thread may take the write lock twice.
 */
#include "park_posix.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static wwlock_class_t *g_class;
static wwlock_t *g_lock;

struct reader_job {
    int id;
    atomic_int *entered;
};

struct wait_job {
    int mode;
    atomic_int *holding;
    uint64_t elapsed_ns;
    int rc;
};

static uint64_t nsec_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *reader(void *raw)
{
    struct reader_job *job = (struct reader_job *)raw;
    int now;

    if (wwlock_rdlock(g_lock) != WWLOCK_OK) {
        return (void *)1;
    }
    now = atomic_fetch_add_explicit(job->entered, 1, memory_order_acq_rel) + 1;
    printf("reader %d entered, readers inside %d\n", job->id, now);
    while (atomic_load_explicit(job->entered, memory_order_acquire) < 3) {
        sched_yield();
    }
    wwlock_unlock(g_lock);
    return NULL;
}

static void *hold_then_wait(void *raw)
{
    struct wait_job *job = (struct wait_job *)raw;
    uint64_t t0;

    while (atomic_load_explicit(job->holding, memory_order_acquire) == 0) {
        sched_yield();
    }
    t0 = nsec_now();
    job->rc = job->mode == WWLOCK_READ ? wwlock_rdlock(g_lock) : wwlock_wrlock(g_lock);
    job->elapsed_ns = nsec_now() - t0;
    if (job->rc == WWLOCK_OK) {
        wwlock_unlock(g_lock);
    }
    return NULL;
}

static int fail(const char *msg)
{
    fprintf(stderr, "read/write example failed: %s\n", msg);
    return 1;
}

int main(void)
{
    posix_park_t park_state;
    wwlock_park_t park;
    pthread_t threads[3];
    struct reader_job jobs[3];
    struct wait_job waiter;
    atomic_int entered;
    atomic_int holding;
    int i;
    int rc;
    void *status;

    alarm(15);
    posix_park_init(&park_state);
    posix_park_export(&park_state, &park);
    if (wwlock_class_init(&g_class, &park) != WWLOCK_OK || wwlock_init(&g_lock, g_class) != WWLOCK_OK) {
        return fail("init");
    }

    printf("same thread takes the write lock twice\n");
    if (wwlock_wrlock(g_lock) != WWLOCK_OK) {
        return fail("first write");
    }
    rc = wwlock_wrlock(g_lock);
    printf("second write on the same thread returned %s\n", rc == WWLOCK_OK ? "OK" : "an error");
    if (rc != WWLOCK_OK) {
        return fail("second write");
    }
    wwlock_unlock(g_lock);
    wwlock_unlock(g_lock);

    printf("three readers enter one lock together\n");
    atomic_store(&entered, 0);
    for (i = 0; i < 3; i++) {
        jobs[i].id = i + 1;
        jobs[i].entered = &entered;
        pthread_create(&threads[i], NULL, reader, &jobs[i]);
    }
    for (i = 0; i < 3; i++) {
        pthread_join(threads[i], &status);
        if (status != NULL) {
            return fail("reader");
        }
    }

    printf("writer waits while a reader holds the lock\n");
    atomic_store(&holding, 0);
    waiter.mode = WWLOCK_WRITE;
    waiter.holding = &holding;
    waiter.rc = -1;
    if (wwlock_rdlock(g_lock) != WWLOCK_OK) {
        return fail("reader hold");
    }
    atomic_store_explicit(&holding, 1, memory_order_release);
    pthread_create(&threads[0], NULL, hold_then_wait, &waiter);
    usleep(80000);
    wwlock_unlock(g_lock);
    pthread_join(threads[0], NULL);
    printf("writer entered after %llu ms\n", (unsigned long long)(waiter.elapsed_ns / 1000000ull));
    if (waiter.rc != WWLOCK_OK || waiter.elapsed_ns < 50000000ull) {
        return fail("writer did not wait for the reader");
    }

    printf("reader waits while a writer holds the lock\n");
    atomic_store(&holding, 0);
    waiter.mode = WWLOCK_READ;
    waiter.rc = -1;
    if (wwlock_wrlock(g_lock) != WWLOCK_OK) {
        return fail("writer hold");
    }
    atomic_store_explicit(&holding, 1, memory_order_release);
    pthread_create(&threads[0], NULL, hold_then_wait, &waiter);
    usleep(80000);
    wwlock_unlock(g_lock);
    pthread_join(threads[0], NULL);
    printf("reader entered after %llu ms\n", (unsigned long long)(waiter.elapsed_ns / 1000000ull));
    if (waiter.rc != WWLOCK_OK || waiter.elapsed_ns < 50000000ull) {
        return fail("reader did not wait for the writer");
    }

    wwlock_deinit(g_lock);
    wwlock_class_deinit(g_class);
    posix_park_fini(&park_state);
    printf("read/write example passed\n");
    return 0;
}
