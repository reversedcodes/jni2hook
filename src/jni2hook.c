#include "jni2hook/jni2hook.h"

#include "hook/bytecode_scan.h"
#include "hook/class_cache.h"
#include "hook/trampoline.h"
#include "hook/class_watch.h"
#include "hook/field_scan.h"
#include "hook/jvm.h"
#include "hook/mutex.h"
#include "hook/vm_structs.h"
#include "jni2hook/utils/class_file_parser.h"
#include "jni2hook/utils/class_transform.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COPY_SUFFIX "$jni2hook"
#define INSERT_PREFIX "$jni2hook$"

typedef enum
{
    HOOK_MAKE_NATIVE,
    HOOK_INSERT_CALL
} hook_kind;

typedef struct
{
    jmethodID method;
    char *name;
    char *signature;
    char *added_name;
    void *native_function;
    void *trampoline;
    bool is_static;
    jmethodID original;
    uint32_t bytecode_offset;
    hook_kind kind;
} hook_entry;

typedef struct
{
    char *class_name;
    jclass klass;
    hook_entry *hooks;
    size_t count;
    size_t capacity;
} hooked_class;

static hook_mutex g_lock;
static bool g_initialized = false;
static bool g_can_suspend = false;
static jvmtiError g_last_error = JVMTI_ERROR_NONE;
static int g_flag_state = -1;
static hooked_class *g_classes = NULL;
static size_t g_class_count = 0;
static size_t g_class_cap = 0;
static uint64_t g_insert_serial = 0;

const char *JNI2Hook_StatusMessage(jni2hook_status status)
{
    switch (status)
    {
    case JNI2HOOK_OK:
        return "ok";
    case JNI2HOOK_ERR_NOT_INITIALIZED:
        return "JNI2Hook_Init has not run";
    case JNI2HOOK_ERR_NO_JNI:
        return "could not obtain a JNIEnv";
    case JNI2HOOK_ERR_NO_JVMTI:
        return "could not obtain a jvmtiEnv";
    case JNI2HOOK_ERR_CAPABILITIES:
        return "the VM refused the required JVMTI capabilities";
    case JNI2HOOK_ERR_JVMTI:
        return "a JVMTI operation failed";
    case JNI2HOOK_ERR_JNI:
        return "a JNI operation failed";
    case JNI2HOOK_ERR_JAVA_EXCEPTION:
        return "a Java exception was thrown";
    case JNI2HOOK_ERR_CLASS_NOT_CACHED:
        return "the declaring class could not be captured";
    case JNI2HOOK_ERR_CLASS_FILE:
        return "the class file could not be read or written";
    case JNI2HOOK_ERR_TRANSFORM:
        return "the method cannot be hooked";
    case JNI2HOOK_ERR_ALREADY_HOOKED:
        return "this method is already hooked";
    case JNI2HOOK_ERR_NOT_HOOKED:
        return "this method is not hooked";
    case JNI2HOOK_ERR_INVALID_PATTERN:
        return "the bytecode pattern is invalid";
    case JNI2HOOK_ERR_NOT_FOUND:
        return "no method matched the bytecode pattern";
    case JNI2HOOK_ERR_OUT_OF_MEMORY:
        return "out of memory";
    }
    return "unknown error";
}

jvmtiError JNI2Hook_LastJvmtiError(void)
{
    return g_last_error;
}

int JNI2Hook_ForcedRedefinitionFlag(void)
{
    return g_flag_state;
}

/* g_lock does not exist before Init. The second check closes the race with a
   concurrent Shutdown after the initial unlocked read. */
static bool lock_if_initialized(void)
{
    if (!g_initialized)
        return false;

    hook_mutex_lock(&g_lock);
    if (g_initialized)
        return true;

    hook_mutex_unlock(&g_lock);
    return false;
}

static jni2hook_status jvmti_failed(jvmtiError error)
{
    g_last_error = error;
    return JNI2HOOK_ERR_JVMTI;
}

static char *duplicate(const char *text)
{
    if (text == NULL)
        return NULL;
    const size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL)
        memcpy(copy, text, length + 1);
    return copy;
}

/* Class identity includes its loader, so binary names are not unique keys. */
static hooked_class *find_class(JNIEnv *env, jclass klass)
{
    if (env == NULL || klass == NULL)
        return NULL;

    for (size_t i = 0; i < g_class_count; i++)
    {
        if ((*env)->IsSameObject(env, g_classes[i].klass, klass) == JNI_TRUE)
            return &g_classes[i];
    }
    return NULL;
}

static hooked_class *find_class_of_method(jmethodID method, hook_entry **out_entry)
{
    for (size_t i = 0; i < g_class_count; i++)
    {
        for (size_t j = 0; j < g_classes[i].count; j++)
        {
            if (g_classes[i].hooks[j].method == method)
            {
                if (out_entry != NULL)
                    *out_entry = &g_classes[i].hooks[j];
                return &g_classes[i];
            }
        }
    }
    return NULL;
}

