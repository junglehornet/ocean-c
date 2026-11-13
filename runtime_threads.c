// Native thread backend embedded in the compiler and linked only when needed.
// All calls into Ocean use explicit C ABI bridges emitted in the support module.
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <limits.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Ocean's diagnostic runtime uses dprintf; the Windows CRT exposes vfprintf
// instead. Keep the compatibility symbol local to threading-enabled programs.
int dprintf(int descriptor, const char *format, ...) {
    va_list arguments; va_start(arguments, format);
    int result = vfprintf(descriptor == 2 ? stderr : stdout, format, arguments);
    va_end(arguments); return result;
}
typedef CRITICAL_SECTION Lock;
typedef CONDITION_VARIABLE Cond;
typedef HANDLE Handle;
static void lock_init(Lock *p) { InitializeCriticalSection(p); }
static void lock(Lock *p) { EnterCriticalSection(p); }
static void unlock(Lock *p) { LeaveCriticalSection(p); }
static void cond_init(Cond *p) { InitializeConditionVariable(p); }
static void broadcast(Cond *p) { WakeAllConditionVariable(p); }
static void cond_wait(Cond *p, Lock *m) {
    if (!SleepConditionVariableCS(p, m, 10) && GetLastError() != ERROR_TIMEOUT) abort();
}
static void join(Handle h) { if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) abort(); CloseHandle(h); }
#else
#include <pthread.h>
#include <time.h>
#include <errno.h>
typedef pthread_mutex_t Lock;
typedef pthread_cond_t Cond;
typedef pthread_t Handle;
static void checked(int status) { if (status) abort(); }
static void lock_init(Lock *p) { checked(pthread_mutex_init(p, NULL)); }
static void lock(Lock *p) { checked(pthread_mutex_lock(p)); }
static void unlock(Lock *p) { checked(pthread_mutex_unlock(p)); }
static void cond_init(Cond *p) { checked(pthread_cond_init(p, NULL)); }
static void broadcast(Cond *p) { checked(pthread_cond_broadcast(p)); }
static void cond_wait(Cond *p, Lock *m) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) abort();
    deadline.tv_nsec += 10000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    int status = pthread_cond_timedwait(p, m, &deadline);
    if (status && status != ETIMEDOUT) abort();
}
static void join(Handle h) { checked(pthread_join(h, NULL)); }
#endif

extern void ocean_runtime_thread_invoke(void *code, void *environment);
extern void ocean_runtime_thread_return(void);
extern int ocean_runtime_can_cancel(void);
extern void ocean_runtime_closure_init(void);

typedef struct Thread Thread;
struct Thread {
    int64_t id;
    Handle handle;
    void *code, *environment;
    int done, yielded, joining, joined;
    _Atomic int cancelled;
    jmp_buf root;
    Thread *next;
};
static Lock registry;
static Cond changed;
static Thread main_thread;
static Thread *tail = &main_thread;
static int64_t next_id = 1;
static int shutting_down;
static _Thread_local Thread *self;
static _Thread_local int returning;

static void cleanup_sync(void);
static void shutdown_threads(void);
static void initialize(void) {
    lock_init(&registry);
    cond_init(&changed);
    self = &main_thread;
    // atexit runs in reverse order: join workers before closure/global cleanup.
    ocean_runtime_closure_init();
    if (atexit(shutdown_threads)) abort();
}
#if defined(_WIN32)
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK initialize_once(PINIT_ONCE p, PVOID arg, PVOID *context) {
    (void)p; (void)arg; (void)context; initialize(); return TRUE;
}
static void ensure(void) { if (!InitOnceExecuteOnce(&once, initialize_once, NULL, NULL)) abort(); }
#else
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void ensure(void) { checked(pthread_once(&once, initialize)); }
#endif

