#include "class_watch.h"

#include "bytecode_scan.h"
#include "class_cache.h"
#include "jvm.h"
#include "mutex.h"
#include "jni2hook/utils/class_file_constant.h"
#include "jni2hook/utils/class_file_parser.h"

#include <stdlib.h>
#include <string.h>

typedef enum
{
    WATCH_WAITING,
    WATCH_CAPTURED,
    WATCH_READY,
    WATCH_FAILED,
    WATCH_CANCELLED
} watch_state;

/* Future classes move WAITING -> CAPTURED -> READY. The loaded-class snapshot
   may resolve WAITING directly to READY; FAILED and CANCELLED are terminal. */
struct jni2hook_method_watch
{
    bytecode_pattern pattern;
    watch_state state;
    jni2hook_status failure;

    char *class_name;
    jobject loader;
    bool bootstrap_loader;
    char *method_name;
    char *method_signature;
    bool method_static;

    jclass declaring_class;
    jmethodID method;
    uint32_t offset;

    bool registered;
    struct jni2hook_method_watch *next;
};

static hook_mutex g_watch_lock;
static bool g_watch_lock_ready = false;
static jni2hook_method_watch *g_watches = NULL;

static void lock_watches(void)
{
    if (!g_watch_lock_ready)
    {
        hook_mutex_init(&g_watch_lock);
        g_watch_lock_ready = true;
    }
    hook_mutex_lock(&g_watch_lock);
}

static void unlock_watches(void)
{
    hook_mutex_unlock(&g_watch_lock);
}

static char *duplicate_text(const char *text)
{
    if (text == NULL)
        return NULL;
    const size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL)
        memcpy(copy, text, length + 1);
    return copy;
}

static char *duplicate_utf8(const constant_pool *pool, u2 index)
{
    const u1 *bytes = NULL;
    u2 length = 0;
    if (!constant_pool_utf8(pool, index, &bytes, &length))
        return NULL;

    char *copy = malloc((size_t)length + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, bytes, length);
    copy[length] = '\0';
    return copy;
}

static char *class_file_name(const ClassFile *class_file)
{
    const cp_info *entry = constant_pool_at(&class_file->constant_pool,
                                            class_file->this_class);
    if (entry == NULL || entry->tag != JVM_CONSTANT_Class)
        return NULL;
    return duplicate_utf8(&class_file->constant_pool, entry->u.class_info.name_index);
}

static void release_candidate(JNIEnv *env, jni2hook_method_watch *watch)
{
    if (env != NULL && watch->loader != NULL)
        (*env)->DeleteGlobalRef(env, watch->loader);
    watch->loader = NULL;
    free(watch->class_name);
    free(watch->method_name);
    free(watch->method_signature);
    watch->class_name = NULL;
    watch->method_name = NULL;
    watch->method_signature = NULL;
}

static void release_ready(JNIEnv *env, jni2hook_method_watch *watch)
{
    if (env != NULL && watch->declaring_class != NULL)
        (*env)->DeleteGlobalRef(env, watch->declaring_class);
    watch->declaring_class = NULL;
    watch->method = NULL;
}

static void fail_watch(JNIEnv *env, jni2hook_method_watch *watch, jni2hook_status failure)
{
    release_candidate(env, watch);
    release_ready(env, watch);
    watch->failure = failure;
    watch->state = WATCH_FAILED;
}

jni2hook_status class_watch_create(JNIEnv *env, const char *pattern,
                                    jni2hook_method_watch **out_watch)
{
    if (env == NULL || out_watch == NULL)
        return JNI2HOOK_ERR_INVALID_PATTERN;
    *out_watch = NULL;

    jni2hook_method_watch *watch = calloc(1, sizeof(*watch));
    if (watch == NULL)
        return JNI2HOOK_ERR_OUT_OF_MEMORY;

    jni2hook_status status = bytecode_pattern_compile(pattern, &watch->pattern);
    if (status != JNI2HOOK_OK)
    {
        free(watch);
        return status;
    }
    watch->state = WATCH_WAITING;
    watch->failure = JNI2HOOK_OK;

    lock_watches();
    watch->next = g_watches;
    watch->registered = true;
    g_watches = watch;

    if (!class_cache_set_watch_events(true))
    {
        g_watches = watch->next;
        watch->registered = false;
        unlock_watches();
        bytecode_pattern_destroy(&watch->pattern);
        free(watch);
        return JNI2HOOK_ERR_JVMTI;
    }
    unlock_watches();

    *out_watch = watch;
    return JNI2HOOK_OK;
}