static bool method_conflicts(jmethodID method, hook_kind kind)
{
    for (size_t i = 0; i < g_class_count; i++)
    {
        for (size_t j = 0; j < g_classes[i].count; j++)
        {
            const hook_entry *entry = &g_classes[i].hooks[j];
            if (entry->method == method &&
                (kind == HOOK_MAKE_NATIVE || entry->kind == HOOK_MAKE_NATIVE))
                return true;
        }
    }
    return false;
}

static char *make_insert_name(void)
{
    const size_t capacity = sizeof(INSERT_PREFIX) + 20;
    char *name = malloc(capacity);
    if (name == NULL)
        return NULL;

    snprintf(name, capacity, "%s%" PRIu64, INSERT_PREFIX, g_insert_serial++);
    return name;
}

static void free_entry(hook_entry *entry)
{
    /* The page stays mapped on purpose: a stale call site may still arrive
       after the entry is gone. Disarmed it returns instead of dispatching. */
    trampoline_disarm(entry->trampoline);
    entry->trampoline = NULL;

    free(entry->name);
    free(entry->signature);
    free(entry->added_name);
    memset(entry, 0, sizeof(*entry));
}

static void drop_class(hooked_class *target)
{
    JNIEnv *env = jvm_env();
    if (env != NULL && target->klass != NULL)
        (*env)->DeleteGlobalRef(env, target->klass);

    for (size_t i = 0; i < target->count; i++)
        free_entry(&target->hooks[i]);
    free(target->hooks);
    free(target->class_name);

    const size_t index = (size_t)(target - g_classes);
    const size_t tail = g_class_count - index - 1;
    if (tail != 0)
        memmove(target, target + 1, tail * sizeof(*target));
    g_class_count--;
}

typedef struct
{
    jthread *threads;
    jvmtiError *results;
    jint count;
} suspended_set;

/* Between RedefineClasses and RegisterNatives the method is native but has no
   implementation bound, so any thread reaching it in that window would get an
   UnsatisfiedLinkError. Suspending is best effort: without can_suspend the
   window simply stays open, which is better than refusing to hook at all. */
static void suspend_others(suspended_set *set)
{
    set->threads = NULL;
    set->results = NULL;
    set->count = 0;

    jvmtiEnv *jvmti = jvm_jvmti();
    if (jvmti == NULL || !g_can_suspend)
        return;

    jthread current = NULL;
    if ((*jvmti)->GetCurrentThread(jvmti, &current) != JVMTI_ERROR_NONE)
        return;

    jint count = 0;
    jthread *threads = NULL;
    if ((*jvmti)->GetAllThreads(jvmti, &count, &threads) != JVMTI_ERROR_NONE)
        return;

    JNIEnv *env = jvm_env();
    jint kept = 0;
    for (jint i = 0; i < count; i++)
    {
        if (env != NULL && current != NULL &&
            (*env)->IsSameObject(env, threads[i], current) == JNI_TRUE)
            continue;
        threads[kept++] = threads[i];
    }

    set->threads = threads;
    if (kept <= 0)
        return;

    /* Keep the result array for resume; allocation must not fail after suspend. */
    jvmtiError *results = malloc((size_t)kept * sizeof(*results));
    if (results == NULL)
        return;

    for (jint i = 0; i < kept; i++)
        results[i] = JVMTI_ERROR_INTERNAL;

    (*jvmti)->SuspendThreadList(jvmti, kept, threads, results);

    /* Resume only threads suspended here, never ones another agent suspended. */
    jint suspended = 0;
    for (jint i = 0; i < kept; i++)
    {
        if (results[i] == JVMTI_ERROR_NONE)
            threads[suspended++] = threads[i];
    }

    set->results = results;
    set->count = suspended;
}

static void resume_others(suspended_set *set)
{
    jvmtiEnv *jvmti = jvm_jvmti();

    if (jvmti != NULL && set->count > 0 && set->results != NULL)
        (*jvmti)->ResumeThreadList(jvmti, set->count, set->threads, set->results);

    free(set->results);
    if (jvmti != NULL && set->threads != NULL)
        (*jvmti)->Deallocate(jvmti, (unsigned char *)set->threads);

    set->threads = NULL;
    set->results = NULL;
    set->count = 0;
}

static jni2hook_status transform_result(transform_status result)
{
    return result == TRANSFORM_ERR_CLASSFILE ? JNI2HOOK_ERR_CLASS_FILE : JNI2HOOK_ERR_TRANSFORM;
}

static int compare_insert_entries(const hook_entry *left, const hook_entry *right)
{
    int order = strcmp(left->name, right->name);
    if (order != 0)
        return order;

    order = strcmp(left->signature, right->signature);
    if (order != 0)
        return order;

    if (left->bytecode_offset > right->bytecode_offset)
        return -1;
    if (left->bytecode_offset < right->bytecode_offset)
        return 1;
    return 0;
}

