#include "class_transform.h"

#include <string.h>

const char *transform_status_message(transform_status status)
{
    switch (status)
    {
    case TRANSFORM_OK:
        return "ok";
    case TRANSFORM_ERR_METHOD_NOT_FOUND:
        return "no such method in this class";
    case TRANSFORM_ERR_ALREADY_NATIVE:
        return "method is already native";
    case TRANSFORM_ERR_ABSTRACT:
        return "an abstract method has no body to move";
    case TRANSFORM_ERR_INITIALIZER:
        return "<init> and <clinit> may not be made native";
    case TRANSFORM_ERR_INTERFACE:
        return "a method of an interface may not be made native";
    case TRANSFORM_ERR_NO_CODE:
        return "method has no Code attribute";
    case TRANSFORM_ERR_NAME_IN_USE:
        return "the copy name is already taken by another method";
    case TRANSFORM_ERR_CLASSFILE:
        return "class file operation failed";
    }
    return "unknown error";
}

static bool is_initializer(const ClassFile *cf, const member_info *method)
{
    return classFile_utf8_equals(cf, method->name_index, "<init>") ||
           classFile_utf8_equals(cf, method->name_index, "<clinit>");
}

static u2 copy_flags(u2 original_flags)
{
   /* Private plus static or final is the only shape RedefineClasses accepts for
      a method that was not in the class before. On top of that the copy has to
      keep every flag that describes the body it inherits: SYNCHRONIZED or the
      moved code loses its monitor, BRIDGE or the verifier stops being lenient
      about the covariant return of a javac bridge, STRICT for the float
      semantics of old class files, VARARGS and SYNTHETIC for bookkeeping. */
   const u2 inherited = (u2)(JVM_ACC_STATIC | JVM_ACC_SYNCHRONIZED | JVM_ACC_BRIDGE |
                             JVM_ACC_VARARGS | JVM_ACC_STRICT | JVM_ACC_SYNTHETIC);
   return (u2)(JVM_ACC_PRIVATE | JVM_ACC_FINAL | (original_flags & inherited));
}

static transform_status fail(classfile_status cause, classfile_status *out_cause)
{
    if (out_cause != NULL)
        *out_cause = cause;
    return TRANSFORM_ERR_CLASSFILE;
}

transform_status class_transform_make_native(ClassFile *cf,
                                             const char *name,
                                             const char *descriptor,
                                             const char *copy_name,
                                             classfile_status *out_cause)
{
    if (out_cause != NULL)
        *out_cause = CLASSFILE_OK;
    if (cf == NULL || name == NULL || descriptor == NULL || copy_name == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;

    if ((cf->access_flags & JVM_ACC_INTERFACE) != 0)
        return TRANSFORM_ERR_INTERFACE;

    if (classFile_find_method(cf, copy_name, descriptor) != NULL)
        return TRANSFORM_ERR_NAME_IN_USE;

    const member_info *found = classFile_find_method(cf, name, descriptor);
    if (found == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;

    const u2 index = (u2)(found - cf->methods.items);
    const member_info *method = &cf->methods.items[index];

    if (is_initializer(cf, method))
        return TRANSFORM_ERR_INITIALIZER;
    if ((method->access_flags & JVM_ACC_NATIVE) != 0)
        return TRANSFORM_ERR_ALREADY_NATIVE;
    if ((method->access_flags & JVM_ACC_ABSTRACT) != 0)
        return TRANSFORM_ERR_ABSTRACT;
    if (attribute_list_find(&method->attributes, &cf->constant_pool, "Code") == NULL)
        return TRANSFORM_ERR_NO_CODE;

    u2 copy_name_index = 0;
    classfile_status status = classFile_intern_utf8(cf, copy_name, &copy_name_index);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    /* Take the copy from the original before the list can move under us. */
    member_info copy;
    status = member_info_copy(&cf->methods.items[index], &copy);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    copy.name_index = copy_name_index;
    copy.access_flags = copy_flags(copy.access_flags);

    member_info *slot = NULL;
    status = member_list_append(&cf->methods, &slot);
    if (status != CLASSFILE_OK)
    {
        member_info_free(&copy);
        return fail(status, out_cause);
    }
    *slot = copy;

    /* Appending may have reallocated the list, so look the original up again. */
    member_info *original = &cf->methods.items[index];
    member_info_set_native(original, true);
    attribute_list_remove_named(&original->attributes, &cf->constant_pool, "Code");

    return TRANSFORM_OK;
}

transform_status class_transform_restore(ClassFile *cf,
                                         const char *name,
                                         const char *descriptor,
                                         const char *copy_name,
                                         classfile_status *out_cause)
{
    if (out_cause != NULL)
        *out_cause = CLASSFILE_OK;
    if (cf == NULL || name == NULL || descriptor == NULL || copy_name == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;

    const member_info *found_copy = classFile_find_method(cf, copy_name, descriptor);
    if (found_copy == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    const u2 copy_index = (u2)(found_copy - cf->methods.items);

    const member_info *found = classFile_find_method(cf, name, descriptor);
    if (found == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    const u2 index = (u2)(found - cf->methods.items);

    const attribute_info *code = attribute_list_find(&cf->methods.items[copy_index].attributes,
                                                     &cf->constant_pool, "Code");
    if (code == NULL)
        return TRANSFORM_ERR_NO_CODE;

    attribute_info *restored = NULL;
    classfile_status status = attribute_list_append(&cf->methods.items[index].attributes, &restored);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    /* The append may have moved the attribute list, so resolve the source again. */
    code = attribute_list_find(&cf->methods.items[copy_index].attributes,
                               &cf->constant_pool, "Code");
    restored->attribute_name_index = code->attribute_name_index;
    status = attribute_info_set_info(restored, code->info, code->attribute_length);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    member_info_set_native(&cf->methods.items[index], false);
    member_list_remove(&cf->methods, copy_index);

    return TRANSFORM_OK;
}
