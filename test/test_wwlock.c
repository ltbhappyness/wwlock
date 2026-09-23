/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "park_posix.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fails;
static posix_park_t g_park;
static wwlock_class_t *g_class;

static void check(int cond, const char *file, int line, const char *expr)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s:%d %s\n", file, line, expr);
        g_fails++;
    }
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)

static int held(int rc)
{
    return rc == WWLOCK_OK || rc == WWLOCK_ALREADY;
}

static uint64_t nsec_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void wait_until(atomic_int *value, int need)
{
    while (atomic_load_explicit(value, memory_order_acquire) < need) {
        sched_yield();
    }
}

static wwlock_t *make_lock(void)
{
    wwlock_t *lock = NULL;

    CHECK(wwlock_init(&lock, g_class) == WWLOCK_OK);
    return lock;
}

static void *probe_thread(void *raw);

static void test_same_thread_write(void)
{
    wwlock_t *lock = make_lock();
    pthread_t thread;
    uint64_t t0;
    uint64_t elapsed;

    printf("one lock: same thread write, then write again, needs two unlocks\n");
    CHECK(wwlock_wrlock(lock) == WWLOCK_OK);
    CHECK(wwlock_wrlock(lock) == WWLOCK_OK);
    CHECK(wwlock_deinit(lock) == WWLOCK_BUSY);
    CHECK(wwlock_unlock(lock) == WWLOCK_OK);
    CHECK(wwlock_deinit(lock) == WWLOCK_BUSY);
    CHECK(wwlock_unlock(lock) == WWLOCK_OK);
    CHECK(wwlock_unlock(lock) == WWLOCK_INVAL);
    t0 = nsec_now();
    pthread_create(&thread, NULL, probe_thread, lock);
    pthread_join(thread, NULL);
    elapsed = nsec_now() - t0;
    CHECK(elapsed < 50000000ull);
    CHECK(wwlock_deinit(lock) == WWLOCK_OK);
}

static void *probe_thread(void *raw)
{
    wwlock_t *lock = (wwlock_t *)raw;

    CHECK(wwlock_wrlock(lock) == WWLOCK_OK);
    wwlock_unlock(lock);
    return NULL;
}

static void test_same_thread_read(void)
{
    wwlock_t *lock = make_lock();

    printf("one lock: same thread read, then read again, needs two unlocks\n");
    CHECK(wwlock_rdlock(lock) == WWLOCK_OK);
    CHECK(wwlock_rdlock(lock) == WWLOCK_OK);
    CHECK(wwlock_unlock(lock) == WWLOCK_OK);
    CHECK(wwlock_unlock(lock) == WWLOCK_OK);
    CHECK(wwlock_unlock(lock) == WWLOCK_INVAL);
    CHECK(wwlock_lock_mode(lock, 0) == WWLOCK_INVAL);
    wwlock_deinit(lock);
}

static void test_read_then_write(void)
{
    wwlock_t *lock = make_lock();

    printf("one lock: read then write needs its own unlock\n");
    CHECK(wwlock_rdlock(lock) == WWLOCK_OK);
    CHECK(wwlock_wrlock(lock) == WWLOCK_OK);
    CHECK(wwlock_deinit(lock) == WWLOCK_BUSY);
    CHECK(wwlock_unlock(lock) == WWLOCK_OK);
    CHECK(wwlock_deinit(lock) == WWLOCK_BUSY);
    CHECK(wwlock_unlock(lock) == WWLOCK_OK);
    CHECK(wwlock_deinit(lock) == WWLOCK_OK);
}

struct wait_job {
    wwlock_t *lock;
    int mode;
    atomic_int *ready;
    int rc;
    uint64_t elapsed_ns;
};

static void *waiter_thread(void *raw)
{
    struct wait_job *job = (struct wait_job *)raw;
    uint64_t t0;

    wait_until(job->ready, 1);
    t0 = nsec_now();
    job->rc = wwlock_lock_mode(job->lock, job->mode);
    job->elapsed_ns = nsec_now() - t0;
    if (job->rc == WWLOCK_OK) {
        wwlock_unlock(job->lock);
    }
    return NULL;
}