/* Rebuilds from the cached original so hooks can change in any order. */
static jni2hook_status reapply(hooked_class *target)
{
    jvmtiEnv *jvmti = jvm_jvmti();
    JNIEnv *env = jvm_env();
    if (jvmti == NULL)
        return JNI2HOOK_ERR_NO_JVMTI;
    if (env == NULL)
        return JNI2HOOK_ERR_NO_JNI;

    jint original_size = 0;
    const unsigned char *original_bytes = class_cache_get(env, target->klass, &original_size);
    if (original_bytes == NULL)
        return JNI2HOOK_ERR_CLASS_NOT_CACHED;

    unsigned char *definition = NULL;
    jint definition_size = 0;
    bool definition_owned = false;

    if (target->count == 0)
    {
        definition = (unsigned char *)original_bytes;
        definition_size = original_size;
    }
    else
    {
        ClassFile *cf = NULL;
        classfile_status status = classfile_parse(original_bytes, (size_t)original_size, &cf);
        if (status != CLASSFILE_OK)
            return JNI2HOOK_ERR_CLASS_FILE;

        size_t insert_count = 0;
        for (size_t i = 0; i < target->count; i++)
        {
            if (target->hooks[i].kind == HOOK_INSERT_CALL)
                insert_count++;
        }

        hook_entry **inserts = NULL;
        if (insert_count != 0)
        {
            inserts = malloc(insert_count * sizeof(*inserts));
            if (inserts == NULL)
            {
                classFile_destroy(cf);
                return JNI2HOOK_ERR_OUT_OF_MEMORY;
            }
        }

        size_t insert_index = 0;
        for (size_t i = 0; i < target->count; i++)
        {
            const hook_entry *entry = &target->hooks[i];
            if (entry->kind == HOOK_INSERT_CALL)
            {
                inserts[insert_index++] = &target->hooks[i];
                continue;
            }

            classfile_status cause = CLASSFILE_OK;
            const transform_status result = class_transform_make_native(
                cf, entry->name, entry->signature, entry->added_name, &cause);
            if (result != TRANSFORM_OK)
            {
                free(inserts);
                classFile_destroy(cf);
                return transform_result(result);
            }
        }

        for (size_t i = 1; i < insert_count; i++)
        {
            hook_entry *entry = inserts[i];
            size_t j = i;
            while (j > 0 && compare_insert_entries(entry, inserts[j - 1]) < 0)
            {
                inserts[j] = inserts[j - 1];
                j--;
            }
            inserts[j] = entry;
        }

        for (size_t i = 0; i < insert_count; i++)
        {
            const hook_entry *entry = inserts[i];
            classfile_status cause = CLASSFILE_OK;
            const transform_status result =
                class_transform_insert_call(cf, entry->name, entry->signature,
                                            entry->bytecode_offset, entry->added_name, &cause);
            if (result != TRANSFORM_OK)
            {
                free(inserts);
                classFile_destroy(cf);
                return transform_result(result);
            }
        }
        free(inserts);

        u1 *bytes = NULL;
        size_t size = 0;
        status = classfile_serialize(cf, &bytes, &size);
        classFile_destroy(cf);
        if (status != CLASSFILE_OK)
            return JNI2HOOK_ERR_CLASS_FILE;

        definition = bytes;
        definition_size = (jint)size;
        definition_owned = true;
    }

    /* Suspend across the RedefineClasses/RegisterNatives gap, when a native
       method exists without an implementation. Restore-only has no such gap. */
    suspended_set suspended;
    if (target->count != 0)
        suspend_others(&suspended);
    else
        memset(&suspended, 0, sizeof(suspended));

    jvmtiClassDefinition class_definition;
    class_definition.klass = target->klass;
    class_definition.class_byte_count = definition_size;
    class_definition.class_bytes = definition;

    jni2hook_status result = JNI2HOOK_OK;
    const jvmtiError error = (*jvmti)->RedefineClasses(jvmti, 1, &class_definition);
    if (error != JVMTI_ERROR_NONE)
    {
        result = jvmti_failed(error);
    }
    else
    {
        /* Bind every native even after a failure; otherwise later methods stay
           native and unbound. Report the first failure. */
        for (size_t i = 0; i < target->count; i++)
        {
            hook_entry *entry = &target->hooks[i];

            JNINativeMethod binding;
            binding.name = entry->kind == HOOK_MAKE_NATIVE ? entry->name : entry->added_name;
            binding.signature = entry->kind == HOOK_MAKE_NATIVE ? entry->signature : "()V";
            binding.fnPtr = entry->trampoline != NULL ? entry->trampoline
                                                     : entry->native_function;

            if ((*env)->RegisterNatives(env, target->klass, &binding, 1) < 0)
            {
                jvm_clear_exception(env);
                if (result == JNI2HOOK_OK)
                    result = JNI2HOOK_ERR_JNI;
                continue;
            }

            if (entry->kind == HOOK_MAKE_NATIVE)
            {
                entry->original = entry->is_static
                                      ? (*env)->GetStaticMethodID(
                                            env, target->klass, entry->added_name, entry->signature)
                                      : (*env)->GetMethodID(env, target->klass, entry->added_name,
                                                            entry->signature);

                if (entry->original == NULL)
                {
                    jvm_clear_exception(env);
                    if (result == JNI2HOOK_OK)
                        result = JNI2HOOK_ERR_JNI;
                }
            }
        }
    }

    resume_others(&suspended);

    if (definition_owned)
        free(definition);

    return result;
}

