/* Compile check for the vendored JNI and JVMTI headers. Nothing here runs, the
   point is that the declarations line up and that jni_md.h resolves. */

#include <jni.h>
#include <jvmti.h>
#include <jvmticmlr.h>

#include <stdio.h>

_Static_assert(sizeof(jint)  == 4, "jint must be 32 bit");
_Static_assert(sizeof(jlong) == 8, "jlong must be 64 bit");
_Static_assert(sizeof(jbyte) == 1, "jbyte must be 8 bit");

static jvmtiCapabilities wanted;

static jint attach(JavaVM *vm)
{
    jvmtiEnv *jvmti = NULL;
    JNIEnv   *env   = NULL;

    if ((*vm)->GetEnv(vm, (void **)&jvmti, JVMTI_VERSION_1_2) != JNI_OK)
        return JNI_ERR;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK)
        return JNI_ERR;

    wanted.can_redefine_classes       = 1;
    wanted.can_retransform_classes    = 1;
    wanted.can_redefine_any_class     = 1;
    wanted.can_retransform_any_class  = 1;
    wanted.can_suspend                = 1;

    if ((*jvmti)->AddCapabilities(jvmti, &wanted) != JVMTI_ERROR_NONE)
        return JNI_ERR;

    jvmtiEventCallbacks callbacks;
    (void)callbacks;

    jclass clazz = (*env)->FindClass(env, "java/lang/Object");
    if (clazz == NULL)
        return JNI_ERR;

    jvmtiClassDefinition definition;
    definition.klass            = clazz;
    definition.class_byte_count = 0;
    definition.class_bytes      = NULL;

    return (*jvmti)->RedefineClasses(jvmti, 1, &definition) == JVMTI_ERROR_NONE ? JNI_OK : JNI_ERR;
}

int main(void)
{
    printf("jni.h and jvmti.h compile: JNI_VERSION_1_8=0x%X JVMTI_VERSION_1_2=0x%X\n",
           (unsigned)JNI_VERSION_1_8, (unsigned)JVMTI_VERSION_1_2);
    (void)attach;
    return 0;
}
