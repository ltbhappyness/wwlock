/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "park_posix.h"
#include "wwlock_futex.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

static wwlock_class_t *g_class;

static uint64_t nsec_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void report(const char *name, int threads, uint64_t ops, uint64_t ns)
{
    double seconds = (double)ns / 1e9;
    double per = seconds == 0.0 ? 0.0 : (double)ns / (double)ops;

    printf("%-28s %2d threads %10llu ops %8.1f ns/op %8.2f Mops/s\n", name, threads,
           (unsigned long long)ops, per, seconds == 0.0 ? 0.0 : (double)ops / seconds / 1e6);
}

static void bench_uncontended(int mode, const char *name)
{
    wwlock_t *lock = NULL;
    enum { loops = 400000 };
    uint64_t t0;
    uint64_t t1;
    int i;

    wwlock_init(&lock, g_class);
    for (i = 0; i < 1000; i++) {
        wwlock_lock_mode(lock, mode);
        wwlock_unlock(lock);
    }
    t0 = nsec_now();
    for (i = 0; i < loops; i++) {
        wwlock_lock_mode(lock, mode);
        wwlock_unlock(lock);
    }
    t1 = nsec_now();
    wwlock_deinit(lock);
    report(name, 1, (uint64_t)loops, t1 - t0);
}

struct contend_arg {
    wwlock_t *lock;
    int loops;
    int mode;
};

static void *contend_thread(void *raw)
{
    struct contend_arg *arg = (struct contend_arg *)raw;
    int i;

    for (i = 0; i < arg->loops; i++) {
        wwlock_lock_mode(arg->lock, arg->mode);
        wwlock_unlock(arg->lock);
    }
    return NULL;
}

static void bench_contended(int mode, const char *name)
{
    enum { threads = 4, loops = 40000 };
    wwlock_t *lock = NULL;
    pthread_t ids[threads];
    struct contend_arg arg;
    uint64_t t0;
    uint64_t t1;
    int i;

    wwlock_init(&lock, g_class);
    arg.lock = lock;
    arg.loops = loops;
    arg.mode = mode;
    t0 = nsec_now();
    for (i = 0; i < threads; i++) {
        pthread_create(&ids[i], NULL, contend_thread, &arg);
    }
    for (i = 0; i < threads; i++) {
        pthread_join(ids[i], NULL);
    }
    t1 = nsec_now();
    wwlock_deinit(lock);
    report(name, threads, (uint64_t)threads * (uint64_t)loops, t1 - t0);
}

struct own_arg {
    wwlock_class_t *klass;
    int loops;
};

static void *own_thread(void *raw)
{
    struct own_arg *arg = (struct own_arg *)raw;
    wwlock_t *lock = NULL;
    int i;

    wwlock_init(&lock, arg->klass);
    for (i = 0; i < arg->loops; i++) {
        wwlock_wrlock(lock);
        wwlock_unlock(lock);
    }
    wwlock_deinit(lock);
    return NULL;
}

static void bench_distinct(void)
{
    enum { threads = 4, loops = 200000 };
    pthread_t ids[threads];
    struct own_arg arg;
    uint64_t t0;
    uint64_t t1;
    int i;

    arg.klass = g_class;
    arg.loops = loops;
    t0 = nsec_now();
    for (i = 0; i < threads; i++) {
        pthread_create(&ids[i], NULL, own_thread, &arg);
    }
    for (i = 0; i < threads; i++) {
        pthread_join(ids[i], NULL);
    }
    t1 = nsec_now();
    report("exclusive distinct locks", threads, (uint64_t)threads * (uint64_t)loops, t1 - t0);
}

struct abba_arg {
    wwlock_t *a;
    wwlock_t *b;
    int loops;
};

static void *abba_thread(void *raw)
{
    struct abba_arg *arg = (struct abba_arg *)raw;
    int i;

    for (i = 0; i < arg->loops; i++) {
        for (;;) {
            int rc = wwlock_wrlock(arg->a);

            if (rc == WWLOCK_DIE) {
                continue;
            }
            rc = wwlock_wrlock(arg->b);
            if (rc == WWLOCK_DIE) {
                continue;
            }
            break;
        }
        wwlock_unlock(arg->b);
        wwlock_unlock(arg->a);
    }
    return NULL;
}

static void bench_abba(void)
{
    enum { loops = 2000 };
    wwlock_t *a = NULL;
    wwlock_t *b = NULL;
    struct abba_arg args[2];
    pthread_t threads[2];
    uint64_t t0;
    uint64_t t1;
    int i;

    wwlock_init(&a, g_class);
    wwlock_init(&b, g_class);
    args[0].a = a;
    args[0].b = b;
    args[0].loops = loops;
    args[1].a = b;
    args[1].b = a;
    args[1].loops = loops;
    t0 = nsec_now();
    for (i = 0; i < 2; i++) {
        pthread_create(&threads[i], NULL, abba_thread, &args[i]);
    }
    for (i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
    }
    t1 = nsec_now();
    wwlock_deinit(a);
    wwlock_deinit(b);
    report("exclusive ABBA retry", 2, (uint64_t)loops * 2ull, t1 - t0);
}

static int run_suite(const char *title, const wwlock_park_t *park)
{
    printf("\n%s\n", title);
    if (wwlock_class_init(&g_class, park) != WWLOCK_OK) {
        return 1;
    }
    bench_uncontended(WWLOCK_EXCLUSIVE, "exclusive uncontended");
    bench_uncontended(WWLOCK_SHARED, "shared uncontended");
    bench_distinct();
    bench_contended(WWLOCK_SHARED, "shared one lock");
    bench_contended(WWLOCK_EXCLUSIVE, "exclusive one lock");
    bench_abba();
    wwlock_class_deinit(g_class);
    g_class = NULL;
    return 0;
}

int main(void)
{
    posix_park_t park_state;
    wwlock_park_t cond;
    wwlock_park_t futex;

    posix_park_init(&park_state);
    posix_park_export(&park_state, &cond);
    if (run_suite("wwlock benchmark, condition variable", &cond) != 0) {
        return 1;
    }
    if (wwlock_futex_fill(&futex) == WWLOCK_OK) {
        if (run_suite("wwlock benchmark, per-word futex", &futex) != 0) {
            return 1;
        }
    }
    posix_park_fini(&park_state);
    return 0;
}