jni2hook_status JNI2Hook_Init(JavaVM *vm)
{
    static bool lock_ready = false;
    if (!lock_ready)
    {
        hook_mutex_init(&g_lock);
        lock_ready = true;
    }

    hook_mutex_lock(&g_lock);

    if (g_initialized)
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_OK;
    }

    if (!jvm_bind(vm))
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_NO_JVMTI;
    }

    if (jvm_env() == NULL)
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_NO_JNI;
    }

    jvmtiEnv *jvmti = jvm_jvmti();

    jvmtiCapabilities available;
    memset(&available, 0, sizeof(available));
    (*jvmti)->GetPotentialCapabilities(jvmti, &available);

    /* A live-phase agent may request only capabilities still reported available. */
    jvmtiCapabilities wanted;
    memset(&wanted, 0, sizeof(wanted));
    wanted.can_redefine_classes = available.can_redefine_classes;
    wanted.can_retransform_classes = available.can_retransform_classes;
    wanted.can_redefine_any_class = available.can_redefine_any_class;
    wanted.can_retransform_any_class = available.can_retransform_any_class;
    wanted.can_generate_all_class_hook_events = available.can_generate_all_class_hook_events;
    wanted.can_get_bytecodes = available.can_get_bytecodes;
    wanted.can_get_constant_pool = available.can_get_constant_pool;
    wanted.can_suspend = available.can_suspend;

    const jvmtiError error = (*jvmti)->AddCapabilities(jvmti, &wanted);
    if (error != JVMTI_ERROR_NONE)
    {
        g_last_error = error;
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_CAPABILITIES;
    }

    jvmtiCapabilities granted;
    memset(&granted, 0, sizeof(granted));
    (*jvmti)->GetCapabilities(jvmti, &granted);

    if (granted.can_redefine_classes == 0 || granted.can_retransform_classes == 0)
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_CAPABILITIES;
    }

    g_can_suspend = granted.can_suspend != 0;

    /* Added method copies require this non-manageable HotSpot product flag. */
    {
        bool previous = false;
        if (vm_structs_set_bool_flag("AllowRedefinitionToAddDeleteMethods", true, &previous))
            g_flag_state = previous ? 0 : 1;
    }

    if (!class_cache_start())
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_JVMTI;
    }

    g_initialized = true;
    hook_mutex_unlock(&g_lock);
    return JNI2HOOK_OK;
}

jni2hook_status JNI2Hook_InitFromRunningVm(void)
{
    JavaVM *vm = jvm_find_running();
    if (vm == NULL)
        return JNI2HOOK_ERR_NO_JVMTI;

    /* HotSpot requires attachment before a native thread asks GetEnv for JVMTI. */
    JNIEnv *env = NULL;
    const jint env_status = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (env_status == JNI_EDETACHED)
    {
        JavaVMAttachArgs args;
        args.version = JNI_VERSION_1_6;
        args.name = "jni2hook-init";
        args.group = NULL;
        if ((*vm)->AttachCurrentThreadAsDaemon(vm, (void **)&env, &args) != JNI_OK)
            return JNI2HOOK_ERR_NO_JNI;
    }
    else if (env_status != JNI_OK || env == NULL)
    {
        return JNI2HOOK_ERR_NO_JNI;
    }

    return JNI2Hook_Init(vm);
}

jni2hook_status JNI2Hook_Attach(JNIEnv **out_env)
{
    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;
    hook_mutex_unlock(&g_lock);

    JNIEnv *env = jvm_env();
    if (env == NULL)
        return JNI2HOOK_ERR_NO_JNI;

    if (out_env != NULL)
        *out_env = env;
    return JNI2HOOK_OK;
}

