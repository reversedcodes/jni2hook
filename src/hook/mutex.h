#ifndef JNI2HOOK_HOOK_MUTEX_H
#define JNI2HOOK_HOOK_MUTEX_H

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION hook_mutex;
static inline void hook_mutex_init(hook_mutex *m)    { InitializeCriticalSection(m); }
static inline void hook_mutex_destroy(hook_mutex *m) { DeleteCriticalSection(m); }
static inline void hook_mutex_lock(hook_mutex *m)    { EnterCriticalSection(m); }
static inline void hook_mutex_unlock(hook_mutex *m)  { LeaveCriticalSection(m); }
#else
#include <pthread.h>
typedef pthread_mutex_t hook_mutex;
static inline void hook_mutex_init(hook_mutex *m)
{
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attributes);
    pthread_mutexattr_destroy(&attributes);
}
static inline void hook_mutex_destroy(hook_mutex *m) { pthread_mutex_destroy(m); }
static inline void hook_mutex_lock(hook_mutex *m)    { pthread_mutex_lock(m); }
static inline void hook_mutex_unlock(hook_mutex *m)  { pthread_mutex_unlock(m); }
#endif

#endif
