#include "class_cache.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    char          *class_name;
    unsigned char *bytes;
    jint           size;
} cached_class;

static cached_class *g_entries  = NULL;
static size_t        g_count    = 0;
static size_t        g_capacity = 0;

static const char *g_capture_name = NULL;
static bool        g_captured     = false;
static bool        g_started      = false;

static cached_class *find_entry(const char *class_name)
{
    for (size_t i = 0; i < g_count; i++)
    {
        if (strcmp(g_entries[i].class_name, class_name) == 0)
            return &g_entries[i];
    }
    return NULL;
}

static bool store(const char *class_name, const unsigned char *bytes, jint size)
{
    if (find_entry(class_name) != NULL)
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

    const size_t length = strlen(class_name);
    char *name_copy = malloc(length + 1);
    if (name_copy == NULL)
        return false;
    memcpy(name_copy, class_name, length + 1);

    unsigned char *byte_copy = malloc((size_t)size ? (size_t)size : 1);
    if (byte_copy == NULL)
    {
        free(name_copy);
        return false;
    }
    memcpy(byte_copy, bytes, (size_t)size);

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
    (void)env;
    (void)class_being_redefined;
    (void)loader;
    (void)protection_domain;

    /* Leave the class alone: we only listen, the rewrite goes through
       RedefineClasses so that it also applies to already running code. */
    *new_class_data_len = 0;
    *new_class_data     = NULL;

    if (g_capture_name == NULL || name == NULL || class_data == NULL)
        return;
    if (strcmp(name, g_capture_name) != 0)
        return;

    if (store(name, class_data, class_data_len))
        g_captured = true;
}

bool class_cache_start(void)
{
    jvmtiEnv *jvmti = jvm_jvmti();
    if (jvmti == NULL)
        return false;
    if (g_started)
        return true;

    jvmtiEventCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.ClassFileLoadHook = on_class_file_load;

    if ((*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks)) != JVMTI_ERROR_NONE)
        return false;

    g_started = true;
    return true;
}

void class_cache_stop(void)
{
    jvmtiEnv *jvmti = jvm_jvmti();
    if (jvmti != NULL && g_started)
    {
        (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                           JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
        jvmtiEventCallbacks callbacks;
        memset(&callbacks, 0, sizeof(callbacks));
        (*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks));
    }
    g_started = false;
}

bool class_cache_ensure(jclass klass, const char *class_name, jvmtiError *out_error)
{
    if (out_error != NULL)
        *out_error = JVMTI_ERROR_NONE;

    if (class_name == NULL || klass == NULL)
        return false;
    if (find_entry(class_name) != NULL)
        return true;

    jvmtiEnv *jvmti = jvm_jvmti();
    if (jvmti == NULL || !class_cache_start())
        return false;

    jboolean modifiable = JNI_FALSE;
    jvmtiError error = (*jvmti)->IsModifiableClass(jvmti, klass, &modifiable);
    if (error != JVMTI_ERROR_NONE || modifiable != JNI_TRUE)
    {
        if (out_error != NULL)
            *out_error = error != JVMTI_ERROR_NONE ? error : JVMTI_ERROR_UNMODIFIABLE_CLASS;
        return false;
    }

    g_capture_name = class_name;
    g_captured     = false;

    error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                                               JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    if (error == JVMTI_ERROR_NONE)
    {
        error = (*jvmti)->RetransformClasses(jvmti, 1, &klass);
        (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
                                           JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    }

    g_capture_name = NULL;

    if (error != JVMTI_ERROR_NONE)
    {
        if (out_error != NULL)
            *out_error = error;
        return false;
    }

    if (!g_captured || find_entry(class_name) == NULL)
    {
        if (out_error != NULL)
            *out_error = JVMTI_ERROR_INTERNAL;
        return false;
    }

    return true;
}

const unsigned char *class_cache_get(const char *class_name, jint *out_size)
{
    const cached_class *entry = find_entry(class_name);
    if (entry == NULL)
        return NULL;
    if (out_size != NULL)
        *out_size = entry->size;
    return entry->bytes;
}

void class_cache_forget(const char *class_name)
{
    cached_class *entry = find_entry(class_name);
    if (entry == NULL)
        return;

    free(entry->class_name);
    free(entry->bytes);

    const size_t index = (size_t)(entry - g_entries);
    const size_t tail  = g_count - index - 1;
    if (tail != 0)
        memmove(entry, entry + 1, tail * sizeof(*entry));
    g_count--;
}

void class_cache_clear(void)
{
    for (size_t i = 0; i < g_count; i++)
    {
        free(g_entries[i].class_name);
        free(g_entries[i].bytes);
    }
    free(g_entries);
    g_entries  = NULL;
    g_count    = 0;
    g_capacity = 0;
}

size_t class_cache_size(void) { return g_count; }