static Thread *find(int64_t id) {
    for (Thread *t = &main_thread; t; t = t->next) if (t->id == id) return t;
    return NULL;
}
static int cancelled(void) {
    return self && !returning && atomic_load_explicit(&self->cancelled, memory_order_acquire) && ocean_runtime_can_cancel();
}
void ocean_threads_checkpoint(void) {
    ensure();
    if (cancelled()) ocean_runtime_thread_return();
}
void ocean_threads_begin_return(void) { ensure(); returning = 1; }
static void complete(Thread *t) {
    lock(&registry);
    t->done = 1;
    t->yielded = 0;
    broadcast(&changed);
    unlock(&registry);
}
_Noreturn void ocean_threads_exit(void) {
    ensure();
    if (!self) abort();
    if (self != &main_thread) longjmp(self->root, 1);
    complete(&main_thread);
#if defined(_WIN32)
    ExitThread(0);
#else
    pthread_exit(NULL);
#endif
    abort();
}
#if defined(_WIN32)
static DWORD WINAPI start(void *argument) {
#else
static void *start(void *argument) {
#endif
    Thread *t = argument;
    self = t;
    // Pair with Spawn's registry lock: native handle publication completes
    // before user code or cancellation can inspect this record.
    lock(&registry); unlock(&registry);
    if (!setjmp(t->root)) {
        ocean_threads_checkpoint();
        ocean_runtime_thread_invoke(t->code, t->environment);
    }
    complete(t);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}
int64_t ocean_threads_spawn(void *code, void *environment) {
    ensure();
    Thread *t = calloc(1, sizeof(*t));
    if (!t) return -1;
    lock(&registry);
    if (shutting_down || next_id == INT64_MAX) { unlock(&registry); free(t); return -1; }
    t->code = code;
    t->environment = environment;
    t->id = next_id;
    atomic_init(&t->cancelled, 0);
#if defined(_WIN32)
    t->handle = CreateThread(NULL, 0, start, t, 0, NULL);
    int failed = t->handle == NULL;
#else
    // Darwin defaults new pthreads to 512 KiB, which is too small for native
    // Ocean parsing and other recursive code. Preserve larger host defaults.
    pthread_attr_t attributes;
    int failed = pthread_attr_init(&attributes);
    if (!failed) {
        size_t stack_size = 0;
        failed = pthread_attr_getstacksize(&attributes, &stack_size);
        const size_t minimum_stack = 8 * 1024 * 1024;
        if (!failed && stack_size < minimum_stack)
            failed = pthread_attr_setstacksize(&attributes, minimum_stack);
        if (!failed) failed = pthread_create(&t->handle, &attributes, start, t);
        pthread_attr_destroy(&attributes);
    }
#endif
    if (failed) { unlock(&registry); free(t); return -1; }
    next_id++;
    tail->next = t;
    tail = t;
    int64_t id = t->id;
    unlock(&registry);
    return id;
}
int64_t ocean_threads_current(void) { ensure(); return self ? self->id : -1; }
int32_t ocean_threads_is_done(int64_t id) {
    ensure(); lock(&registry);
    Thread *t = find(id);
    int result = t ? t->done : -1;
    unlock(&registry); return result;
}
int32_t ocean_threads_await(int64_t id) {
    ensure(); lock(&registry);
    Thread *t = find(id);
    if (!t || t == self) { unlock(&registry); return t ? -2 : -1; }
    while (!t->done || t->joining) {
        if (cancelled()) { unlock(&registry); ocean_runtime_thread_return(); }
        cond_wait(&changed, &registry);
    }
    if (t->joined || t == &main_thread) { unlock(&registry); ocean_threads_checkpoint(); return 0; }
    t->joining = 1;
    unlock(&registry);
    join(t->handle);
    lock(&registry);
    t->joining = 0; t->joined = 1;
    broadcast(&changed); unlock(&registry);
    ocean_threads_checkpoint(); return 0;
}
void ocean_threads_yield(void) {
    ensure();
    if (!self) abort();
    lock(&registry);
    self->yielded = 1;
    while (self->yielded && !cancelled()) cond_wait(&changed, &registry);
    self->yielded = 0;
    unlock(&registry);
    ocean_threads_checkpoint();
}
int32_t ocean_threads_wake(int64_t id) {
    ensure(); lock(&registry);
    Thread *t = find(id);
    if (!t) { unlock(&registry); return -1; }
    if (t->yielded) { t->yielded = 0; broadcast(&changed); }
    unlock(&registry); return 0;
}
int32_t ocean_threads_kill(int64_t id) {
    ensure(); lock(&registry);
    Thread *t = find(id);
    if (!t) { unlock(&registry); return -1; }
    if (!t->done) atomic_store_explicit(&t->cancelled, 1, memory_order_release);
    broadcast(&changed);
    unlock(&registry);
    ocean_threads_checkpoint(); return 0;
}
static void shutdown_threads(void) {
    lock(&registry);
    if (self != &main_thread && !main_thread.done) {
        // Unrecovered worker panic: terminate without freeing shared storage.
        unlock(&registry); _Exit(1);
    }
    shutting_down = 1;
    main_thread.done = 1;
    broadcast(&changed);
    Thread *t = main_thread.next;
    unlock(&registry);
    returning = 1;
    for (; t; t = t->next) if (t != self) ocean_threads_await(t->id);
    // IDs remain valid throughout execution. Retire native handles and records
    // only after every worker has stopped and no Ocean code can observe them.
    t = main_thread.next;
    while (t) {
        Thread *next = t->next;
#if defined(_WIN32)
        if (t == self && !t->joined) CloseHandle(t->handle);
#endif
        free(t); t = next;
    }
    main_thread.next = NULL;
    cleanup_sync();
}

// Both public synchronization types contain a single zero-initialized word.
// Lazy publication uses a C atomic CAS; all subsequent accesses are native.
typedef struct Sync Sync;
static Sync *sync_head;
struct Sync {
    Sync *next;
    Lock gate;
    Cond changed;
    int locked;
    int64_t count, waiters;
};
static Sync *synchronizer(void *slot) {
    _Atomic(uintptr_t) *location = slot;
    uintptr_t present = atomic_load_explicit(location, memory_order_acquire);
    if (present) return (Sync *)present;
    Sync *value = calloc(1, sizeof(*value));
    if (!value) abort();
    lock_init(&value->gate); cond_init(&value->changed);
    uintptr_t empty = 0;
    if (!atomic_compare_exchange_strong_explicit(location, &empty, (uintptr_t)value, memory_order_acq_rel, memory_order_acquire)) {
#if defined(_WIN32)
        DeleteCriticalSection(&value->gate);
#else
        checked(pthread_mutex_destroy(&value->gate)); checked(pthread_cond_destroy(&value->changed));
#endif
        free(value); return (Sync *)empty;
    }
    lock(&registry); value->next = sync_head; sync_head = value; unlock(&registry);
    return value;
}
void ocean_threads_mutex_lock(void *slot) {
    ensure();
    Sync *s = synchronizer(slot);
    lock(&s->gate);
    while (s->locked) {
        if (cancelled()) { unlock(&s->gate); ocean_runtime_thread_return(); }
        cond_wait(&s->changed, &s->gate);
    }
    // Cancel before taking ownership, so an unregistered defer cannot strand it.
    if (cancelled()) { unlock(&s->gate); ocean_runtime_thread_return(); }
    s->locked = 1;
    unlock(&s->gate);
}
int32_t ocean_threads_mutex_try_lock(void *slot) {
    ensure(); Sync *s = synchronizer(slot); lock(&s->gate);
    int result = !s->locked;
    if (result) s->locked = 1;
    unlock(&s->gate); return result;
}
int32_t ocean_threads_mutex_unlock(void *slot) {
    ensure(); Sync *s = synchronizer(slot); lock(&s->gate);
    if (!s->locked) { unlock(&s->gate); return -1; }
    s->locked = 0; broadcast(&s->changed); unlock(&s->gate); return 0;
}
int32_t ocean_threads_group_add(void *slot, int64_t delta) {
    ensure(); Sync *s = synchronizer(slot); lock(&s->gate);
    if (delta < -s->count) { unlock(&s->gate); return -1; }
    if (delta > INT64_MAX - s->count) { unlock(&s->gate); return -2; }
    if (delta > 0 && s->count == 0 && s->waiters) { unlock(&s->gate); return -3; }
    s->count += delta;
    if (!s->count) broadcast(&s->changed);
    unlock(&s->gate); return 0;
}
void ocean_threads_group_wait(void *slot) {
    ensure(); Sync *s = synchronizer(slot); lock(&s->gate);
    if (s->count) {
        s->waiters++;
        while (s->count) {
            if (cancelled()) {
                s->waiters--; unlock(&s->gate); ocean_runtime_thread_return();
            }
            cond_wait(&s->changed, &s->gate);
        }
        s->waiters--;
    }
    unlock(&s->gate); ocean_threads_checkpoint();
}

static void cleanup_sync(void) {
    while (sync_head) {
        Sync *s = sync_head; sync_head = s->next;
#if defined(_WIN32)
        DeleteCriticalSection(&s->gate);
#else
        checked(pthread_mutex_destroy(&s->gate)); checked(pthread_cond_destroy(&s->changed));
#endif
        free(s);
    }
}

_Noreturn void ocean_threads_panic_exit(int32_t status) { _Exit(status); }