static jni2hook_status install(jmethodID method, uint32_t bytecode_offset, void *native_function,
                               jmethodID *out_original, hook_kind kind)
{
    if (method == NULL || native_function == NULL)
        return JNI2HOOK_ERR_TRANSFORM;

    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    jvmtiEnv *jvmti = jvm_jvmti();
    JNIEnv *env = jvm_env();
    if (jvmti == NULL)
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_NO_JVMTI;
    }
    if (env == NULL)
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_NO_JNI;
    }

    if (method_conflicts(method, kind))
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_ALREADY_HOOKED;
    }

    jni2hook_status result = JNI2HOOK_OK;
    char *name = NULL, *signature = NULL, *class_name = NULL, *added_name = NULL;
    jclass declaring = NULL;
    hooked_class *target = NULL;

    jvmtiError error = (*jvmti)->GetMethodDeclaringClass(jvmti, method, &declaring);
    if (error != JVMTI_ERROR_NONE)
    {
        result = jvmti_failed(error);
        goto done;
    }

    class_name = jvm_class_name_of(declaring);
    if (class_name == NULL)
    {
        result = JNI2HOOK_ERR_JVMTI;
        goto done;
    }

    {
        char *raw_name = NULL, *raw_signature = NULL;
        error = (*jvmti)->GetMethodName(jvmti, method, &raw_name, &raw_signature, NULL);
        if (error != JVMTI_ERROR_NONE)
        {
            result = jvmti_failed(error);
            goto done;
        }
        name = jvm_take_string(raw_name);
        signature = jvm_take_string(raw_signature);
    }

    if (name == NULL || signature == NULL)
    {
        result = JNI2HOOK_ERR_OUT_OF_MEMORY;
        goto done;
    }

    if (kind == HOOK_MAKE_NATIVE)
    {
        const size_t length = strlen(name) + sizeof(COPY_SUFFIX);
        added_name = malloc(length);
        if (added_name != NULL)
            snprintf(added_name, length, "%s%s", name, COPY_SUFFIX);
    }
    else
    {
        added_name = make_insert_name();
    }
    if (added_name == NULL)
    {
        result = JNI2HOOK_ERR_OUT_OF_MEMORY;
        goto done;
    }

    jint modifiers = 0;
    error = (*jvmti)->GetMethodModifiers(jvmti, method, &modifiers);
    if (error != JVMTI_ERROR_NONE)
    {
        result = jvmti_failed(error);
        goto done;
    }

    if (!class_cache_ensure(declaring, class_name, &error))
    {
        g_last_error = error;
        result = JNI2HOOK_ERR_CLASS_NOT_CACHED;
        goto done;
    }

    target = find_class(env, declaring);
    if (target == NULL)
    {
        if (g_class_count == g_class_cap)
        {
            const size_t capacity = g_class_cap ? g_class_cap * 2 : 8;
            hooked_class *grown = realloc(g_classes, capacity * sizeof(*grown));
            if (grown == NULL)
            {
                result = JNI2HOOK_ERR_OUT_OF_MEMORY;
                goto done;
            }
            g_classes = grown;
            g_class_cap = capacity;
        }

        target = &g_classes[g_class_count];
        memset(target, 0, sizeof(*target));
        target->class_name = duplicate(class_name);
        target->klass = (*env)->NewGlobalRef(env, declaring);
        if (target->class_name == NULL || target->klass == NULL)
        {
            free(target->class_name);
            result = JNI2HOOK_ERR_OUT_OF_MEMORY;
            goto done;
        }
        g_class_count++;
    }

    if (target->count == target->capacity)
    {
        const size_t capacity = target->capacity ? target->capacity * 2 : 4;
        hook_entry *grown = realloc(target->hooks, capacity * sizeof(*grown));
        if (grown == NULL)
        {
            result = JNI2HOOK_ERR_OUT_OF_MEMORY;
            goto done;
        }
        target->hooks = grown;
        target->capacity = capacity;
    }

    {
        hook_entry *entry = &target->hooks[target->count];
        memset(entry, 0, sizeof(*entry));
        entry->method = method;
        entry->name = name;
        entry->signature = signature;
        entry->added_name = added_name;
        entry->native_function = native_function;
        {
            const char *returns = entry->kind == HOOK_MAKE_NATIVE && entry->signature != NULL
                                      ? strchr(entry->signature, ')')
                                      : NULL;
            entry->trampoline =
                trampoline_create(native_function, returns != NULL ? returns[1] : 'V');
        }
        entry->is_static = (modifiers & JVM_ACC_STATIC) != 0;
        entry->bytecode_offset = bytecode_offset;
        entry->kind = kind;
        target->count++;

        name = signature = added_name = NULL;
    }

    result = reapply(target);
    if (result != JNI2HOOK_OK)
    {
        target->count--;
        free_entry(&target->hooks[target->count]);
        if (target->count == 0)
            drop_class(target);
        else
            reapply(target);
        goto done;
    }

    if (kind == HOOK_MAKE_NATIVE && out_original != NULL)
        *out_original = target->hooks[target->count - 1].original;

done:
    free(name);
    free(signature);
    free(added_name);
    free(class_name);
    if (declaring != NULL)
        (*env)->DeleteLocalRef(env, declaring);

    hook_mutex_unlock(&g_lock);
    return result;
}

jni2hook_status JNI2Hook_Install(jmethodID method, void *native_function, jmethodID *out_original)
{
    return install(method, 0, native_function, out_original, HOOK_MAKE_NATIVE);
}

jni2hook_status JNI2Hook_InstallAt(jmethodID method, uint32_t bytecode_offset,
                                   void *native_function)
{
    return install(method, bytecode_offset, native_function, NULL, HOOK_INSERT_CALL);
}

