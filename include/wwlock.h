/* Copyright (c) 2026 LTBH wwlock contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef WWLOCK_H
#define WWLOCK_H

/*
 * Wait-Die locks for one process. Use this when one piece of work must hold
 * several locks and the order cannot be fixed in advance. The kernel's
 * wait/wound mutex page describes that problem:
 * https://docs.kernel.org/locking/ww-mutex-design.html
 *
 * This library implements Wait-Die only. It does not implement Wound-Wait.
 * It is not a substitute for pthread_rwlock on one lock that is mostly read,
 * and not a substitute for pthread_mutex on a short exclusive section.
 * Those stay on one atomic word. A hold here synchronizes the protected data
 * the same way: stores before unlock are visible to the next lock.
 *
 * Readers may overlap. A writer excludes every other thread. A new reader
 * does not join an existing read if an older attempt is already waiting.
 * Fairness follows when each attempt started, not arrival at this lock and
 * not a reader-preferring or writer-preferring pthread policy. Finish slow
 * work after the last unlock. A new attempt after that goes to the back.
 *
 * One class at a time per thread. Not process-shared, not safe after fork
 * in a process that had other threads, not async-signal-safe, and no
 * priority inheritance.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Read hold. Other readers may enter. A writer may not. */
#define WWLOCK_READ 1

/** @brief Write hold. No other thread may read or write. */
#define WWLOCK_WRITE 2

/** @brief Same as WWLOCK_READ. */
#define WWLOCK_SHARED WWLOCK_READ

/** @brief Same as WWLOCK_WRITE. */
#define WWLOCK_EXCLUSIVE WWLOCK_WRITE

/**
 * @brief Wake every sleeper on this lock's word.
 *
 * The wake callback must wake every sleeper on that word. Waking fewer can
 * leave the oldest waiter asleep until some later wake. The word address is
 * the wait key. A class-wide condition variable is correct and will not scale.
 */
#define WWLOCK_WAKE_ALL 0x7fffffff

/** @brief The lock is held. */
#define WWLOCK_OK 0

/**
 * @brief This call did not take a new hold. One unlock still releases the lock.
 *
 * Returned when a restarted attempt finds the lock already held for it.
 */
#define WWLOCK_ALREADY 1

/**
 * @brief This thread already held another lock, so this attempt must start over.
 *
 * The lock just requested is held, and the earlier ones were released. Do not
 * unlock that lock before retrying. If it was the last hold, the unlock ends
 * the attempt and the next lock is a newer, younger one. Two threads that
 * unlock it can bounce forever. A thread that holds nothing never gets this:
 * it sleeps until the lock is free. Code inside wwlock_call does not see this
 * value. The library runs that function again.
 */
#define WWLOCK_DIE 2

/** @brief Memory for a lock or a holder could not be allocated. */
#define WWLOCK_NOMEM 3

/** @brief A pointer or a mode is not usable, or two classes were mixed. */
#define WWLOCK_INVAL 4

/** @brief The lock or class is still in use, so it was not freed. */
#define WWLOCK_BUSY 5

/**
 * @brief Sleep until a wake, or return at once when @p word is no longer @p expect.
 *
 * Check and sleep as one step, as with a futex. Load @p word with an atomic load.
 * A plain read races with the library. A spurious return is allowed. Return 0.
 * This function must not call back into wwlock. A thread blocked here must not
 * be cancelled. The waiter lives on that thread's stack until wait returns.
 */
typedef int (*wwlock_wait_fn)(uint32_t *word, uint32_t expect, void *user);

/**
 * @brief Wake every sleeper blocked in wait on @p word.
 *
 * @p n is WWLOCK_WAKE_ALL. Waking fewer than every sleeper on that word can
 * leave a waiter asleep with no later wake. Return 0. This function must not
 * call back into wwlock. The allocator used by the library must not call back
 * into wwlock either.
 */
typedef int (*wwlock_wake_fn)(uint32_t *word, int n, void *user);

/** @brief Sleep and wake used by every lock in one class. */
typedef struct wwlock_park {
    wwlock_wait_fn wait;
    wwlock_wake_fn wake;
    void *user;
} wwlock_park_t;

typedef struct wwlock_class wwlock_class_t;
typedef struct wwlock wwlock_t;

/**
 * @brief Create a class. Locks in one class avoid deadlock with each other.
 *
 * @param[out] klass Receives the class
 * @param[in]  park  Wait and wake callbacks
 * @return WWLOCK_OK, WWLOCK_NOMEM, or WWLOCK_INVAL.
 */