static void expect_waits(const char *title, int holder_mode, int waiter_mode)
{
    wwlock_t *lock = make_lock();
    struct wait_job job;
    pthread_t thread;
    atomic_int ready;

    printf("%s\n", title);
    atomic_store(&ready, 0);
    memset(&job, 0, sizeof(job));
    job.lock = lock;
    job.mode = waiter_mode;
    job.ready = &ready;
    CHECK(wwlock_lock_mode(lock, holder_mode) == WWLOCK_OK);
    pthread_create(&thread, NULL, waiter_thread, &job);
    usleep(10000);
    atomic_store_explicit(&ready, 1, memory_order_release);
    usleep(80000);
    wwlock_unlock(lock);
    pthread_join(thread, NULL);
    CHECK(job.rc == WWLOCK_OK);
    CHECK(job.elapsed_ns >= 40000000ull);
    wwlock_deinit(lock);
}

static void test_two_classes(void)
{
    wwlock_class_t *other = NULL;
    wwlock_park_t park;
    wwlock_t *a = make_lock();
    wwlock_t *b = NULL;

    printf("two classes: a thread cannot mix them\n");
    posix_park_export(&g_park, &park);
    CHECK(wwlock_class_init(&other, &park) == WWLOCK_OK);
    CHECK(wwlock_init(&b, other) == WWLOCK_OK);
    CHECK(wwlock_class_deinit(other) == WWLOCK_BUSY);
    CHECK(wwlock_wrlock(a) == WWLOCK_OK);
    CHECK(wwlock_wrlock(b) == WWLOCK_INVAL);
    wwlock_unlock(a);
    CHECK(wwlock_wrlock(b) == WWLOCK_OK);
    wwlock_unlock(b);
    wwlock_deinit(b);
    wwlock_deinit(a);
    wwlock_class_deinit(other);
}

struct readers_arg {
    wwlock_t *lock;
    atomic_int *arrived;
    int n;
};

static void *readers_thread(void *raw)
{
    struct readers_arg *arg = (struct readers_arg *)raw;

    CHECK(wwlock_rdlock(arg->lock) == WWLOCK_OK);
    atomic_fetch_add_explicit(arg->arrived, 1, memory_order_release);
    wait_until(arg->arrived, arg->n);
    wwlock_unlock(arg->lock);
    return NULL;
}

static void test_readers(void)
{
    wwlock_t *lock = make_lock();
    pthread_t threads[4];
    struct readers_arg arg;
    atomic_int arrived;
    int i;

    printf("one lock: four readers sit inside together\n");
    atomic_store(&arrived, 0);
    arg.lock = lock;
    arg.arrived = &arrived;
    arg.n = 4;
    for (i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, readers_thread, &arg);
    }
    for (i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK(atomic_load(&arrived) == 4);
    wwlock_deinit(lock);
}

struct exclude_arg {
    wwlock_t *lock;
    atomic_int *readers;
    atomic_int *writer;
    atomic_int *bad;
    int loops;
    int is_writer;
};

static void *exclude_thread(void *raw)
{
    struct exclude_arg *arg = (struct exclude_arg *)raw;
    int i;

    for (i = 0; i < arg->loops; i++) {
        if (arg->is_writer) {
            CHECK(held(wwlock_wrlock(arg->lock)));
            atomic_store_explicit(arg->writer, 1, memory_order_release);
            if (atomic_load_explicit(arg->readers, memory_order_acquire) != 0) {
                atomic_store(arg->bad, 1);
            }
            usleep(200);
            atomic_store_explicit(arg->writer, 0, memory_order_release);
            wwlock_unlock(arg->lock);
        } else {
            CHECK(held(wwlock_rdlock(arg->lock)));
            atomic_fetch_add_explicit(arg->readers, 1, memory_order_acq_rel);
            if (atomic_load_explicit(arg->writer, memory_order_acquire) != 0) {
                atomic_store(arg->bad, 1);
            }
            usleep(200);
            atomic_fetch_sub_explicit(arg->readers, 1, memory_order_acq_rel);
            wwlock_unlock(arg->lock);
        }
    }
    return NULL;
}

