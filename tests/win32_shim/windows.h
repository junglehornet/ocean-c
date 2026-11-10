// Test-only Win32 API shim: exercise the Windows runtime branch on POSIX.
// This tests runtime control flow, not the Windows SDK ABI or a real kernel.
#ifndef OCEAN_TEST_WINDOWS_H
#define OCEAN_TEST_WINDOWS_H
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define ERROR_TIMEOUT 1460
#define WAIT_OBJECT_0 0
#define INFINITE 0xffffffffu
typedef int BOOL;
typedef uint32_t DWORD;
typedef void *PVOID;
typedef struct { pthread_mutex_t value; } CRITICAL_SECTION;
typedef struct { pthread_cond_t value; } CONDITION_VARIABLE;
typedef struct { pthread_mutex_t lock; int ready; } INIT_ONCE, *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { PTHREAD_MUTEX_INITIALIZER, 0 }
typedef struct ocean_test_handle { pthread_t thread; DWORD (*entry)(void *); void *argument; } *HANDLE;
static _Thread_local DWORD ocean_test_error;
static void InitializeCriticalSection(CRITICAL_SECTION *m) { if (pthread_mutex_init(&m->value, NULL)) abort(); }
static void DeleteCriticalSection(CRITICAL_SECTION *m) { if (pthread_mutex_destroy(&m->value)) abort(); }
static void EnterCriticalSection(CRITICAL_SECTION *m) { if (pthread_mutex_lock(&m->value)) abort(); }
static void LeaveCriticalSection(CRITICAL_SECTION *m) { if (pthread_mutex_unlock(&m->value)) abort(); }
static void InitializeConditionVariable(CONDITION_VARIABLE *c) { if (pthread_cond_init(&c->value, NULL)) abort(); }
static void WakeAllConditionVariable(CONDITION_VARIABLE *c) { if (pthread_cond_broadcast(&c->value)) abort(); }
static BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *m, DWORD milliseconds) {
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)milliseconds * 1000000;
    deadline.tv_sec += deadline.tv_nsec / 1000000000;
    deadline.tv_nsec %= 1000000000;
    int result = pthread_cond_timedwait(&c->value, &m->value, &deadline);
    ocean_test_error = result == ETIMEDOUT ? ERROR_TIMEOUT : (DWORD)result;
    return result == 0;
}
static DWORD GetLastError(void) { return ocean_test_error; }
static BOOL InitOnceExecuteOnce(PINIT_ONCE once, BOOL (*callback)(PINIT_ONCE, PVOID, PVOID *), PVOID arg, PVOID *context) {
    pthread_mutex_lock(&once->lock);
    if (!once->ready) once->ready = callback(once, arg, context);
    int result = once->ready; pthread_mutex_unlock(&once->lock); return result;
}
static void *ocean_test_start(void *argument) {
    HANDLE h = argument; return (void *)(uintptr_t)h->entry(h->argument);
}
static HANDLE CreateThread(void *attributes, size_t size, DWORD (*entry)(void *), void *argument, DWORD flags, DWORD *id) {
    (void)attributes; (void)size; (void)flags; (void)id;
    HANDLE h = malloc(sizeof(*h)); if (!h) return NULL;
    h->entry = entry; h->argument = argument;
    if (pthread_create(&h->thread, NULL, ocean_test_start, h)) { free(h); return NULL; }
    return h;
}
static DWORD WaitForSingleObject(HANDLE h, DWORD timeout) { (void)timeout; if (pthread_join(h->thread, NULL)) abort(); return WAIT_OBJECT_0; }
static BOOL CloseHandle(HANDLE h) { free(h); return TRUE; }
_Noreturn static void ExitThread(DWORD code) { pthread_exit((void *)(uintptr_t)code); }
#endif
