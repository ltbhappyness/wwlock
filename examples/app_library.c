/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * The application write-locks a file. A log library it calls read-locks a log.
 * The other thread write-locks the log, and the library write-locks the file.
 * Neither side checks WWLOCK_DIE. wwlock_call runs the work again.
 */
#include "park_posix.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static wwlock_class_t *g_class;
static wwlock_t *g_file;
static wwlock_t *g_log;
static atomic_int g_phase;

/* Log library. It does not know the application holds another lock. */
static int lib_read_log(void *unused)
{
    (void)unused;
    wwlock_rdlock(g_log);
    wwlock_unlock(g_log);
    return 0;
}

static int lib_copy_file(void *unused)
{
    (void)unused;
    wwlock_wrlock(g_file);
    wwlock_unlock(g_file);
    return 0;
}

static void lib_append(void)
{
    wwlock_call(lib_read_log, NULL);
}

static void lib_snapshot(void)
{
    wwlock_call(lib_copy_file, NULL);
}

static void meet(void)
{
    atomic_fetch_add_explicit(&g_phase, 1, memory_order_release);
    while (atomic_load_explicit(&g_phase, memory_order_acquire) < 2) {
        sched_yield();
    }
}

/* Application. It does not know the library takes a lock. */
static int app_save(void *unused)
{
    (void)unused;
    wwlock_wrlock(g_file);
    meet();
    lib_append();
    wwlock_unlock(g_file);
    return 0;
}

static int app_rotate_log(void *unused)
{
    (void)unused;
    wwlock_wrlock(g_log);
    meet();
    lib_snapshot();
    wwlock_unlock(g_log);
    return 0;
}

static void *run_save(void *unused)
{
    (void)unused;
    if (wwlock_call(app_save, NULL) != 0) {
        return (void *)1;
    }
    return NULL;
}

static void *run_rotate(void *unused)
{
    (void)unused;
    if (wwlock_call(app_rotate_log, NULL) != 0) {
        return (void *)1;
    }
    return NULL;
}

int main(void)
{
    posix_park_t park_state;
    wwlock_park_t park;
    pthread_t threads[2];
    void *status;
    int i;

    alarm(15);
    posix_park_init(&park_state);
    posix_park_export(&park_state, &park);
    if (wwlock_class_init(&g_class, &park) != WWLOCK_OK || wwlock_init(&g_file, g_class) != WWLOCK_OK ||
        wwlock_init(&g_log, g_class) != WWLOCK_OK) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    atomic_store(&g_phase, 0);
    pthread_create(&threads[0], NULL, run_save, NULL);
    pthread_create(&threads[1], NULL, run_rotate, NULL);
    for (i = 0; i < 2; i++) {
        pthread_join(threads[i], &status);
        if (status != NULL) {
            fprintf(stderr, "worker failed\n");
            return 1;
        }
    }
    wwlock_deinit(g_file);
    wwlock_deinit(g_log);
    wwlock_class_deinit(g_class);
    posix_park_fini(&park_state);
    printf("application and library locked without handling WWLOCK_DIE\n");
    return 0;
}