static void test_exclude(void)
{
    wwlock_t *lock = make_lock();
    pthread_t threads[4];
    struct exclude_arg args[4];
    atomic_int readers;
    atomic_int writer;
    atomic_int bad;
    int i;

    printf("one lock: a writer and readers never overlap\n");
    atomic_store(&readers, 0);
    atomic_store(&writer, 0);
    atomic_store(&bad, 0);
    for (i = 0; i < 4; i++) {
        args[i].lock = lock;
        args[i].readers = &readers;
        args[i].writer = &writer;
        args[i].bad = &bad;
        args[i].loops = 20;
        args[i].is_writer = i == 0;
        pthread_create(&threads[i], NULL, exclude_thread, &args[i]);
    }
    for (i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK(atomic_load(&bad) == 0);
    wwlock_deinit(lock);
}

struct timed_arg {
    wwlock_t *lock;
    wwlock_t *extra;
    atomic_int *flag;
    int rc;
    int again;
    int extra_rc;
    uint64_t elapsed_ns;
};

static void *young_die_thread(void *raw)
{
    struct timed_arg *arg = (struct timed_arg *)raw;
    uint64_t t0;

    wait_until(arg->flag, 1);
    CHECK(wwlock_wrlock(arg->extra) == WWLOCK_OK);
    wait_until(arg->flag, 2);
    t0 = nsec_now();
    arg->rc = wwlock_wrlock(arg->lock);
    arg->elapsed_ns = nsec_now() - t0;
    arg->again = wwlock_wrlock(arg->lock);
    arg->extra_rc = wwlock_wrlock(arg->extra);
    wwlock_unlock(arg->lock);
    wwlock_unlock(arg->extra);
    return NULL;
}

static void test_younger_dies(void)
{
    wwlock_t *lock = make_lock();
    wwlock_t *older = make_lock();
    wwlock_t *extra = make_lock();
    struct timed_arg arg;
    pthread_t thread;
    atomic_int flag;

    printf("younger thread sleeps, drops its earlier lock, and reports DIE\n");
    atomic_store(&flag, 0);
    memset(&arg, 0, sizeof(arg));
    arg.lock = lock;
    arg.extra = extra;
    arg.flag = &flag;
    CHECK(wwlock_wrlock(older) == WWLOCK_OK);
    atomic_store_explicit(&flag, 1, memory_order_release);
    pthread_create(&thread, NULL, young_die_thread, &arg);
    usleep(20000);
    CHECK(wwlock_wrlock(lock) == WWLOCK_OK);
    atomic_store_explicit(&flag, 2, memory_order_release);
    usleep(80000);
    wwlock_unlock(lock);
    wwlock_unlock(older);
    pthread_join(thread, NULL);
    CHECK(arg.rc == WWLOCK_DIE);
    CHECK(arg.again == WWLOCK_ALREADY);
    CHECK(arg.extra_rc == WWLOCK_OK);
    CHECK(arg.elapsed_ns >= 50000000ull);
    wwlock_deinit(lock);
    wwlock_deinit(older);
    wwlock_deinit(extra);
}

static void *older_wait_thread(void *raw)
{
    struct timed_arg *arg = (struct timed_arg *)raw;
    unsigned before;
    uint64_t t0;

    CHECK(wwlock_wrlock(arg->extra) == WWLOCK_OK);
    atomic_store_explicit(arg->flag, 1, memory_order_release);
    wait_until(arg->flag, 2);
    before = posix_park_waits(&g_park);
    t0 = nsec_now();
    arg->rc = wwlock_wrlock(arg->lock);
    arg->elapsed_ns = nsec_now() - t0;
    CHECK(posix_park_waits(&g_park) > before);
    wwlock_unlock(arg->lock);
    wwlock_unlock(arg->extra);
    return NULL;
}

static void test_older_waits(void)
{
    wwlock_t *lock = make_lock();
    wwlock_t *extra = make_lock();
    struct timed_arg arg;
    pthread_t thread;
    atomic_int flag;

    printf("older thread sleeps in the wait callback until the younger holder unlocks\n");
    atomic_store(&g_park.waits, 0);
    atomic_store(&flag, 0);
    memset(&arg, 0, sizeof(arg));
    arg.lock = lock;
    arg.extra = extra;
    arg.flag = &flag;
    pthread_create(&thread, NULL, older_wait_thread, &arg);
    wait_until(&flag, 1);
    CHECK(wwlock_wrlock(lock) == WWLOCK_OK);
    atomic_store_explicit(&flag, 2, memory_order_release);
    usleep(80000);
    wwlock_unlock(lock);
    pthread_join(thread, NULL);
    CHECK(arg.rc == WWLOCK_OK);
    CHECK(arg.elapsed_ns >= 50000000ull);
    wwlock_deinit(lock);
    wwlock_deinit(extra);
}

static void *plain_wait_thread(void *raw)
{
    struct timed_arg *arg = (struct timed_arg *)raw;
    unsigned before;
    uint64_t t0;

    wait_until(arg->flag, 1);
    before = posix_park_waits(&g_park);
    t0 = nsec_now();
    arg->rc = wwlock_wrlock(arg->lock);
    arg->elapsed_ns = nsec_now() - t0;
    CHECK(posix_park_waits(&g_park) > before);
    wwlock_unlock(arg->lock);
    return NULL;
}

static void test_first_lock_sleeps(void)
{
    wwlock_t *lock = make_lock();
    struct timed_arg arg;
    pthread_t thread;
    atomic_int flag;

    printf("first lock of a thread sleeps; it does not return DIE\n");
    atomic_store(&g_park.waits, 0);
    atomic_store(&flag, 0);
    memset(&arg, 0, sizeof(arg));
    arg.lock = lock;
    arg.flag = &flag;
    CHECK(wwlock_wrlock(lock) == WWLOCK_OK);
    pthread_create(&thread, NULL, plain_wait_thread, &arg);
    usleep(10000);
    atomic_store_explicit(&flag, 1, memory_order_release);
    usleep(80000);
    wwlock_unlock(lock);
    pthread_join(thread, NULL);
    CHECK(arg.rc == WWLOCK_OK);
    CHECK(arg.elapsed_ns >= 40000000ull);
    wwlock_deinit(lock);
}

struct pair_arg {
    wwlock_t *first;
    wwlock_t *second;
    atomic_int *phase;
    atomic_int *dies;
};

static int lock_pair(struct pair_arg *arg)
{
    for (;;) {
        int rc = wwlock_wrlock(arg->first);

        if (rc == WWLOCK_DIE) {
            atomic_fetch_add_explicit(arg->dies, 1, memory_order_relaxed);
            continue;
        }
        CHECK(held(rc));
        atomic_fetch_add_explicit(arg->phase, 1, memory_order_release);
        wait_until(arg->phase, 2);
        rc = wwlock_wrlock(arg->second);
        if (rc == WWLOCK_DIE) {
            atomic_fetch_add_explicit(arg->dies, 1, memory_order_relaxed);
            continue;
        }
        CHECK(held(rc));
        return WWLOCK_OK;
    }
}

static void *pair_thread(void *raw)
{
    struct pair_arg *arg = (struct pair_arg *)raw;

    CHECK(lock_pair(arg) == WWLOCK_OK);
    wwlock_unlock(arg->second);
    wwlock_unlock(arg->first);
    return NULL;
}

static void test_abba(void)
{
    wwlock_t *a = make_lock();
    wwlock_t *b = make_lock();
    struct pair_arg args[2];
    pthread_t threads[2];
    atomic_int phase;
    atomic_int dies;
    int i;

    printf("opposite order: both threads finish, and at least one DIE is reported\n");
    atomic_store(&phase, 0);
    atomic_store(&dies, 0);
    args[0].first = a;
    args[0].second = b;
    args[1].first = b;
    args[1].second = a;
    args[0].phase = args[1].phase = &phase;
    args[0].dies = args[1].dies = &dies;
    for (i = 0; i < 2; i++) {
        pthread_create(&threads[i], NULL, pair_thread, &args[i]);
    }
    for (i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK(atomic_load(&dies) > 0);
    wwlock_deinit(a);
    wwlock_deinit(b);
}

struct cycle_arg {
    wwlock_t **locks;
    int index;
    int n;
    atomic_int *phase;
};

static void *cycle_thread(void *raw)
{
    struct cycle_arg *arg = (struct cycle_arg *)raw;
    wwlock_t *first = arg->locks[arg->index];
    wwlock_t *second = arg->locks[(arg->index + 1) % arg->n];

    for (;;) {
        int rc = wwlock_wrlock(first);

        if (rc == WWLOCK_DIE) {
            continue;
        }
        CHECK(held(rc));
        atomic_fetch_add_explicit(arg->phase, 1, memory_order_release);
        wait_until(arg->phase, arg->n);
        rc = wwlock_wrlock(second);
        if (rc == WWLOCK_DIE) {
            continue;
        }
        CHECK(held(rc));
        wwlock_unlock(second);
        wwlock_unlock(first);
        return NULL;
    }
}

static void test_cycle(void)
{
    wwlock_t *locks[3];
    struct cycle_arg args[3];
    pthread_t threads[3];
    atomic_int phase;
    int i;

    printf("three threads in a circle all finish\n");
    atomic_store(&phase, 0);
    for (i = 0; i < 3; i++) {
        locks[i] = make_lock();
        args[i].locks = locks;
        args[i].index = i;
        args[i].n = 3;
        args[i].phase = &phase;
    }
    for (i = 0; i < 3; i++) {
        pthread_create(&threads[i], NULL, cycle_thread, &args[i]);
    }
    for (i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL);
        wwlock_deinit(locks[i]);
    }
}

struct stress_arg {
    wwlock_t **locks;
    int nlocks;
    int id;
    int loops;
    atomic_uint *hits;
};

static void *stress_thread(void *raw)
{
    struct stress_arg *arg = (struct stress_arg *)raw;
    wwlock_t *first = arg->locks[arg->id % arg->nlocks];
    wwlock_t *second = arg->locks[(arg->id + 1) % arg->nlocks];
    int i;

    for (i = 0; i < arg->loops; i++) {
        for (;;) {
            int rc = wwlock_lock_mode(first, arg->id % 2 == 0 ? WWLOCK_EXCLUSIVE : WWLOCK_SHARED);

            if (rc == WWLOCK_DIE) {
                continue;
            }
            CHECK(held(rc));
            rc = wwlock_wrlock(second);
            if (rc == WWLOCK_DIE) {
                continue;
            }
            CHECK(held(rc));
            break;
        }
        atomic_fetch_add_explicit(&arg->hits[arg->id], 1, memory_order_relaxed);
        wwlock_unlock(second);
        wwlock_unlock(first);
    }
    return NULL;
}

static void test_stress(void)
{
    enum { nthreads = 8, nlocks = 4, loops = 40 };
    wwlock_t *locks[nlocks];
    struct stress_arg args[nthreads];
    pthread_t threads[nthreads];
    atomic_uint hits[nthreads];
    int i;

    printf("eight threads, four locks, shared and exclusive, 40 rounds each\n");
    for (i = 0; i < nlocks; i++) {
        locks[i] = make_lock();
    }
    for (i = 0; i < nthreads; i++) {
        atomic_store(&hits[i], 0);
        args[i].locks = locks;
        args[i].nlocks = nlocks;
        args[i].id = i;
        args[i].loops = loops;
        args[i].hits = hits;
        pthread_create(&threads[i], NULL, stress_thread, &args[i]);
    }
    for (i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
        CHECK(atomic_load(&hits[i]) == (unsigned)loops);
    }
    for (i = 0; i < nlocks; i++) {
        wwlock_deinit(locks[i]);
    }
}

struct call_job {
    wwlock_t *first;
    wwlock_t *second;
    int write_second;
    atomic_int *phase;
};

static int lib_touch(void *raw)
{
    struct call_job *job = (struct call_job *)raw;

    if (job->write_second) {
        wwlock_wrlock(job->second);
    } else {
        wwlock_rdlock(job->second);
    }
    wwlock_unlock(job->second);
    return 0;
}

static int app_with_library(void *raw)
{
    struct call_job *job = (struct call_job *)raw;

    wwlock_wrlock(job->first);
    atomic_fetch_add_explicit(job->phase, 1, memory_order_release);
    wait_until(job->phase, 2);
    wwlock_call(lib_touch, job);
    wwlock_unlock(job->first);
    return 0;
}

static void *call_thread(void *raw)
{
    CHECK(wwlock_call(app_with_library, raw) == 0);
    return NULL;
}

static int lib_only(void *raw)
{
    wwlock_t *lock = (wwlock_t *)raw;

    wwlock_rdlock(lock);
    wwlock_unlock(lock);
    return 7;
}

static void test_call_composes(void)
{
    wwlock_t *lock_a = make_lock();
    wwlock_t *lock_b = make_lock();
    struct call_job jobs[2];
    pthread_t threads[2];
    atomic_int phase;
    int rc;

    printf("several locks: application and library share wwlock_call\n");
    atomic_store(&phase, 0);
    jobs[0].first = lock_a;
    jobs[0].second = lock_b;
    jobs[0].write_second = 0;
    jobs[0].phase = &phase;
    jobs[1].first = lock_b;
    jobs[1].second = lock_a;
    jobs[1].write_second = 1;
    jobs[1].phase = &phase;
    pthread_create(&threads[0], NULL, call_thread, &jobs[0]);
    pthread_create(&threads[1], NULL, call_thread, &jobs[1]);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    CHECK(wwlock_wrlock(lock_a) == WWLOCK_OK);
    rc = wwlock_call(lib_only, lock_b);
    CHECK(rc == WWLOCK_INVAL);
    wwlock_unlock(lock_a);
    wwlock_deinit(lock_a);
    wwlock_deinit(lock_b);
}

int main(void)
{
    wwlock_park_t park;

    alarm(60);
    posix_park_init(&g_park);
    posix_park_export(&g_park, &park);
    CHECK(wwlock_class_init(&g_class, &park) == WWLOCK_OK);

    printf("-- one lock --\n");
    test_same_thread_write();
    test_same_thread_read();
    test_read_then_write();
    test_readers();
    expect_waits("one lock: writer waits while a reader holds it", WWLOCK_READ, WWLOCK_WRITE);
    expect_waits("one lock: writer waits while a writer holds it", WWLOCK_WRITE, WWLOCK_WRITE);
    expect_waits("one lock: reader waits while a writer holds it", WWLOCK_WRITE, WWLOCK_READ);
    test_exclude();
    test_first_lock_sleeps();
    printf("-- several locks --\n");
    test_two_classes();
    test_younger_dies();
    test_older_waits();
    test_call_composes();
    test_abba();
    test_cycle();
    test_stress();

    wwlock_class_deinit(g_class);
    posix_park_fini(&g_park);
    if (g_fails != 0) {
        fprintf(stderr, "%d checks failed\n", g_fails);
        return 1;
    }
    printf("wwlock tests passed\n");
    return 0;
}
