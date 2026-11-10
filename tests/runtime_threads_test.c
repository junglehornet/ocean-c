// This test also builds directly on Windows, without the Ocean CLI driver:
// clang -std=c11 runtime_threads.c tests/runtime_threads_test.c -o threads-test
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#endif

extern int64_t ocean_threads_spawn(void *, void *);
extern int64_t ocean_threads_current(void);
extern int32_t ocean_threads_await(int64_t);
extern int32_t ocean_threads_is_done(int64_t);
extern void ocean_threads_yield(void);
extern int32_t ocean_threads_wake(int64_t);
extern int32_t ocean_threads_kill(int64_t);
extern void ocean_threads_checkpoint(void);
extern void ocean_threads_begin_return(void);
extern _Noreturn void ocean_threads_exit(void);
extern void ocean_threads_mutex_lock(void *);
extern int32_t ocean_threads_mutex_try_lock(void *);
extern int32_t ocean_threads_mutex_unlock(void *);
extern int32_t ocean_threads_group_add(void *, int64_t);
extern void ocean_threads_group_wait(void *);

// Stand-ins for the compiler's C ABI bridges. Ocean-level defer and checkpoint
// insertion behavior is exercised by the standard-library integration suite.
void ocean_runtime_thread_invoke(void *code, void *environment) { ((void (*)(void *))code)(environment); }
void ocean_runtime_thread_return(void) { ocean_threads_begin_return(); ocean_threads_exit(); }
int ocean_runtime_can_cancel(void) { return 1; }
void ocean_runtime_closure_init(void) {}
static void schedule(void) {
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}
static _Atomic(uintptr_t) mutex, group;
static _Atomic int ready;
static int64_t count;
static void counter(void *argument) {
    (void)argument;
    assert(ocean_threads_current() > 0);
    for (int i = 0; i < 1000; i++) {
        ocean_threads_mutex_lock(&mutex);
        count++;
        assert(ocean_threads_mutex_unlock(&mutex) == 0);
    }
    assert(ocean_threads_group_add(&group, -1) == 0);
}
static void parked(void *argument) {
    (void)argument;
    atomic_store(&ready, 1);
    ocean_threads_mutex_lock(&mutex);
    assert(!ocean_threads_mutex_unlock(&mutex));
    ocean_threads_yield();
}
static void forever(void *argument) {
    (void)argument;
    atomic_store(&ready, 1);
    for (;;) ocean_threads_checkpoint();
}
static void early(void *argument) { (void)argument; ocean_runtime_thread_return(); assert(0); }
int main(void) {
    assert(ocean_threads_current() == 0);
    assert(ocean_threads_wake(-1) == -1);
    assert(ocean_threads_kill(INT64_MAX) == -1);
    assert(ocean_threads_is_done(-1) == -1);
    assert(ocean_threads_await(0) == -2);
    int64_t ids[8];
    assert(!ocean_threads_group_add(&group, 8));
    for (int i = 0; i < 8; i++) { ids[i] = ocean_threads_spawn((void *)counter, NULL); assert(ids[i] > 0); }
    ocean_threads_group_wait(&group);
    for (int i = 0; i < 8; i++) {
        assert(!ocean_threads_await(ids[i])); assert(!ocean_threads_await(ids[i]));
        assert(ocean_threads_is_done(ids[i]) == 1); assert(!ocean_threads_kill(ids[i]));
    }
    assert(count == 8000);
    assert(ocean_threads_mutex_try_lock(&mutex));
    assert(!ocean_threads_mutex_try_lock(&mutex));
    int64_t id = ocean_threads_spawn((void *)parked, NULL);
    while (!atomic_load(&ready)) schedule();
    assert(!ocean_threads_wake(id)); // cannot park while main owns mutex
    assert(!ocean_threads_mutex_unlock(&mutex));
    while (!ocean_threads_is_done(id)) { assert(!ocean_threads_wake(id)); schedule(); }
    assert(!ocean_threads_await(id));
    atomic_store(&ready, 0);
    id = ocean_threads_spawn((void *)forever, NULL);
    while (!atomic_load(&ready)) schedule();
    assert(!ocean_threads_kill(id)); assert(!ocean_threads_await(id));
    id = ocean_threads_spawn((void *)early, NULL);
    assert(!ocean_threads_await(id));
    assert(ocean_threads_group_add(&group, -1) == -1);
    assert(ocean_threads_mutex_unlock(&mutex) == -1);
    puts("native thread runtime passed");
}
