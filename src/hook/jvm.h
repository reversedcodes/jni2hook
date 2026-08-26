#ifndef JNI2HOOK_HOOK_JVM_H
#define JNI2HOOK_HOOK_JVM_H

#include <jni.h>
#include <jvmti.h>

#include <stdbool.h>

bool jvm_bind(JavaVM *vm);
void jvm_release(void);

JavaVM   *jvm_vm(void);
jvmtiEnv *jvm_jvmti(void);

/* Returns a JNIEnv for the calling thread, attaching it as a daemon if it is
   not a Java thread yet. Attaching is unconditional and never gated on a phase
   query: on an injected library the calling thread is usually the process's
   primordial thread, where every jvmtiEnv entry point including GetPhase fails
   with JVMTI_ERROR_UNATTACHED_THREAD until the attach has happened. */
JNIEnv *jvm_env(void);

/* True once the VM has reached the live phase. Attaches first, for the reason
   above, so it is safe to call from a thread the JVM has never seen. */
bool jvm_is_live(void);

/* Clears a pending Java exception and reports whether there was one. */
bool jvm_clear_exception(JNIEnv *env);

/* Copies a JVMTI allocated string into a malloc'd one and deallocates it. */
char *jvm_take_string(char *jvmti_string);

/* "Ljava/lang/String;" -> "java/lang/String", malloc'd. */
char *jvm_class_name_of(jclass klass);

#endif
