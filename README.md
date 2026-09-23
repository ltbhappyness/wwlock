# wwlock

Wait-Die locks for one process. Threads may lock in any order. If that order would wait in a circle, the younger thread drops the locks it took in that attempt, sleeps until the lock it needs is held, and the call returns so the thread can take the set again.

The rules are the Wait-Die class from the Linux kernel (`DEFINE_WD_CLASS` in `ww_mutex`). Wound-Wait is not implemented.

Sleep and wake are callbacks. A futex or a condition variable both fit. `src/wwlock.c` does not link libpthread.

`WWLOCK_READ` is shared. Any number of readers may hold one lock. `WWLOCK_WRITE` is exclusive. A writer waits while any reader or writer holds it, and a reader waits while a writer holds it. The same thread may lock again. Each successful lock needs its own unlock. `WWLOCK_ALREADY` means this call did not add a hold, which happens when a restarted attempt finds the lock already held. `examples/read_write.c` is one lock. `examples/two_locks.c` is two locks.

This is not a replacement for `pthread_rwlock` on one lock that is mostly read. That lock stays on one atomic word. wwlock is for one piece of work that holds several locks and cannot rank them in advance.

## License

[MIT](LICENSE). Anyone may use, change, and ship this code. Keep the copyright notice.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/bench_wwlock
```

The default build type is Release.

## Use

```c
wwlock_class_t *klass;
wwlock_t *lock;

wwlock_class_init(&klass, &park);
wwlock_init(&lock, klass);

wwlock_rdlock(lock);   /* other readers may enter, unless an older attempt waits */
wwlock_unlock(lock);
wwlock_wrlock(lock);  /* other readers and writers wait */
wwlock_unlock(lock);
wwlock_lock(lock);     /* same as wwlock_wrlock */
wwlock_unlock(lock);

wwlock_deinit(lock);
wwlock_class_deinit(klass);
```

The calling thread is the transaction. The first lock on a thread starts it. The last `wwlock_unlock` ends it. There is no context object to create at thread start.

A thread that holds nothing and finds the lock busy sleeps inside the wait callback. It does not spin, and it does not return a busy error.

When the thread already holds a lock and waiting would circle, that call drops the earlier holds, sleeps until the requested lock is held, and returns `WWLOCK_DIE`. Do not unlock that lock. Take the set again. Any other return means the lock was not taken: stop, and do not unlock a lock this attempt does not hold.

```c
int rc;

for (;;) {
    rc = wwlock_wrlock(a);
    if (rc == WWLOCK_DIE)
        continue;
    if (rc != WWLOCK_OK && rc != WWLOCK_ALREADY)
        return rc;
    rc = wwlock_wrlock(b);
    if (rc == WWLOCK_DIE)
        continue;
    if (rc != WWLOCK_OK && rc != WWLOCK_ALREADY)
        return rc;
    break;
}
wwlock_unlock(b);
wwlock_unlock(a);
```

`WWLOCK_ALREADY` means the lock is already held and this call did not add an unlock. A second `wwlock_wrlock` on a lock this thread holds returns `WWLOCK_OK` and needs a second `wwlock_unlock`.

When an application and a library both lock, neither should look for `WWLOCK_DIE`. Put the work in `wwlock_call`. A lock inside that function, or inside a library it calls, starts the function again. A nested `wwlock_call` joins the outer one. `examples/app_library.c` is that case.

One thread uses one class at a time. A second class while locks are still held returns `WWLOCK_INVAL`.

## Callbacks

`wait(word, expect, user)` returns immediately when an atomic load of `*word` is not `expect`. Otherwise it sleeps until `wake`. `include/wwlock_futex.h` waits on that word with a Linux futex and wakes every sleeper on it. Use that park in a real process. `test/park_posix.c` is one condition variable for the whole class. It is correct and it will not scale.

`wake` must wake every sleeper on that word. The library passes `WWLOCK_WAKE_ALL`.

## Layout

```
include/wwlock.h        public API
include/wwlock_futex.h  per-word futex park for a real process
src/wwlock.c            Wait-Die
src/wwlock_futex.c
test/park_posix.c       condition-variable park used by the tests
test/test_wwlock.c
test/bench_wwlock.c
examples/read_write.c   one lock, readers and writers
examples/two_locks.c    two locks, opposite order
examples/app_library.c  application and library share wwlock_call
```
