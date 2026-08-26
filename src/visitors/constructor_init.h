#ifndef JNI2HOOK_VISITORS_CONSTRUCTOR_INIT_H
#define JNI2HOOK_VISITORS_CONSTRUCTOR_INIT_H

#include "code_editor.h"
#include "class_file.h"

/* Finds the invokespecial that consumes the verifier's uninitializedThis
   value. Calls on freshly allocated instances of the current class or its
   superclass are deliberately ignored. */
bool constructor_init_offset(const ClassFile *cf, const code_editor *editor,
                             u4 *out_offset);

#endif
