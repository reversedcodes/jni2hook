#include "jni2hook/jni2hook.h"

#include "hook/bytecode_scan.h"
#include "hook/class_cache.h"
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

static hooked_class *find_class(const char *class_name)
{
    for (size_t i = 0; i < g_class_count; i++)
    {
        if (strcmp(g_classes[i].class_name, class_name) == 0)
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
    jint count;
} suspended_set;

/* Between RedefineClasses and RegisterNatives the method is native but has no
   implementation bound, so any thread reaching it in that window would get an
   UnsatisfiedLinkError. Suspending is best effort: without can_suspend the
   window simply stays open, which is better than refusing to hook at all. */
static void suspend_others(suspended_set *set)
{
    set->threads = NULL;
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

    if (kept > 0)
    {
        jvmtiError *results = malloc((size_t)kept * sizeof(*results));
        if (results != NULL)
        {
            (*jvmti)->SuspendThreadList(jvmti, kept, threads, results);
            free(results);
        }
        else
        {
            kept = 0;
        }
    }

    set->threads = threads;
    set->count = kept;
}

static void resume_others(suspended_set *set)
{
    jvmtiEnv *jvmti = jvm_jvmti();
    if (jvmti == NULL)
        return;

    if (set->count > 0)
    {
        jvmtiError *results = malloc((size_t)set->count * sizeof(*results));
        if (results != NULL)
        {
            (*jvmti)->ResumeThreadList(jvmti, set->count, set->threads, results);
            free(results);
        }
    }

    if (set->threads != NULL)
        (*jvmti)->Deallocate(jvmti, (unsigned char *)set->threads);

    set->threads = NULL;
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

/* Rebuilds the class from the bytes it had before the first hook and applies
   every hook that is currently registered for it. Starting from the original
   every time is what lets hooks be added and removed in any order. */
static jni2hook_status reapply(hooked_class *target)
{
    jvmtiEnv *jvmti = jvm_jvmti();
    JNIEnv *env = jvm_env();
    if (jvmti == NULL)
        return JNI2HOOK_ERR_NO_JVMTI;
    if (env == NULL)
        return JNI2HOOK_ERR_NO_JNI;

    jint original_size = 0;
    const unsigned char *original_bytes = class_cache_get(target->class_name, &original_size);
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

    suspended_set suspended;
    suspend_others(&suspended);

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
        for (size_t i = 0; i < target->count; i++)
        {
            hook_entry *entry = &target->hooks[i];

            JNINativeMethod binding;
            binding.name = entry->kind == HOOK_MAKE_NATIVE ? entry->name : entry->added_name;
            binding.signature = entry->kind == HOOK_MAKE_NATIVE ? entry->signature : "()V";
            binding.fnPtr = entry->native_function;

            if ((*env)->RegisterNatives(env, target->klass, &binding, 1) < 0)
            {
                jvm_clear_exception(env);
                result = JNI2HOOK_ERR_JNI;
                break;
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
                    result = JNI2HOOK_ERR_JNI;
                    break;
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

    /* Ask only for what the VM says it can still grant in this phase. An agent
       loaded into a running VM does not get everything an OnLoad agent would,
       and asking for an ungrantable capability fails the whole call. */
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

    /* RedefineClasses will not accept the copy that carries the original body
       unless AllowRedefinitionToAddDeleteMethods is on, and it is off by
       default on every current JDK. Nothing supported can change a product flag
       in a running VM, so the value is set through HotSpot's own flag table. */
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
    return JNI2Hook_Init(vm);
}

jni2hook_status JNI2Hook_Attach(JNIEnv **out_env)
{
    if (!g_initialized)
        return JNI2HOOK_ERR_NOT_INITIALIZED;

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
    if (!g_initialized)
        return JNI2HOOK_ERR_NOT_INITIALIZED;
    if (method == NULL || native_function == NULL)
        return JNI2HOOK_ERR_TRANSFORM;

    hook_mutex_lock(&g_lock);

    jvmtiEnv *jvmti = jvm_jvmti();
    JNIEnv *env = jvm_env();
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

    target = find_class(class_name);
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
    if (!g_initialized)
        return JNI2HOOK_ERR_NOT_INITIALIZED;
    if (pattern == NULL || out_method == NULL || out_bytecode_offset == NULL)
        return JNI2HOOK_ERR_INVALID_PATTERN;

    hook_mutex_lock(&g_lock);

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

jni2hook_status JNI2Hook_Uninstall(jmethodID method)
{
    if (!g_initialized)
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    hook_mutex_lock(&g_lock);

    hooked_class *target = find_class_of_method(method, NULL);
    if (target == NULL)
    {
        hook_mutex_unlock(&g_lock);
        return JNI2HOOK_ERR_NOT_HOOKED;
    }

    size_t kept = 0;
    for (size_t i = 0; i < target->count; i++)
    {
        if (target->hooks[i].method == method)
        {
            free_entry(&target->hooks[i]);
            continue;
        }
        if (kept != i)
            target->hooks[kept] = target->hooks[i];
        kept++;
    }
    target->count = kept;

    const jni2hook_status result = reapply(target);

    if (target->count == 0)
        drop_class(target);

    hook_mutex_unlock(&g_lock);
    return result;
}

int JNI2Hook_IsInstalled(jmethodID method)
{
    if (!g_initialized)
        return 0;

    hook_mutex_lock(&g_lock);
    const int installed = find_class_of_method(method, NULL) != NULL;
    hook_mutex_unlock(&g_lock);
    return installed;
}

void JNI2Hook_Shutdown(void)
{
    if (!g_initialized)
        return;

    hook_mutex_lock(&g_lock);

    while (g_class_count > 0)
    {
        hooked_class *target = &g_classes[g_class_count - 1];
        for (size_t i = 0; i < target->count; i++)
            free_entry(&target->hooks[i]);
        target->count = 0;
        reapply(target);
        drop_class(target);
    }

    free(g_classes);
    g_classes = NULL;
    g_class_count = 0;
    g_class_cap = 0;
    g_insert_serial = 0;

    class_cache_clear();
    class_cache_stop();
    jvm_release();

    g_initialized = false;
    hook_mutex_unlock(&g_lock);
}

jni2hook_status JNI2Hook_FindFieldInClass(jclass target, const char *pattern,
                                          uint32_t instruction_offset, jfieldID *out_field,
                                          int *out_is_static)
{
    if (!g_initialized)
        return JNI2HOOK_ERR_NOT_INITIALIZED;

    JNIEnv *env = jvm_env();
    jvmtiEnv *jvmti = jvm_jvmti();
    if (env == NULL)
        return JNI2HOOK_ERR_NO_JNI;
    if (jvmti == NULL)
        return JNI2HOOK_ERR_NO_JVMTI;

    jvmtiError error = JVMTI_ERROR_NONE;
    const jni2hook_status status = field_scan_find_in_class(
        env, jvmti, target, pattern, instruction_offset, out_field, out_is_static, &error);
    if (status == JNI2HOOK_ERR_JVMTI)
        g_last_error = error;
    return status;
}