jni2hook_status class_watch_resolve_loaded(JNIEnv *env, jvmtiEnv *jvmti,
                                           jni2hook_method_watch *watch,
                                           jmethodID method, uint32_t offset)
{
    if (env == NULL || jvmti == NULL || watch == NULL || method == NULL)
        return JNI2HOOK_ERR_JVMTI;

    lock_watches();
    if (!watch->registered || watch->state == WATCH_READY)
    {
        unlock_watches();
        return JNI2HOOK_OK;
    }

    jclass declaring = NULL;
    const jvmtiError error = (*jvmti)->GetMethodDeclaringClass(jvmti, method, &declaring);
    if (error != JVMTI_ERROR_NONE || declaring == NULL)
    {
        fail_watch(env, watch, JNI2HOOK_ERR_JVMTI);
        unlock_watches();
        return JNI2HOOK_ERR_JVMTI;
    }

    jclass owned = (*env)->NewGlobalRef(env, declaring);
    (*env)->DeleteLocalRef(env, declaring);
    if (owned == NULL)
    {
        fail_watch(env, watch, JNI2HOOK_ERR_OUT_OF_MEMORY);
        unlock_watches();
        return JNI2HOOK_ERR_OUT_OF_MEMORY;
    }

    release_candidate(env, watch);
    release_ready(env, watch);
    watch->declaring_class = owned;
    watch->method = method;
    watch->offset = offset;
    watch->state = WATCH_READY;
    unlock_watches();
    return JNI2HOOK_OK;
}

jni2hook_status class_watch_get(jni2hook_method_watch *watch, jmethodID *out_method,
                                 uint32_t *out_offset)
{
    if (watch == NULL || out_method == NULL || out_offset == NULL)
        return JNI2HOOK_ERR_INVALID_PATTERN;

    lock_watches();
    jni2hook_status status = JNI2HOOK_ERR_NOT_FOUND;
    if (watch->state == WATCH_READY)
    {
        *out_method = watch->method;
        *out_offset = watch->offset;
        status = JNI2HOOK_OK;
    }
    else if (watch->state == WATCH_FAILED)
    {
        status = watch->failure;
    }
    else if (watch->state == WATCH_CANCELLED)
    {
        status = JNI2HOOK_ERR_NOT_INITIALIZED;
    }
    unlock_watches();
    return status;
}

void class_watch_destroy(JNIEnv *env, jni2hook_method_watch *watch)
{
    if (watch == NULL)
        return;

    lock_watches();
    if (watch->registered)
    {
        jni2hook_method_watch **cursor = &g_watches;
        while (*cursor != NULL && *cursor != watch)
            cursor = &(*cursor)->next;
        if (*cursor == watch)
            *cursor = watch->next;
        watch->registered = false;
    }

    const bool empty = g_watches == NULL;
    if (empty)
        (void)class_cache_set_watch_events(false);

    release_candidate(env, watch);
    release_ready(env, watch);
    unlock_watches();

    bytecode_pattern_destroy(&watch->pattern);
    free(watch);
}

void class_watch_on_class_file_load(JNIEnv *env, jobject loader, jint class_data_len,
                                    const unsigned char *class_data)
{
    if (env == NULL || class_data == NULL || class_data_len <= 0)
        return;

    lock_watches();
    bool waiting = false;
    for (jni2hook_method_watch *watch = g_watches; watch != NULL; watch = watch->next)
    {
        if (watch->state == WATCH_WAITING)
        {
            waiting = true;
            break;
        }
    }
    if (!waiting)
    {
        unlock_watches();
        return;
    }

    ClassFile *class_file = NULL;
    if (classfile_parse(class_data, (size_t)class_data_len, &class_file) != CLASSFILE_OK)
    {
        unlock_watches();
        return;
    }

    char *parsed_class_name = NULL;
    for (jni2hook_method_watch *watch = g_watches; watch != NULL; watch = watch->next)
    {
        if (watch->state != WATCH_WAITING)
            continue;

        size_t method_index = 0;
        uint32_t offset = 0;
        if (!bytecode_pattern_find_in_class_file(&watch->pattern, class_file,
                                                 &method_index, &offset))
            continue;

        if (parsed_class_name == NULL)
            parsed_class_name = class_file_name(class_file);
        const method_info *method = &class_file->methods.items[method_index];

        watch->class_name = duplicate_text(parsed_class_name);
        watch->method_name = duplicate_utf8(&class_file->constant_pool, method->name_index);
        watch->method_signature =
            duplicate_utf8(&class_file->constant_pool, method->descriptor_index);
        watch->method_static = member_info_is_static(method);
        watch->bootstrap_loader = loader == NULL;
        watch->loader = loader != NULL ? (*env)->NewGlobalRef(env, loader) : NULL;
        watch->offset = offset;

        if (watch->class_name == NULL || watch->method_name == NULL ||
            watch->method_signature == NULL || (loader != NULL && watch->loader == NULL))
        {
            fail_watch(env, watch, JNI2HOOK_ERR_OUT_OF_MEMORY);
            continue;
        }
        watch->state = WATCH_CAPTURED;
    }

    free(parsed_class_name);
    classFile_destroy(class_file);
    unlock_watches();
}

