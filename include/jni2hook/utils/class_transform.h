#ifndef JNI2HOOK_CLASS_TRANSFORM_H
#define JNI2HOOK_CLASS_TRANSFORM_H

#include "jni2hook/utils/visitors/class_file.h"

typedef enum
{
   TRANSFORM_OK = 0,
   TRANSFORM_ERR_METHOD_NOT_FOUND,
   TRANSFORM_ERR_ALREADY_NATIVE,
   TRANSFORM_ERR_ABSTRACT,
   TRANSFORM_ERR_INITIALIZER,
   TRANSFORM_ERR_INTERFACE,
   TRANSFORM_ERR_NO_CODE,
   TRANSFORM_ERR_NAME_IN_USE,
   TRANSFORM_ERR_CLASSFILE,
   TRANSFORM_ERR_BAD_OFFSET,
   TRANSFORM_ERR_AMBIGUOUS_INIT
} transform_status;

const char *transform_status_message(transform_status status);

/* Makes the method native and moves its body to a private final copy, also
   static when the original is static. out_cause describes class-file errors. */
transform_status class_transform_make_native(ClassFile *cf,
                                             const char *name,
                                             const char *descriptor,
                                             const char *copy_name,
                                             classfile_status *out_cause);

/* Inserts a call to an existing static method without adding a method to the
   target class. owner is an internal class name. The callee always receives
   argument first; forward_arguments additionally passes the receiver and
   parameters, with reference types erased to java/lang/Object. */
transform_status class_transform_insert_static_call(ClassFile *cf,
                                                    const char *name,
                                                    const char *descriptor,
                                                    u4 at_offset,
                                                    const char *owner,
                                                    const char *callee,
                                                    int argument,
                                                    bool forward_arguments,
                                                    classfile_status *out_cause);

/* Inserts an entry guard. A true result skips the body and returns either the
   type's default or value_callee(argument). Initializers are unsupported; only
   method entry has a StackMapTable state derivable without dataflow analysis. */
transform_status class_transform_insert_guarded_call(ClassFile *cf,
                                                     const char *name,
                                                     const char *descriptor,
                                                     const char *owner,
                                                     const char *callee,
                                                     const char *value_callee,
                                                     int argument,
                                                     classfile_status *out_cause);

/* Inserts a call to a new native hook method at an instruction boundary. A
   constructor offset before this()/super() is moved after initialization. */
transform_status class_transform_insert_call(ClassFile *cf,
                                             const char *name,
                                             const char *descriptor,
                                             u4 at_offset,
                                             const char *hook_name,
                                             classfile_status *out_cause);

/* Undoes the rewrite: moves the body back from the copy and drops it again. */
transform_status class_transform_restore(ClassFile *cf,
                                         const char *name,
                                         const char *descriptor,
                                         const char *copy_name,
                                         classfile_status *out_cause);

#endif
