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
   TRANSFORM_ERR_BAD_OFFSET,
   TRANSFORM_ERR_AMBIGUOUS_INIT
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
/* Inserts a call to a static method of *another* class, adding nothing to this
 * one.
 *
 * That distinction is the whole point. class_transform_insert_call and
 * class_transform_make_native both put a new method on the class they rewrite,
 * and RedefineClasses only accepts an added method while
 * AllowRedefinitionToAddDeleteMethods is on -- a deprecated product flag that
 * has to be written straight into HotSpot's flag table because nothing
 * supported can change it in a running VM. Every hook currently rests on that.
 *
 * Changing only the bytecode of an existing method is the supported case and
 * needs no flag at all. So if the callee lives on a class jni2hook defined
 * itself, the target class only ever gets its constant pool grown and one
 * method's Code rewritten.
 *
 * owner is an internal name ("com/example/Hooks"), and the callee has to exist
 * by the time the method runs.
 *
 * With forward_arguments the receiver and every parameter are passed along, read
 * straight out of the local slots they already occupy at method entry, and the
 * callee's descriptor is derived from the target's. References are forwarded as
 * java/lang/Object, because the callee's own loader cannot be expected to
 * resolve the target's types. The callee always takes the int argument first:
 *
 *   int compute(int)          ->  callee(I, Object, int)
 *   static void run(String)   ->  callee(int, Object)
 *
 * Without it only the int is passed, which is enough to identify the hook. */
transform_status class_transform_insert_static_call(ClassFile *cf,
                                                    const char *name,
                                                    const char *descriptor,
                                                    u4 at_offset,
                                                    const char *owner,
                                                    const char *callee,
                                                    int argument,
                                                    bool forward_arguments,
                                                    classfile_status *out_cause);

/* A hook that can decide whether the original body runs at all, still without
 * adding anything to the class being hooked.
 *
 * The callee takes the same arguments class_transform_insert_static_call
 * forwards and returns a boolean. False lets the body run untouched; true skips
 * it and returns the default for the method's own return type.
 *
 * Only at method entry, and not in an initialiser. Skipping the body means
 * branching over it, a branch target needs a StackMapTable frame, and computing
 * one in general is dataflow analysis this library does not do. At offset 0 the
 * frame is the state the method starts in -- the receiver and the declared
 * parameters, an empty stack -- which is readable off the descriptor. Nowhere
 * else is. */
transform_status class_transform_insert_guarded_call(ClassFile *cf,
                                                     const char *name,
                                                     const char *descriptor,
                                                     const char *owner,
                                                     const char *callee,
                                                     int argument,
                                                     classfile_status *out_cause);

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
