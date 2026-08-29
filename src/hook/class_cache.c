#include "class_cache.h"

#include "class_watch.h"
#include "mutex.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    jclass         klass;
    char          *class_name;
    unsigned char *bytes;
    jint           size;
} cached_class;

static hook_mutex    g_lock;
static bool          g_lock_ready = false;
static cached_class *g_entries    = NULL;
static size_t        g_count      = 0;
static size_t        g_capacity   = 0;

/* A global ref, not the caller's local one: the callback may run on a thread
   that never created that local ref, where using it would be undefined. */
static jclass      g_capture_class = NULL;
static const char *g_capture_name  = NULL;
static bool        g_captured      = false;
static bool        g_started       = false;
static bool        g_watch_events  = false;

/* The recursive mutex matters: class_cache_ensure holds it across
   RetransformClasses, and the callback that lands inside that call runs on the
   very same thread. Other threads loading classes at the same time block here
   for the duration, which is what stops them from tearing the entry array while
   it is being grown. They wait in a JVMTI callback with the thread in native,
   so the VM can still reach a safepoint and the retransform completes. */
static void lock_cache(void)
{
    if (!g_lock_ready)
    {
        hook_mutex_init(&g_lock);
        g_lock_ready = true;
    }
    hook_mutex_lock(&g_lock);
}

static void unlock_cache(void)
{
    hook_mutex_unlock(&g_lock);
}

static cached_class *find_entry(JNIEnv *env, jclass klass)
{
    if (env == NULL || klass == NULL)
        return NULL;

    for (size_t i = 0; i < g_count; i++)
    {
        if ((*env)->IsSameObject(env, g_entries[i].klass, klass) == JNI_TRUE)
            return &g_entries[i];
    }
    return NULL;
}

static void release_entry(cached_class *entry)
{
    JNIEnv *env = jvm_env();
    if (env != NULL && entry->klass != NULL)
        (*env)->DeleteGlobalRef(env, entry->klass);
    free(entry->class_name);
    free(entry->bytes);
    memset(entry, 0, sizeof(*entry));
}

static bool store(JNIEnv *env, jclass klass, const char *class_name,
                  const unsigned char *bytes, jint size)
{
    if (env == NULL || klass == NULL || size < 0)
        return false;
    if (find_entry(env, klass) != NULL)
        return true;

    if (g_count == g_capacity)
    {
        const size_t capacity = g_capacity ? g_capacity * 2 : 16;
        cached_class *grown = realloc(g_entries, capacity * sizeof(*grown));
        if (grown == NULL)
            return false;
        g_entries  = grown;
        g_capacity = capacity;
    }

    jclass owned = (*env)->NewGlobalRef(env, klass);
    if (owned == NULL)
        return false;

    char *name_copy = NULL;
    if (class_name != NULL)
    {
        const size_t length = strlen(class_name);
        name_copy = malloc(length + 1);
        if (name_copy == NULL)
        {
            (*env)->DeleteGlobalRef(env, owned);
            return false;
        }
        memcpy(name_copy, class_name, length + 1);
    }

    unsigned char *byte_copy = malloc((size_t)size ? (size_t)size : 1);
    if (byte_copy == NULL)
    {
        (*env)->DeleteGlobalRef(env, owned);
        free(name_copy);
        return false;
    }
    memcpy(byte_copy, bytes, (size_t)size);

    g_entries[g_count].klass      = owned;
    g_entries[g_count].class_name = name_copy;
    g_entries[g_count].bytes      = byte_copy;
    g_entries[g_count].size       = size;
    g_count++;
    return true;
}

static void JNICALL on_class_file_load(jvmtiEnv *jvmti,
                                       JNIEnv *env,
                                       jclass class_being_redefined,
                                       jobject loader,
                                       const char *name,
                                       jobject protection_domain,
                                       jint class_data_len,
                                       const unsigned char *class_data,
                                       jint *new_class_data_len,
                                       unsigned char **new_class_data)
{
    (void)jvmti;
    (void)protection_domain;

    /* Leave the class alone: we only listen, the rewrite goes through
       RedefineClasses so that it also applies to already running code. */
    *new_class_data_len = 0;
    *new_class_data     = NULL;

    if (class_data == NULL || env == NULL)
        return;

    if (class_being_redefined != NULL)
    {
        lock_cache();

        /* Both halves are needed, and each covers what the other cannot.
         *
         * The name alone is not an identity: two loaders may each define a class of
         * that name, and a modded game has plenty of loaders.
         *
         * class_being_redefined alone is worse. A class loaded while a retransform
         * is in flight is reported with class_being_redefined still pointing at the
         * class being retransformed, not NULL -- java/lang/Module$ReflectionData
         * arrives that way in the middle of retransforming a class in a named
         * module. Matching on identity alone therefore cached a JDK class's bytes
         * under our target and the original body was gone for good. */
        if (g_capture_class != NULL && name != NULL && g_capture_name != NULL &&
            strcmp(name, g_capture_name) == 0 &&
            (*env)->IsSameObject(env, class_being_redefined, g_capture_class) == JNI_TRUE)
        {
            if (store(env, class_being_redefined, name, class_data, class_data_len))
                g_captured = true;
        }

        unlock_cache();
        return;
    }

    class_watch_on_class_file_load(env, loader, class_data_len, class_data);
}

static void JNICALL on_class_prepare(jvmtiEnv *jvmti, JNIEnv *env, jthread thread,
                                     jclass klass)
{
    (void)thread;
    class_watch_on_class_prepare(jvmti, env, klass);
}