static jni2hook_status find_method(jclass target, const char *pattern, jmethodID *out_method,
                                   uint32_t *out_bytecode_offset, jni2hook_search_stats *out_stats)
{
    if (pattern == NULL || out_method == NULL || out_bytecode_offset == NULL)
        return JNI2HOOK_ERR_INVALID_PATTERN;

    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    JNIEnv *env = jvm_env();
    jvmtiEnv *jvmti = jvm_jvmti();
    jni2hook_status result = JNI2HOOK_OK;
    if (env == NULL)
    {
        result = JNI2HOOK_ERR_NO_JNI;
    }
    else if (jvmti == NULL)
    {
        result = JNI2HOOK_ERR_NO_JVMTI;
    }
    else
    {
        *out_method = NULL;
        *out_bytecode_offset = 0;
        jvmtiError error = JVMTI_ERROR_NONE;
        result = target == NULL ? bytecode_scan_find(env, jvmti, pattern, out_method,
                                                     out_bytecode_offset, out_stats, &error)
                                : bytecode_scan_find_in_class(jvmti, target, pattern, out_method,
                                                              out_bytecode_offset, &error);
        if (result == JNI2HOOK_ERR_JVMTI)
            g_last_error = error;
    }

    hook_mutex_unlock(&g_lock);
    return result;
}

jni2hook_status JNI2Hook_FindMethod(const char *pattern, jmethodID *out_method,
                                    uint32_t *out_bytecode_offset, jni2hook_search_stats *out_stats)
{
    return find_method(NULL, pattern, out_method, out_bytecode_offset, out_stats);
}

jni2hook_status JNI2Hook_FindMethodInClass(jclass target, const char *pattern,
                                           jmethodID *out_method, uint32_t *out_bytecode_offset)
{
    if (target == NULL)
        return JNI2HOOK_ERR_INVALID_PATTERN;
    return find_method(target, pattern, out_method, out_bytecode_offset, NULL);
}

jni2hook_status JNI2Hook_WatchMethod(const char *pattern,
                                     jni2hook_method_watch **out_watch)
{
    if (pattern == NULL || out_watch == NULL)
        return JNI2HOOK_ERR_INVALID_PATTERN;
    *out_watch = NULL;

    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    JNIEnv *env = jvm_env();
    jvmtiEnv *jvmti = jvm_jvmti();
    jni2hook_method_watch *watch = NULL;
    jni2hook_status result;
    if (env == NULL)
    {
        result = JNI2HOOK_ERR_NO_JNI;
        goto done;
    }
    if (jvmti == NULL)
    {
        result = JNI2HOOK_ERR_NO_JVMTI;
        goto done;
    }

    result = class_watch_create(env, pattern, &watch);
    if (result != JNI2HOOK_OK)
        goto done;

    /* The events are active before this snapshot. A class that races the scan
       is therefore either found here or captured from its raw load bytes. */
    jmethodID method = NULL;
    uint32_t offset = 0;
    jvmtiError error = JVMTI_ERROR_NONE;
    result = bytecode_scan_find(env, jvmti, pattern, &method, &offset, NULL, &error);
    if (result == JNI2HOOK_OK)
    {
        result = class_watch_resolve_loaded(env, jvmti, watch, method, offset);
    }
    else if (result == JNI2HOOK_ERR_NOT_FOUND)
    {
        result = JNI2HOOK_OK;
    }
    else
    {
        if (result == JNI2HOOK_ERR_JVMTI)
            g_last_error = error;
        class_watch_destroy(env, watch);
        watch = NULL;
    }

    if (result == JNI2HOOK_OK)
        *out_watch = watch;

done:
    hook_mutex_unlock(&g_lock);
    return result;
}

jni2hook_status JNI2Hook_GetWatchedMethod(jni2hook_method_watch *watch,
                                          jmethodID *out_method,
                                          uint32_t *out_bytecode_offset)
{
    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;
    const jni2hook_status result =
        class_watch_get(watch, out_method, out_bytecode_offset);
    hook_mutex_unlock(&g_lock);
    return result;
}

void JNI2Hook_DestroyMethodWatch(jni2hook_method_watch *watch)
{
    if (watch == NULL)
        return;

    if (lock_if_initialized())
    {
        class_watch_destroy(jvm_env(), watch);
        hook_mutex_unlock(&g_lock);
        return;
    }

    class_watch_destroy(NULL, watch);
}

/* Moves matching entries aside, rebuilds, then frees them. On rebuild failure
   the old count is restored so live detours remain tracked and retryable. */
typedef bool (*hook_predicate)(const hook_entry *entry, const void *context);

