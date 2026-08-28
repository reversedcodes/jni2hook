#ifndef JNI2HOOK_VISITORS_CONSTRUCTOR_INIT_H
#define JNI2HOOK_VISITORS_CONSTRUCTOR_INIT_H

#include "code_editor.h"
#include "class_file.h"

typedef enum
{
    CONSTRUCTOR_INIT_FOUND = 0,
    /* No path initialises this, so the constructor cannot be instrumented. */
    CONSTRUCTOR_INIT_MISSING,
    /* Different paths reach a different initialising call, so there is no
       single offset after which this is initialised on all of them. Inserting
       at any one of them would instrument that path only, which is worse than
       refusing: the hook would silently not fire on the others. Resolving it
       needs the common post-dominator of those calls, which this does not
       compute. */
    CONSTRUCTOR_INIT_AMBIGUOUS
} constructor_init_result;

/* Finds the invokespecial that consumes the verifier's uninitializedThis
   value. Calls on freshly allocated instances of the current class or its
   superclass are deliberately ignored. */
constructor_init_result constructor_init_offset(const ClassFile *cf, const code_editor *editor,
                                                u4 *out_offset);

#endif