bool class_cache_start(void)
{
    jvmtiEnv *jvmti = jvm_jvmti();
    if (jvmti == NULL)
        return false;

    lock_cache();
    if (g_started)
    {
        unlock_cache();
        return true;
    }

    jvmtiEventCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.ClassFileLoadHook = on_class_file_load;
    callbacks.ClassPrepare = on_class_prepare;

    if ((*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks)) != JVMTI_ERROR_NONE)
    {
        unlock_cache();
        return false;
    }

    g_started = true;
    unlock_cache();
    return true;
}

void class_cache_stop(void)
{
    jvmtiEnv *jvmti = jvm_jvmti();

    lock_cache();
    if (jvmti != NULL && g_started)
    {
        (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                           JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
        (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                           JVMTI_EVENT_CLASS_PREPARE, NULL);
        jvmtiEventCallbacks callbacks;
        memset(&callbacks, 0, sizeof(callbacks));
        (*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks));
    }
    g_started = false;
    g_captured = false;
    g_watch_events = false;
    unlock_cache();
}

bool class_cache_set_watch_events(bool enabled)
{
    jvmtiEnv *jvmti = jvm_jvmti();
    if (jvmti == NULL)
        return false;

    lock_cache();
    if (!g_started)
    {
        unlock_cache();
        return false;
    }
    if (g_watch_events == enabled)
    {
        unlock_cache();
        return true;
    }

    jvmtiError error = JVMTI_ERROR_NONE;
    if (enabled)
    {
        error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                                                   JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
        if (error == JVMTI_ERROR_NONE)
        {
            error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                                                       JVMTI_EVENT_CLASS_PREPARE, NULL);
        }
        if (error != JVMTI_ERROR_NONE)
        {
            (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                               JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
        }
    }
    else
    {
        error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                                   JVMTI_EVENT_CLASS_PREPARE, NULL);
        if (g_capture_class == NULL)
        {
            const jvmtiError file_error =
                (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                                   JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
            if (error == JVMTI_ERROR_NONE)
                error = file_error;
        }
    }

    if (error == JVMTI_ERROR_NONE)
        g_watch_events = enabled;
    unlock_cache();
    return error == JVMTI_ERROR_NONE;
}

bool class_cache_ensure(jclass klass, const char *class_name, jvmtiError *out_error)
{
    if (out_error != NULL)
        *out_error = JVMTI_ERROR_NONE;

    if (klass == NULL)
        return false;

    JNIEnv *env = jvm_env();
    jvmtiEnv *jvmti = jvm_jvmti();
    if (env == NULL || jvmti == NULL)
        return false;

    lock_cache();

    if (find_entry(env, klass) != NULL)
    {
        unlock_cache();
        return true;
    }

    if (!class_cache_start())
    {
        unlock_cache();
        return false;
    }

    jboolean modifiable = JNI_FALSE;
    jvmtiError error = (*jvmti)->IsModifiableClass(jvmti, klass, &modifiable);
    if (error != JVMTI_ERROR_NONE || modifiable != JNI_TRUE)
    {
        if (out_error != NULL)
            *out_error = error != JVMTI_ERROR_NONE ? error : JVMTI_ERROR_UNMODIFIABLE_CLASS;
        unlock_cache();
        return false;
    }

    g_capture_class = (*env)->NewGlobalRef(env, klass);
    g_capture_name  = class_name;
    g_captured      = false;
    if (g_capture_class == NULL)
    {
        if (out_error != NULL)
            *out_error = JVMTI_ERROR_OUT_OF_MEMORY;
        unlock_cache();
        return false;
    }

    error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                                               JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    if (error == JVMTI_ERROR_NONE)
    {
        error = (*jvmti)->RetransformClasses(jvmti, 1, &klass);
        if (!g_watch_events)
            (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                               JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    }

    (*env)->DeleteGlobalRef(env, g_capture_class);
    g_capture_class = NULL;
    g_capture_name  = NULL;

    if (error != JVMTI_ERROR_NONE)
    {
        if (out_error != NULL)
            *out_error = error;
        unlock_cache();
        return false;
    }

    const bool captured = g_captured && find_entry(env, klass) != NULL;
    if (!captured && out_error != NULL)
        *out_error = JVMTI_ERROR_INTERNAL;

    unlock_cache();
    return captured;
}

const unsigned char *class_cache_get(JNIEnv *env, jclass klass, jint *out_size)
{
    lock_cache();

    const cached_class *entry = find_entry(env, klass);
    const unsigned char *bytes = NULL;
    if (entry != NULL)
    {
        if (out_size != NULL)
            *out_size = entry->size;
        bytes = entry->bytes;
    }

    unlock_cache();
    return bytes;
}

void class_cache_forget(JNIEnv *env, jclass klass)
{
    lock_cache();

    cached_class *entry = find_entry(env, klass);
    if (entry != NULL)
    {
        const size_t index = (size_t)(entry - g_entries);
        const size_t tail  = g_count - index - 1;
        release_entry(entry);
        if (tail != 0)
            memmove(entry, entry + 1, tail * sizeof(*entry));
        g_count--;
    }

    unlock_cache();
}

void class_cache_clear(void)
{
    lock_cache();

    for (size_t i = 0; i < g_count; i++)
        release_entry(&g_entries[i]);
    free(g_entries);
    g_entries  = NULL;
    g_count    = 0;
    g_capacity = 0;
    g_captured = false;

    unlock_cache();
}

size_t class_cache_size(void)
{
    lock_cache();
    const size_t count = g_count;
    unlock_cache();
    return count;
}