int wwlock_class_init(wwlock_class_t **klass, const wwlock_park_t *park);

/**
 * @brief Free a class. Every lock created from it must already have been freed.
 *
 * @param[in] klass Class object
 * @return WWLOCK_OK, or WWLOCK_BUSY when a lock from this class still exists.
 *         BUSY is not a barrier. Stop every thread that can create a lock
 *         before calling this, as with destroying a pthread rwlock.
 */
int wwlock_class_deinit(wwlock_class_t *klass);

/**
 * @brief Create an unlocked lock.
 *
 * @param[out] lock  Receives the lock
 * @param[in]  klass Class from wwlock_class_init
 * @return WWLOCK_OK, WWLOCK_NOMEM, or WWLOCK_INVAL.
 */
int wwlock_init(wwlock_t **lock, wwlock_class_t *klass);

/**
 * @brief Free a lock. The lock must not be held and must have no waiters.
 *
 * @param[in] lock Lock object
 * @return WWLOCK_OK, WWLOCK_BUSY when it is still in use, or WWLOCK_INVAL.
 *         Stop every other thread from locking it before this call. The busy
 *         check can race with a new locker.
 */
int wwlock_deinit(wwlock_t *lock);

/**
 * @brief Hold @p lock on the calling thread in @p mode.
 *
 * The first hold on a thread starts its attempt. The last unlock ends it.
 * Readers do not conflict with readers. A writer conflicts with every other
 * holder. When this thread already holds a lock and waiting would circle,
 * earlier holds are dropped, this call sleeps until @p lock is held, and the
 * return is WWLOCK_DIE.
 *
 * Callers normally use wwlock_rdlock, wwlock_wrlock, or wwlock_lock.
 * Do not test the return as a boolean. WWLOCK_OK and WWLOCK_ALREADY both
 * mean the lock is held. WWLOCK_DIE means the requested lock is held and
 * the attempt must be started again. Any other value means it is not held.
 * A read lock followed by a write lock on the same thread returns WWLOCK_OK
 * and needs its own unlock. The hold stays a write until the last unlock.
 *
 * @param[in,out] lock Lock object
 * @param[in]     mode WWLOCK_READ or WWLOCK_WRITE
 * @return WWLOCK_OK, WWLOCK_ALREADY, WWLOCK_DIE, WWLOCK_NOMEM, or WWLOCK_INVAL.
 */
int wwlock_lock_mode(wwlock_t *lock, int mode);

/** @brief Read lock. Other readers may enter unless an older attempt is already waiting. */
#define wwlock_rdlock(lock) wwlock_lock_mode((lock), WWLOCK_READ)

/** @brief Write lock. Other readers and writers wait. */
#define wwlock_wrlock(lock) wwlock_lock_mode((lock), WWLOCK_WRITE)

/** @brief Write lock. Same as wwlock_wrlock. */
#define wwlock_lock(lock) wwlock_lock_mode((lock), WWLOCK_WRITE)

/**
 * @brief Drop one hold on @p lock and, on the last hold, wake every sleeper on it.
 *
 * Each successful lock call needs one unlock. A second lock on the same thread
 * needs a second unlock. WWLOCK_ALREADY does not add a hold.
 *
 * @param[in,out] lock Lock object
 * @return WWLOCK_OK, or WWLOCK_INVAL when this thread does not hold @p lock.
 */
int wwlock_unlock(wwlock_t *lock);

/**
 * @brief Run @p fn, and run it again when a lock in it cannot wait safely.
 *
 * @p fn and any library it calls may use wwlock_rdlock and wwlock_wrlock.
 * They do not check WWLOCK_DIE. A nested wwlock_call joins this one and
 * cannot restart by itself. Calling this while the thread already holds a
 * lock returns WWLOCK_INVAL.
 *
 * @p fn may run again from its first line. Do not allocate, open a file,
 * take another mutex, or publish a result before the locks are held. A path
 * that does not lock the contended object again on the retry leaves that
 * hold owned by the thread. Do not longjmp out of @p fn, do not call
 * pthread_exit, and do not cancel the thread. Those leave the restart point
 * aimed at a finished call. C++ destructors between the start of @p fn and
 * the lock call do not run when the function is restarted.
 *
 * @param[in] fn  Work to run. Return value is passed back.
 * @param[in] arg Pointer passed to @p fn
 * @return What @p fn returns, or WWLOCK_INVAL.
 */
int wwlock_call(int (*fn)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif

#endif