static jni2hook_status uninstall_matching(jmethodID method, hook_predicate matches,
                                          const void *context)
{
    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    hooked_class *target = find_class_of_method(method, NULL);
    if (target == NULL)
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_NOT_HOOKED;
    }

    const size_t original = target->count;
    size_t kept = 0;
    for (size_t i = 0; i < original; i++)
    {
        if (matches(&target->hooks[i], context))
            continue;
        if (kept != i)
        {
            const hook_entry moved = target->hooks[kept];
            target->hooks[kept] = target->hooks[i];
            target->hooks[i] = moved;
        }
        kept++;
    }

    if (kept == original)
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_NOT_HOOKED;
    }

    target->count = kept;
    const jni2hook_status result = reapply(target);

    if (result != JNI2HOOK_OK)
    {
        target->count = original;
        hook_mutex_unlock(&g_lock);
        return result;
    }

    for (size_t i = kept; i < original; i++)
        free_entry(&target->hooks[i]);

    if (target->count == 0)
        drop_class(target);

    hook_mutex_unlock(&g_lock);
    return JNI2HOOK_OK;
}

static bool matches_method(const hook_entry *entry, const void *context)
{
    return entry->method == (jmethodID)context;
}

typedef struct
{
    jmethodID method;
    uint32_t bytecode_offset;
    void *native_function;
} insert_key;

static bool matches_insert(const hook_entry *entry, const void *context)
{
    const insert_key *key = context;
    return entry->kind == HOOK_INSERT_CALL && entry->method == key->method &&
           entry->bytecode_offset == key->bytecode_offset &&
           entry->native_function == key->native_function;
}

jni2hook_status JNI2Hook_Uninstall(jmethodID method)
{
    return uninstall_matching(method, matches_method, method);
}

jni2hook_status JNI2Hook_UninstallAt(jmethodID method, uint32_t bytecode_offset,
                                     void *native_function)
{
    if (method == NULL || native_function == NULL)
        return JNI2HOOK_ERR_NOT_HOOKED;

    const insert_key key = {method, bytecode_offset, native_function};
    return uninstall_matching(method, matches_insert, &key);
}

int JNI2Hook_IsInstalled(jmethodID method)
{
    if (!lock_if_initialized())
        return 0;

    const int installed = find_class_of_method(method, NULL) != NULL;
    hook_mutex_unlock(&g_lock);
    return installed;
}

jni2hook_status JNI2Hook_Shutdown(void)
{
    if (!lock_if_initialized())
        return JNI2HOOK_OK;

    /* Restore every class and return the first failure to the unload caller. */
    jni2hook_status result = JNI2HOOK_OK;

    while (g_class_count > 0)
    {
        hooked_class *target = &g_classes[g_class_count - 1];
        const size_t count = target->count;

        target->count = 0;
        const jni2hook_status restored = reapply(target);
        if (restored != JNI2HOOK_OK && result == JNI2HOOK_OK)
            result = restored;

        for (size_t i = 0; i < count; i++)
            free_entry(&target->hooks[i]);
        drop_class(target);
    }

    free(g_classes);
    g_classes = NULL;
    g_class_count = 0;
    g_class_cap = 0;
    g_insert_serial = 0;

    /* Put the VM flag back the way it was found. Leaving a deprecated product
       flag flipped on outlives the library in the host process. */
    if (g_flag_state == 1)
        vm_structs_set_bool_flag("AllowRedefinitionToAddDeleteMethods", false, NULL);
    g_flag_state = -1;

    class_watch_clear(jvm_env());
    class_cache_clear();
    class_cache_stop();
    jvm_release();

    g_can_suspend = false;
    g_initialized = false;
    hook_mutex_unlock(&g_lock);
    return result;
}

jni2hook_status JNI2Hook_FindFieldInClass(jclass target, const char *pattern,
                                          uint32_t instruction_offset, jfieldID *out_field,
                                          int *out_is_static)
{
    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    JNIEnv *env = jvm_env();
    jvmtiEnv *jvmti = jvm_jvmti();
    jni2hook_status status;

    if (env == NULL)
    {
        status = JNI2HOOK_ERR_NO_JNI;
    }
    else if (jvmti == NULL)
    {
        status = JNI2HOOK_ERR_NO_JVMTI;
    }
    else
    {
        jvmtiError error = JVMTI_ERROR_NONE;
        status = field_scan_find_in_class(env, jvmti, target, pattern, instruction_offset,
                                          out_field, out_is_static, &error);
        if (status == JNI2HOOK_ERR_JVMTI)
            g_last_error = error;
    }

    hook_mutex_unlock(&g_lock);
    return status;
}

/* GetLoadedClasses hands back one local reference per class, and a live game
   has tens of thousands of them, so each is released as it is examined rather
   than left to pile up in the frame until the caller returns. */
