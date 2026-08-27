#ifndef JNI2HOOK_CLASS_TRANSFORM_H
#define JNI2HOOK_CLASS_TRANSFORM_H

#include "jni2hook/utils/visitors/class_file.h"

/* Turns a method into a native one and parks its body in a copy, which is the
   rewrite every hook is built on.
 *
 *   before   int compute(int)            { <body> }
 *   after    native int compute(int);
 *            private final int <copy>(int) { <body> }
 *
 * The caller then binds compute to its own C function with RegisterNatives and
 * reaches the original through the copy.
 *
 * The flags on the copy are not a style choice. RedefineClasses refuses added
 * methods unless they are private and either static or final, so those are the
 * only flags a copy may carry; see compare_and_normalize_class_versions in
 * HotSpot's jvmtiRedefineClasses.cpp. The NATIVE bit on the original is the one
 * modifier change the same check lets through.
 */

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
   TRANSFORM_ERR_BAD_OFFSET
} transform_status;

const char *transform_status_message(transform_status status);

/* On success the class file holds the native method and the copy. The status
   of the underlying class file operation is reported through out_cause when it
   is not NULL and the result is TRANSFORM_ERR_CLASSFILE. */
transform_status class_transform_make_native(ClassFile *cf,
                                             const char *name,
                                             const char *descriptor,
                                             const char *copy_name,
                                             classfile_status *out_cause);

/* Inserts a call to a fresh native method at a bytecode offset, leaving the
   body itself in place. This is the shape a hook takes when it has to observe a
   point inside a method rather than replace it:
 *
 *     public int compute(int a) {
 *         this.<hook>();          <- inserted at the requested offset
 *         <original body>
 *     }
 *
 * The hook method is declared native so the caller can bind it with
 * RegisterNatives. It takes no arguments; reaching the locals of the method it
 * sits in is not part of this. The offset has to name an instruction boundary,
 * which is what a bytecode signature scan yields anyway. In <init>, an offset
 * before the initializing this()/super() invokespecial is moved directly after
 * that call, where this is a regular initialized reference. */
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
