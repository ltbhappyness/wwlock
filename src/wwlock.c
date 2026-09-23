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
#include "wwlock.h"

#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct wwlock_tx {
    struct wwlock_class *klass;
    uint64_t stamp;
    _Atomic unsigned acquired;
    struct wwlock_slot *holds;
    struct wwlock *pinned;
    int active;
    int keep;
};

struct wwlock_waiter {
    struct wwlock_tx *tx;
    struct wwlock_waiter *next;
    int mode;
    int die;
};

struct wwlock_slot {
    struct wwlock_tx *tx;
    struct wwlock *lock;
    struct wwlock_slot *next_in_lock;
    struct wwlock_slot *next_in_tx;
    struct wwlock_slot *free_next;
    int mode;
    int in_use;
    unsigned count;
};

struct wwlock_class {
    _Atomic uint64_t stamp;
    wwlock_park_t park;
    _Atomic uint32_t free_gate;
    _Atomic unsigned nlocks;
    struct wwlock_slot *freelist;
};

enum { WWLOCK_INLINE_HOLDERS = 8 };

struct wwlock {
    struct wwlock_class *klass;
    _Atomic uint32_t gate;
    _Atomic uint32_t seq;
    struct wwlock_slot *holders;
    struct wwlock_waiter *waiters;
    struct wwlock_slot inline_store[WWLOCK_INLINE_HOLDERS];
};

static _Thread_local struct wwlock_tx tls_tx;
static _Thread_local jmp_buf tls_restart;
static _Thread_local int tls_in_call;