jni2hook_status JNI2Hook_FindLoadedClass(const char *internal_name, jclass *out_class)
{
    if (internal_name == NULL || out_class == NULL)
        return JNI2HOOK_ERR_NOT_FOUND;

    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    JNIEnv *env = jvm_env();
    jvmtiEnv *jvmti = jvm_jvmti();
    jni2hook_status status;

    if (env == NULL)
    {
        status = JNI2HOOK_ERR_NO_JNI;
        goto done;
    }
    if (jvmti == NULL)
    {
        status = JNI2HOOK_ERR_NO_JVMTI;
        goto done;
    }

    jint count = 0;
    jclass *classes = NULL;
    const jvmtiError error = (*jvmti)->GetLoadedClasses(jvmti, &count, &classes);
    if (error != JVMTI_ERROR_NONE)
    {
        g_last_error = error;
        status = JNI2HOOK_ERR_JVMTI;
        goto done;
    }

    jclass found = NULL;
    for (jint i = 0; i < count; i++)
    {
        if (found == NULL)
        {
            /* Comparing the signature rather than a name built by hand keeps
               array and primitive classes from ever matching by accident. */
            char *name = jvm_class_name_of(classes[i]);
            if (name != NULL && strcmp(name, internal_name) == 0)
                found = (*env)->NewLocalRef(env, classes[i]);
            free(name);
        }

        (*env)->DeleteLocalRef(env, classes[i]);
    }

    (*jvmti)->Deallocate(jvmti, (unsigned char *)classes);

    if (found == NULL)
    {
        status = JNI2HOOK_ERR_NOT_FOUND;
        goto done;
    }

    *out_class = found;
    status = JNI2HOOK_OK;

done:
    hook_mutex_unlock(&g_lock);
    return status;
}

jni2hook_status JNI2Hook_GetClassLoader(jclass klass, jobject *out_loader)
{
    if (klass == NULL || out_loader == NULL)
        return JNI2HOOK_ERR_NOT_FOUND;

    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    jvmtiEnv *jvmti = jvm_jvmti();
    jni2hook_status status;

    if (jvmti == NULL)
    {
        status = JNI2HOOK_ERR_NO_JVMTI;
    }
    else
    {
        /* A NULL loader is the bootstrap loader, which is an answer rather
           than a failure, so it is passed straight through. */
        jobject loader = NULL;
        const jvmtiError error = (*jvmti)->GetClassLoader(jvmti, klass, &loader);
        if (error != JVMTI_ERROR_NONE)
        {
            g_last_error = error;
            status = JNI2HOOK_ERR_JVMTI;
        }
        else
        {
            *out_loader = loader;
            status = JNI2HOOK_OK;
        }
    }

    hook_mutex_unlock(&g_lock);
    return status;
}

jni2hook_status JNI2Hook_DefineClass(jobject loader, const char *internal_name,
                                     const unsigned char *bytes, size_t size, jclass *out_class)
{
    if (internal_name == NULL || bytes == NULL || size == 0 || out_class == NULL)
        return JNI2HOOK_ERR_CLASS_FILE;

    /* DefineClass takes a jsize, so anything past its range cannot be passed
       on and is refused here instead of being truncated. */
    if (size > (size_t)0x7FFFFFFF)
        return JNI2HOOK_ERR_CLASS_FILE;

    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    JNIEnv *env = jvm_env();
    jni2hook_status status;

    if (env == NULL)
    {
        status = JNI2HOOK_ERR_NO_JNI;
    }
    else
    {
        jclass defined = (*env)->DefineClass(env, internal_name, loader, (const jbyte *)bytes,
                                             (jsize)size);

        if ((*env)->ExceptionCheck(env) == JNI_TRUE)
        {
            /* ClassFormatError, LinkageError and a duplicate definition all
               arrive this way. The exception is cleared so the caller is not
               handed a VM with one still pending. */
            (*env)->ExceptionClear(env);
            if (defined != NULL)
                (*env)->DeleteLocalRef(env, defined);
            status = JNI2HOOK_ERR_JAVA_EXCEPTION;
        }
        else if (defined == NULL)
        {
            status = JNI2HOOK_ERR_JNI;
        }
        else
        {
            *out_class = defined;
            status = JNI2HOOK_OK;
        }
    }

    hook_mutex_unlock(&g_lock);
    return status;
}

jni2hook_status JNI2Hook_RedefineClass(jclass klass, const unsigned char *bytes, size_t size)
{
    if (klass == NULL || bytes == NULL || size == 0)
        return JNI2HOOK_ERR_CLASS_FILE;

    /* RedefineClasses counts the bytes in a jint. */
    if (size > (size_t)0x7FFFFFFF)
        return JNI2HOOK_ERR_CLASS_FILE;

    if (!lock_if_initialized())
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    jvmtiEnv *jvmti = jvm_jvmti();
    jni2hook_status status;

    if (jvmti == NULL)
    {
        status = JNI2HOOK_ERR_NO_JVMTI;
    }
    else
    {
        jvmtiClassDefinition definition;
        definition.klass = klass;
        definition.class_byte_count = (jint)size;
        definition.class_bytes = bytes;

        const jvmtiError error = (*jvmti)->RedefineClasses(jvmti, 1, &definition);
        if (error != JVMTI_ERROR_NONE)
        {
            g_last_error = error;
            status = JNI2HOOK_ERR_JVMTI;
        }
        else
        {
            status = JNI2HOOK_OK;
        }
    }

    hook_mutex_unlock(&g_lock);
    return status;
}
