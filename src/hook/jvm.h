#ifndef JNI2HOOK_HOOK_JVM_H
#define JNI2HOOK_HOOK_JVM_H

#include <jni.h>
#include <jvmti.h>

#include <stdbool.h>

/* Finds the JVM already running in this process. */
JavaVM *jvm_find_running(void);

bool jvm_bind(JavaVM *vm);
void jvm_release(void);

JavaVM   *jvm_vm(void);
jvmtiEnv *jvm_jvmti(void);

/* Returns a JNIEnv, attaching the calling thread as a daemon when needed. */
JNIEnv *jvm_env(void);

/* Attaches the caller before querying whether the VM is live. */
bool jvm_is_live(void);

/* Clears a pending Java exception and reports whether there was one. */
bool jvm_clear_exception(JNIEnv *env);

/* Copies a JVMTI allocated string into a malloc'd one and deallocates it. */
char *jvm_take_string(char *jvmti_string);

/* "Ljava/lang/String;" -> "java/lang/String", malloc'd. */
char *jvm_class_name_of(jclass klass);

#endif