static void cpu_relax(void)
{
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

static void gate_lock(struct wwlock *lock)
{
    for (;;) {
        uint32_t zero = 0;

        if (atomic_compare_exchange_weak_explicit(&lock->gate, &zero, 1, memory_order_acquire,
                                                  memory_order_relaxed)) {
            return;
        }
        while (atomic_load_explicit(&lock->gate, memory_order_relaxed) != 0) {
            cpu_relax();
        }
    }
}

static void gate_unlock(struct wwlock *lock)
{
    atomic_store_explicit(&lock->gate, 0, memory_order_release);
}

static void free_gate_lock(struct wwlock_class *klass)
{
    for (;;) {
        uint32_t zero = 0;

        if (atomic_compare_exchange_weak_explicit(&klass->free_gate, &zero, 1, memory_order_acquire,
                                                  memory_order_relaxed)) {
            return;
        }
        while (atomic_load_explicit(&klass->free_gate, memory_order_relaxed) != 0) {
            cpu_relax();
        }
    }
}

static void free_gate_unlock(struct wwlock_class *klass)
{
    atomic_store_explicit(&klass->free_gate, 0, memory_order_release);
}

static void bump(struct wwlock *lock)
{
    atomic_fetch_add_explicit(&lock->seq, 1, memory_order_release);
}

static uint32_t load_seq(struct wwlock *lock)
{
    return atomic_load_explicit(&lock->seq, memory_order_acquire);
}

static unsigned held_count(const struct wwlock_tx *tx)
{
    return atomic_load_explicit(&tx->acquired, memory_order_acquire);
}

static void held_add(struct wwlock_tx *tx)
{
    atomic_fetch_add_explicit(&tx->acquired, 1, memory_order_release);
}

static void held_sub(struct wwlock_tx *tx)
{
    atomic_fetch_sub_explicit(&tx->acquired, 1, memory_order_release);
}

static int covers(int have, int want)
{
    if (have == WWLOCK_EXCLUSIVE) {
        return 1;
    }
    return want == WWLOCK_SHARED;
}

static struct wwlock_slot *find_holder(struct wwlock *lock, struct wwlock_tx *tx)
{
    struct wwlock_slot *slot;

    for (slot = lock->holders; slot != NULL; slot = slot->next_in_lock) {
        if (slot->tx == tx) {
            return slot;
        }
    }
    return NULL;
}

static int conflicts(struct wwlock *lock, struct wwlock_tx *tx, int mode)
{
    struct wwlock_slot *slot;

    for (slot = lock->holders; slot != NULL; slot = slot->next_in_lock) {
        if (slot->tx == tx) {
            continue;
        }
        if (mode == WWLOCK_SHARED && slot->mode == WWLOCK_SHARED) {
            continue;
        }
        return 1;
    }
    return 0;
}

static int others_block_exclusive(struct wwlock *lock, struct wwlock_tx *tx)
{
    struct wwlock_slot *slot;

    for (slot = lock->holders; slot != NULL; slot = slot->next_in_lock) {
        if (slot->tx != tx) {
            return 1;
        }
    }
    return 0;
}

static int older_holder(struct wwlock *lock, struct wwlock_tx *tx, int mode)
{
    struct wwlock_slot *slot;

    for (slot = lock->holders; slot != NULL; slot = slot->next_in_lock) {
        if (slot->tx == tx) {
            continue;
        }
        if (mode == WWLOCK_SHARED && slot->mode == WWLOCK_SHARED) {
            continue;
        }
        if (slot->tx->stamp < tx->stamp) {
            return 1;
        }
    }
    return 0;
}

static int older_waiter(struct wwlock *lock, struct wwlock_tx *tx)
{
    struct wwlock_waiter *waiter;

    for (waiter = lock->waiters; waiter != NULL; waiter = waiter->next) {
        if (waiter->tx != tx && waiter->tx->stamp < tx->stamp) {
            return 1;
        }
    }
    return 0;
}

static void enqueue(struct wwlock *lock, struct wwlock_waiter *waiter)
{
    struct wwlock_waiter **link = &lock->waiters;

    while (*link != NULL && (*link)->tx->stamp < waiter->tx->stamp) {
        link = &(*link)->next;
    }
    waiter->next = *link;
    *link = waiter;
}

static void unlink_waiter(struct wwlock *lock, struct wwlock_waiter *waiter)
{
    struct wwlock_waiter **link = &lock->waiters;

    while (*link != NULL) {
        if (*link == waiter) {
            *link = waiter->next;
            waiter->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

static int mark_younger(struct wwlock *lock, struct wwlock_tx *tx)
{
    struct wwlock_waiter *waiter;
    int marked = 0;

    for (waiter = lock->waiters; waiter != NULL; waiter = waiter->next) {
        if (waiter->tx == tx) {
            continue;
        }
        if (waiter->tx->stamp > tx->stamp && held_count(waiter->tx) > 0) {
            waiter->die = 1;
            marked = 1;
        }
    }
    return marked;
}

static void unlist_tx(struct wwlock_tx *tx, struct wwlock_slot *slot)
{
    struct wwlock_slot **link = &tx->holds;

    while (*link != NULL) {
        if (*link == slot) {
            *link = slot->next_in_tx;
            slot->next_in_tx = NULL;
            return;
        }
        link = &(*link)->next_in_tx;
    }
}

static void attach(struct wwlock *lock, struct wwlock_tx *tx, struct wwlock_slot *slot, int mode)
{
    slot->tx = tx;
    slot->lock = lock;
    slot->mode = mode;
    slot->in_use = 1;
    slot->count = 1;
    slot->free_next = NULL;
    slot->next_in_lock = lock->holders;
    lock->holders = slot;
    slot->next_in_tx = tx->holds;
    tx->holds = slot;
    held_add(tx);
}

static struct wwlock_slot *detach(struct wwlock *lock, struct wwlock_tx *tx)
{
    struct wwlock_slot **link = &lock->holders;

    while (*link != NULL) {
        if ((*link)->tx == tx) {
            struct wwlock_slot *slot = *link;

            *link = slot->next_in_lock;
            slot->next_in_lock = NULL;
            unlist_tx(tx, slot);
            held_sub(tx);
            slot->in_use = 0;
            slot->tx = NULL;
            return slot;
        }
        link = &(*link)->next_in_lock;
    }
    return NULL;
}

static struct wwlock_slot *slot_pop(struct wwlock_class *klass)
{
    struct wwlock_slot *slot;

    free_gate_lock(klass);
    slot = klass->freelist;
    if (slot != NULL) {
        klass->freelist = slot->free_next;
        slot->free_next = NULL;
    }
    free_gate_unlock(klass);
    return slot;
}

static void slot_push(struct wwlock_class *klass, struct wwlock_slot *slot)
{
    free_gate_lock(klass);
    slot->tx = NULL;
    slot->lock = NULL;
    slot->in_use = 0;
    slot->count = 0;
    slot->next_in_lock = NULL;
    slot->next_in_tx = NULL;
    slot->free_next = klass->freelist;
    klass->freelist = slot;
    free_gate_unlock(klass);
}

static struct wwlock_slot *slot_new(struct wwlock_class *klass)
{
    struct wwlock_slot *slot = slot_pop(klass);

    if (slot != NULL) {
        return slot;
    }
    slot = (struct wwlock_slot *)malloc(sizeof(*slot));
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
    return slot;
}

static int slot_is_inline(const struct wwlock *lock, const struct wwlock_slot *slot)
{
    return slot >= lock->inline_store && slot < lock->inline_store + WWLOCK_INLINE_HOLDERS;
}

static struct wwlock_slot *inline_free(struct wwlock *lock)
{
    int i;

    for (i = 0; i < WWLOCK_INLINE_HOLDERS; i++) {
        if (!lock->inline_store[i].in_use) {
            return &lock->inline_store[i];
        }
    }
    return NULL;
}

static void recycle(struct wwlock *lock, struct wwlock_slot *slot)
{
    if (slot == NULL || slot_is_inline(lock, slot)) {
        return;
    }
    slot_push(lock->klass, slot);
}

static void wake_waiters(struct wwlock *lock)
{
    wwlock_park_t park = lock->klass->park;

    park.wake((uint32_t *)&lock->seq, WWLOCK_WAKE_ALL, park.user);
}

static void sleep_seq(struct wwlock *lock, uint32_t expect)
{
    wwlock_park_t park = lock->klass->park;

    park.wait((uint32_t *)&lock->seq, expect, park.user);
}

static void end_attempt(struct wwlock_tx *tx)
{
    if (tx->keep || held_count(tx) != 0) {
        return;
    }
    tx->active = 0;
    tx->klass = NULL;
    tx->holds = NULL;
}

static int unlock_one(struct wwlock *lock, struct wwlock_tx *tx)
{
    struct wwlock_slot *slot;
    int waiting;

    gate_lock(lock);
    slot = find_holder(lock, tx);
    if (slot == NULL) {
        gate_unlock(lock);
        return WWLOCK_INVAL;
    }
    if (slot->count > 1) {
        slot->count--;
        gate_unlock(lock);
        return WWLOCK_OK;
    }
    slot = detach(lock, tx);
    if (tx->pinned == lock) {
        tx->pinned = NULL;
    }
    waiting = lock->waiters != NULL;
    if (waiting) {
        bump(lock);
    }
    gate_unlock(lock);
    if (waiting) {
        wake_waiters(lock);
    }
    recycle(lock, slot);
    end_attempt(tx);
    return WWLOCK_OK;
}

static void unlock_all(struct wwlock_tx *tx)
{
    while (tx->holds != NULL) {
        unlock_one(tx->holds->lock, tx);
    }
}

static int lock_with(struct wwlock *lock, struct wwlock_tx *tx, int mode)
{
    struct wwlock_waiter self;
    struct wwlock_slot *spare = NULL;
    int queued = 0;

    memset(&self, 0, sizeof(self));
    self.tx = tx;
    self.mode = mode;

    for (;;) {
        struct wwlock_slot *mine;
        int upgrade;
        int blocked;
        int wake = 0;
        uint32_t expect;

        gate_lock(lock);
        if (queued && self.die && held_count(tx) > 0) {
            unlink_waiter(lock, &self);
            gate_unlock(lock);
            recycle(lock, spare);
            return WWLOCK_DIE;
        }

        mine = find_holder(lock, tx);
        if (mine != NULL && covers(mine->mode, mode)) {
            if (queued) {
                unlink_waiter(lock, &self);
            }
            if (tx->pinned == lock) {
                tx->pinned = NULL;
                gate_unlock(lock);
                recycle(lock, spare);
                return WWLOCK_ALREADY;
            }
            mine->count++;
            gate_unlock(lock);
            recycle(lock, spare);
            return WWLOCK_OK;
        }

        upgrade = mine != NULL && mine->mode == WWLOCK_SHARED && mode == WWLOCK_EXCLUSIVE;
        blocked = upgrade ? others_block_exclusive(lock, tx) : conflicts(lock, tx, mode);
        if (!blocked && !older_waiter(lock, tx)) {
            if (upgrade) {
                mine->mode = WWLOCK_EXCLUSIVE;
                mine->count++;
            } else if (mine == NULL) {
                struct wwlock_slot *use = NULL;

                use = inline_free(lock);
                if (use == NULL && spare != NULL) {
                    use = spare;
                    spare = NULL;
                }
                if (use == NULL) {
                    gate_unlock(lock);
                    spare = slot_new(lock->klass);
                    if (spare == NULL) {
                        gate_lock(lock);
                        if (queued) {
                            unlink_waiter(lock, &self);
                        }
                        gate_unlock(lock);
                        return WWLOCK_NOMEM;
                    }
                    continue;
                }
                attach(lock, tx, use, mode);
            }
            if (queued) {
                unlink_waiter(lock, &self);
                queued = 0;
            }
            wake = mark_younger(lock, tx);
            if (wake) {
                bump(lock);
            }
            gate_unlock(lock);
            if (wake) {
                wake_waiters(lock);
            }
            recycle(lock, spare);
            return WWLOCK_OK;
        }

        if (held_count(tx) > 0 && (older_holder(lock, tx, mode) || older_waiter(lock, tx))) {
            if (queued) {
                unlink_waiter(lock, &self);
            }
            gate_unlock(lock);
            recycle(lock, spare);
            return WWLOCK_DIE;
        }

        if (!queued) {
            enqueue(lock, &self);
            queued = 1;
            wake = mark_younger(lock, tx);
        }
        if (wake) {
            bump(lock);
        }
        expect = load_seq(lock);
        gate_unlock(lock);
        if (wake) {
            wake_waiters(lock);
        }
        sleep_seq(lock, expect);
    }
}

static struct wwlock_tx *begin_attempt(struct wwlock_class *klass)
{
    struct wwlock_tx *tx = &tls_tx;

    if (tx->active) {
        if (tx->klass != klass) {
            return NULL;
        }
        return tx;
    }
    memset(tx, 0, sizeof(*tx));
    atomic_init(&tx->acquired, 0);
    tx->klass = klass;
    tx->stamp = atomic_fetch_add_explicit(&klass->stamp, 1, memory_order_relaxed) + 1;
    tx->active = 1;
    return tx;
}

int wwlock_class_init(wwlock_class_t **klass, const wwlock_park_t *park)
{
    wwlock_class_t *created;

    if (klass == NULL || park == NULL || park->wait == NULL || park->wake == NULL) {
        return WWLOCK_INVAL;
    }
    created = (wwlock_class_t *)calloc(1, sizeof(*created));
    if (created == NULL) {
        return WWLOCK_NOMEM;
    }
    atomic_init(&created->stamp, 0);
    atomic_init(&created->free_gate, 0);
    atomic_init(&created->nlocks, 0);
    created->park = *park;
    *klass = created;
    return WWLOCK_OK;
}

int wwlock_class_deinit(wwlock_class_t *klass)
{
    struct wwlock_slot *slot;

    if (klass == NULL) {
        return WWLOCK_INVAL;
    }
    if (atomic_load_explicit(&klass->nlocks, memory_order_acquire) != 0) {
        return WWLOCK_BUSY;
    }
    free_gate_lock(klass);
    slot = klass->freelist;
    klass->freelist = NULL;
    free_gate_unlock(klass);
    while (slot != NULL) {
        struct wwlock_slot *next = slot->free_next;

        free(slot);
        slot = next;
    }
    free(klass);
    return WWLOCK_OK;
}

int wwlock_init(wwlock_t **lock, wwlock_class_t *klass)
{
    wwlock_t *created;

    if (lock == NULL || klass == NULL) {
        return WWLOCK_INVAL;
    }
    created = (wwlock_t *)calloc(1, sizeof(*created));
    if (created == NULL) {
        return WWLOCK_NOMEM;
    }
    created->klass = klass;
    atomic_init(&created->gate, 0);
    atomic_init(&created->seq, 0);
    atomic_fetch_add_explicit(&klass->nlocks, 1, memory_order_release);
    *lock = created;
    return WWLOCK_OK;
}

int wwlock_deinit(wwlock_t *lock)
{
    if (lock == NULL || lock->klass == NULL) {
        return WWLOCK_INVAL;
    }
    gate_lock(lock);
    if (lock->holders != NULL || lock->waiters != NULL) {
        gate_unlock(lock);
        return WWLOCK_BUSY;
    }
    gate_unlock(lock);
    atomic_fetch_sub_explicit(&lock->klass->nlocks, 1, memory_order_release);
    free(lock);
    return WWLOCK_OK;
}

int wwlock_lock_mode(wwlock_t *lock, int mode)
{
    struct wwlock_tx *tx;
    int rc;

    if (lock == NULL || lock->klass == NULL) {
        return WWLOCK_INVAL;
    }
    if (mode != WWLOCK_SHARED && mode != WWLOCK_EXCLUSIVE) {
        return WWLOCK_INVAL;
    }
    tx = begin_attempt(lock->klass);
    if (tx == NULL) {
        return WWLOCK_INVAL;
    }
    rc = lock_with(lock, tx, mode);
    if (rc != WWLOCK_DIE) {
        if (rc != WWLOCK_OK && rc != WWLOCK_ALREADY) {
            end_attempt(tx);
        }
        return rc;
    }
    tx->keep = 1;
    unlock_all(tx);
    tx->keep = 0;
    rc = lock_with(lock, tx, mode);
    if (rc == WWLOCK_OK || rc == WWLOCK_ALREADY) {
        tx->pinned = lock;
        if (tls_in_call) {
            longjmp(tls_restart, 1);
        }
        return WWLOCK_DIE;
    }
    end_attempt(tx);
    return rc;
}

int wwlock_call(int (*fn)(void *arg), void *arg)
{
    int (*volatile work)(void *) = fn;
    void *volatile data = arg;
    int rc;

    if (work == NULL) {
        return WWLOCK_INVAL;
    }
    if (tls_in_call) {
        return work(data);
    }
    if (tls_tx.active && held_count(&tls_tx) > 0) {
        return WWLOCK_INVAL;
    }
    tls_in_call = 1;
    for (;;) {
        if (setjmp(tls_restart) != 0) {
            continue;
        }
        rc = work(data);
        tls_in_call = 0;
        return rc;
    }
}

int wwlock_unlock(wwlock_t *lock)
{
    struct wwlock_tx *tx = &tls_tx;

    if (lock == NULL || !tx->active || tx->klass != lock->klass) {
        return WWLOCK_INVAL;
    }
    return unlock_one(lock, tx);
}