void class_watch_on_class_prepare(jvmtiEnv *jvmti, JNIEnv *env, jclass klass)
{
    if (jvmti == NULL || env == NULL || klass == NULL)
        return;

    lock_watches();

    /* Registration can land after this class's ClassFileLoadHook but before
       ClassPrepare. Scan this one class once here so that narrow transition is
       not lost; this never walks the global loaded-class set. */
    for (jni2hook_method_watch *watch = g_watches; watch != NULL; watch = watch->next)
    {
        if (watch->state != WATCH_WAITING)
            continue;

        jmethodID method = NULL;
        uint32_t offset = 0;
        if (!bytecode_pattern_find_in_prepared_class(jvmti, klass, &watch->pattern,
                                                     &method, &offset))
            continue;

        jclass owned = (*env)->NewGlobalRef(env, klass);
        if (owned == NULL)
        {
            fail_watch(env, watch, JNI2HOOK_ERR_OUT_OF_MEMORY);
            continue;
        }
        watch->declaring_class = owned;
        watch->method = method;
        watch->offset = offset;
        watch->state = WATCH_READY;
    }

    bool captured = false;
    for (jni2hook_method_watch *watch = g_watches; watch != NULL; watch = watch->next)
    {
        if (watch->state == WATCH_CAPTURED)
        {
            captured = true;
            break;
        }
    }
    if (!captured)
    {
        unlock_watches();
        return;
    }

    char *class_name = jvm_class_name_of(klass);
    jobject loader = NULL;
    if (class_name == NULL ||
        (*jvmti)->GetClassLoader(jvmti, klass, &loader) != JVMTI_ERROR_NONE)
    {
        free(class_name);
        unlock_watches();
        return;
    }

    for (jni2hook_method_watch *watch = g_watches; watch != NULL; watch = watch->next)
    {
        if (watch->state != WATCH_CAPTURED || strcmp(watch->class_name, class_name) != 0)
            continue;

        const bool loader_matches =
            (watch->bootstrap_loader && loader == NULL) ||
            (!watch->bootstrap_loader && loader != NULL &&
             (*env)->IsSameObject(env, watch->loader, loader) == JNI_TRUE);
        if (!loader_matches)
            continue;

        jmethodID method = watch->method_static
                               ? (*env)->GetStaticMethodID(env, klass, watch->method_name,
                                                          watch->method_signature)
                               : (*env)->GetMethodID(env, klass, watch->method_name,
                                                    watch->method_signature);
        if (method == NULL)
        {
            jvm_clear_exception(env);
            fail_watch(env, watch, JNI2HOOK_ERR_JNI);
            continue;
        }

        jclass owned = (*env)->NewGlobalRef(env, klass);
        if (owned == NULL)
        {
            fail_watch(env, watch, JNI2HOOK_ERR_OUT_OF_MEMORY);
            continue;
        }

        release_candidate(env, watch);
        watch->declaring_class = owned;
        watch->method = method;
        watch->state = WATCH_READY;
    }

    if (loader != NULL)
        (*env)->DeleteLocalRef(env, loader);
    free(class_name);
    unlock_watches();
}

void class_watch_clear(JNIEnv *env)
{
    lock_watches();
    jni2hook_method_watch *watch = g_watches;
    g_watches = NULL;
    while (watch != NULL)
    {
        jni2hook_method_watch *next = watch->next;
        release_candidate(env, watch);
        release_ready(env, watch);
        watch->registered = false;
        watch->state = WATCH_CANCELLED;
        watch->next = NULL;
        watch = next;
    }
    unlock_watches();
}
