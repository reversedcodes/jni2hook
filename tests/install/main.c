#include <jni2hook/jni2hook.h>
#include <jni2hook/utils/class_file.h>

#include <stdio.h>

int main(void)
{
    printf("%s %s\n", JNI2Hook_StatusMessage(JNI2HOOK_OK), classfile_status_message(CLASSFILE_OK));
    return 0;
}
