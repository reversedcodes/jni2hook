#include "jni2hook/utils/class_transform.h"

#include "jni2hook/utils/visitors/code_editor.h"
#include "jni2hook/utils/visitors/constructor_init.h"

#include <stdlib.h>
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
    case TRANSFORM_ERR_BAD_OFFSET:
        return "the offset is not an instruction boundary";
    case TRANSFORM_ERR_AMBIGUOUS_INIT:
        return "this constructor initializes this on more than one path";
    }
    return "unknown error";
}

static bool is_initializer(const ClassFile *cf, const member_info *method)
{
    return classFile_utf8_equals(cf, method->name_index, "<init>") ||
           classFile_utf8_equals(cf, method->name_index, "<clinit>");
}

static bool is_instance_initializer(const ClassFile *cf, const member_info *method)
{
    return classFile_utf8_equals(cf, method->name_index, "<init>");
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

    const u2 restored_index = (u2)(cf->methods.items[index].attributes.count - 1);

    /* The append may have moved the attribute list, so resolve the source again. */
    code = attribute_list_find(&cf->methods.items[copy_index].attributes,
                               &cf->constant_pool, "Code");
    if (code == NULL)
    {
        attribute_list_remove(&cf->methods.items[index].attributes, restored_index);
        return TRANSFORM_ERR_NO_CODE;
    }

    restored->attribute_name_index = code->attribute_name_index;
    status = attribute_info_set_info(restored, code->info, code->attribute_length);
    if (status != CLASSFILE_OK)
    {
        /* Otherwise the method keeps an empty Code attribute and the class is
           left in a state that only fails much later, at link time. */
        attribute_list_remove(&cf->methods.items[index].attributes, restored_index);
        return fail(status, out_cause);
    }

    member_info_set_native(&cf->methods.items[index], false);
    member_list_remove(&cf->methods, copy_index);

    return TRANSFORM_OK;
}


/* One parameter of a method descriptor, reduced to what emitting a forwarding
   call needs: which load opcode reads it, how many local slots it occupies, and
   what it looks like in the callee's own descriptor.

   A reference is always forwarded as java/lang/Object. The callee lives on a
   class jni2hook defined itself, loaded by the bootstrap loader, and that loader
   cannot resolve net/minecraft/whatever -- naming the real type in the callee's
   descriptor would make the verifier try exactly that and fail. Object is
   assignable from every reference, so the check passes and the native side gets
   a jobject either way. */
typedef struct
{
    u1 load_opcode;
    u2 slot;
    char forwarded;
} forwarded_parameter;

static bool describe_parameters(const char *descriptor, bool is_static,
                                forwarded_parameter *out, u2 capacity, u2 *out_count,
                                u2 *out_slots)
{
    if (descriptor == NULL || descriptor[0] != JVM_SIGNATURE_FUNC)
        return false;

    u2 count = 0;
    u2 slot = 0;

    if (!is_static)
    {
        if (capacity == 0)
            return false;
        out[count].load_opcode = JVM_OPC_aload;
        out[count].slot = slot;
        out[count].forwarded = JVM_SIGNATURE_CLASS;
        count++;
        slot++;
    }

    for (const char *cursor = descriptor + 1; *cursor != JVM_SIGNATURE_ENDFUNC; cursor++)
    {
        if (*cursor == 0)
            return false;
        if (count == capacity)
            return false;

        u1 opcode = 0;
        u2 width = 1;
        char forwarded = *cursor;

        while (*cursor == JVM_SIGNATURE_ARRAY)
        {
            cursor++;
            forwarded = JVM_SIGNATURE_CLASS;
            if (*cursor == 0)
                return false;
        }

        switch (*cursor)
        {
        case JVM_SIGNATURE_BYTE:
        case JVM_SIGNATURE_CHAR:
        case JVM_SIGNATURE_SHORT:
        case JVM_SIGNATURE_BOOLEAN:
        case JVM_SIGNATURE_INT:
            opcode = JVM_OPC_iload;
            if (forwarded != JVM_SIGNATURE_CLASS)
                forwarded = *cursor;
            break;
        case JVM_SIGNATURE_LONG:
            opcode = JVM_OPC_lload;
            width = 2;
            if (forwarded != JVM_SIGNATURE_CLASS)
                forwarded = *cursor;
            break;
        case JVM_SIGNATURE_FLOAT:
            opcode = JVM_OPC_fload;
            if (forwarded != JVM_SIGNATURE_CLASS)
                forwarded = *cursor;
            break;
        case JVM_SIGNATURE_DOUBLE:
            opcode = JVM_OPC_dload;
            width = 2;
            if (forwarded != JVM_SIGNATURE_CLASS)
                forwarded = *cursor;
            break;
        case JVM_SIGNATURE_CLASS:
            opcode = JVM_OPC_aload;
            forwarded = JVM_SIGNATURE_CLASS;
            while (*cursor != JVM_SIGNATURE_ENDCLASS)
            {
                cursor++;
                if (*cursor == 0)
                    return false;
            }
            break;
        default:
            return false;
        }

        out[count].load_opcode = opcode;
        out[count].slot = slot;
        out[count].forwarded = forwarded;
        count++;
        slot = (u2)(slot + width);
    }

    *out_count = count;
    *out_slots = slot;
    return true;
}

/* "(I" + one letter or Ljava/lang/Object; per parameter + ")V". */
static bool build_callee_descriptor(const forwarded_parameter *parameters, u2 count,
                                    char *out, size_t capacity)
{
    static const char kObject[] = "Ljava/lang/Object;";

    size_t used = 0;
    if (capacity < 3)
        return false;
    out[used++] = JVM_SIGNATURE_FUNC;
    out[used++] = JVM_SIGNATURE_INT;

    for (u2 i = 0; i < count; i++)
    {
        if (parameters[i].forwarded == JVM_SIGNATURE_CLASS)
        {
            if (used + sizeof(kObject) - 1 >= capacity)
                return false;
            memcpy(out + used, kObject, sizeof(kObject) - 1);
            used += sizeof(kObject) - 1;
        }
        else
        {
            if (used + 1 >= capacity)
                return false;
            out[used++] = parameters[i].forwarded;
        }
    }

    if (used + 3 > capacity)
        return false;
    out[used++] = JVM_SIGNATURE_ENDFUNC;
    out[used++] = JVM_SIGNATURE_VOID;
    out[used] = 0;
    return true;
}


/* The entry state of a method, as a StackMapTable frame.
 *
 * A guarded call branches over the original body, and a branch target needs a
 * frame. Computing one in general means dataflow analysis, which is exactly
 * what this library does not do -- but the target here is the instruction that
 * was at offset 0, and its state is the state the method starts in: the
 * receiver and the declared parameters in their slots, nothing on the stack.
 * That is readable straight off the descriptor, so this one case is tractable
 * where the general one is not.
 *
 * A long or a double takes two local slots but only one entry in the frame. */
static classfile_status build_entry_frame(ClassFile *cf, const char *descriptor, bool is_static,
                                          i4 offset, stack_map_frame *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = FRAME_FULL;
    out->raw_tag = 255;
    out->offset = offset;

    verification_type locals[256];
    u2 count = 0;

    if (!is_static)
    {
        locals[count].tag = JVM_ITEM_Object;
        locals[count].cpool_index = cf->this_class;
        locals[count].offset = 0;
        count++;
    }

    for (const char *cursor = descriptor + 1; *cursor != JVM_SIGNATURE_ENDFUNC; cursor++)
    {
        if (*cursor == 0 || count == (u2)(sizeof(locals) / sizeof(locals[0])))
            return CLASSFILE_ERR_UNSUPPORTED;

        memset(&locals[count], 0, sizeof(locals[count]));

        const char *type = cursor;
        while (*cursor == JVM_SIGNATURE_ARRAY)
        {
            cursor++;
            if (*cursor == 0)
                return CLASSFILE_ERR_UNSUPPORTED;
        }

        const bool is_array = type != cursor;
        /* Decided before the cursor moves: skipping to the ; of an L...; leaves
           it on the semicolon, and testing there sees no reference at all. */
        const bool is_reference = is_array || *cursor == JVM_SIGNATURE_CLASS;

        if (*cursor == JVM_SIGNATURE_CLASS)
        {
            while (*cursor != JVM_SIGNATURE_ENDCLASS)
            {
                cursor++;
                if (*cursor == 0)
                    return CLASSFILE_ERR_UNSUPPORTED;
            }
        }

        if (is_reference)
        {
            /* An array's verification type names the descriptor itself, a plain
               reference names the class inside the L...; wrapper. */
            char name[512];
            const size_t length = is_array ? (size_t)(cursor - type + 1)
                                           : (size_t)(cursor - type - 1);
            const char *start = is_array ? type : type + 1;
            if (length >= sizeof(name))
                return CLASSFILE_ERR_UNSUPPORTED;
            memcpy(name, start, length);
            name[length] = 0;

            u2 class_index = 0;
            const classfile_status status = classFile_intern_class(cf, name, &class_index);
            if (status != CLASSFILE_OK)
                return status;

            locals[count].tag = JVM_ITEM_Object;
            locals[count].cpool_index = class_index;
        }
        else
        {
            switch (*cursor)
            {
            case JVM_SIGNATURE_LONG:
                locals[count].tag = JVM_ITEM_Long;
                break;
            case JVM_SIGNATURE_FLOAT:
                locals[count].tag = JVM_ITEM_Float;
                break;
            case JVM_SIGNATURE_DOUBLE:
                locals[count].tag = JVM_ITEM_Double;
                break;
            case JVM_SIGNATURE_BYTE:
            case JVM_SIGNATURE_CHAR:
            case JVM_SIGNATURE_SHORT:
            case JVM_SIGNATURE_BOOLEAN:
            case JVM_SIGNATURE_INT:
                locals[count].tag = JVM_ITEM_Integer;
                break;
            default:
                return CLASSFILE_ERR_UNSUPPORTED;
            }
        }

        count++;
    }

    if (count != 0)
    {
        out->locals = malloc((size_t)count * sizeof(*out->locals));
        if (out->locals == NULL)
            return CLASSFILE_ERR_OUT_OF_MEMORY;
        memcpy(out->locals, locals, (size_t)count * sizeof(*out->locals));
    }
    out->locals_count = count;
    return CLASSFILE_OK;
}

static classfile_status insert_frame(stack_map_table *table, const stack_map_frame *frame,
                                    bool *out_taken)
{
    /* Kept sorted by offset, because the deltas are rebuilt from that order. */
    *out_taken = false;
    if (table->count == CLASSFILE_MAX_COUNT)
        return CLASSFILE_ERR_LIMIT_EXCEEDED;

    if (table->count == table->capacity)
    {
        u2 capacity = table->capacity ? table->capacity : 8;
        if ((size_t)capacity * 2 > CLASSFILE_MAX_COUNT)
            capacity = CLASSFILE_MAX_COUNT;
        else
            capacity = (u2)(capacity * 2);

        stack_map_frame *grown = realloc(table->frames, (size_t)capacity * sizeof(*grown));
        if (grown == NULL)
            return CLASSFILE_ERR_OUT_OF_MEMORY;
        memset(grown + table->capacity, 0, (size_t)(capacity - table->capacity) * sizeof(*grown));
        table->frames = grown;
        table->capacity = capacity;
    }

    u2 at = 0;
    while (at < table->count && table->frames[at].offset < frame->offset)
        at++;

    if (at < table->count && table->frames[at].offset == frame->offset)
    {
        *out_taken = false;
        return CLASSFILE_OK; /* the point is already described */
    }

    memmove(&table->frames[at + 1], &table->frames[at],
            (size_t)(table->count - at) * sizeof(*table->frames));
    table->frames[at] = *frame;
    table->count++;
    *out_taken = true;
    return CLASSFILE_OK;
}

static u1 return_opcode_for(char kind)
{
    switch (kind)
    {
    case JVM_SIGNATURE_VOID:
        return JVM_OPC_return;
    case JVM_SIGNATURE_LONG:
        return JVM_OPC_lreturn;
    case JVM_SIGNATURE_FLOAT:
        return JVM_OPC_freturn;
    case JVM_SIGNATURE_DOUBLE:
        return JVM_OPC_dreturn;
    case JVM_SIGNATURE_CLASS:
    case JVM_SIGNATURE_ARRAY:
        return JVM_OPC_areturn;
    default:
        return JVM_OPC_ireturn;
    }
}

static u1 default_opcode_for(char kind)
{
    switch (kind)
    {
    case JVM_SIGNATURE_LONG:
        return JVM_OPC_lconst_0;
    case JVM_SIGNATURE_FLOAT:
        return JVM_OPC_fconst_0;
    case JVM_SIGNATURE_DOUBLE:
        return JVM_OPC_dconst_0;
    case JVM_SIGNATURE_CLASS:
    case JVM_SIGNATURE_ARRAY:
        return JVM_OPC_aconst_null;
    default:
        return JVM_OPC_iconst_0;
    }
}

transform_status class_transform_insert_static_call(ClassFile *cf,
                                                    const char *name,
                                                    const char *descriptor,
                                                    u4 at_offset,
                                                    const char *owner,
                                                    const char *callee,
                                                    int argument,
                                                    bool forward_arguments,
                                                    classfile_status *out_cause)
{
    if (out_cause != NULL)
        *out_cause = CLASSFILE_OK;
    if (cf == NULL || name == NULL || descriptor == NULL || owner == NULL || callee == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    if (argument < -32768 || argument > 32767)
        return TRANSFORM_ERR_BAD_OFFSET;

    const member_info *found = classFile_find_method(cf, name, descriptor);
    if (found == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;

    const u2 index = (u2)(found - cf->methods.items);
    if (classFile_utf8_equals(cf, cf->methods.items[index].name_index, "<clinit>"))
        return TRANSFORM_ERR_INITIALIZER;
    if ((cf->methods.items[index].access_flags & JVM_ACC_ABSTRACT) != 0)
        return TRANSFORM_ERR_ABSTRACT;
    if ((cf->methods.items[index].access_flags & JVM_ACC_NATIVE) != 0)
        return TRANSFORM_ERR_ALREADY_NATIVE;

    attribute_info *code = attribute_list_find(&cf->methods.items[index].attributes,
                                               &cf->constant_pool, "Code");
    if (code == NULL)
        return TRANSFORM_ERR_NO_CODE;

    const bool is_constructor = is_instance_initializer(cf, &cf->methods.items[index]);

    u2 owner_index = 0;
    classfile_status status = classFile_intern_class(cf, owner, &owner_index);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    const bool is_static = (cf->methods.items[index].access_flags & JVM_ACC_STATIC) != 0;

    code = attribute_list_find(&cf->methods.items[index].attributes,
                               &cf->constant_pool, "Code");

    code_editor editor;
    status = code_editor_load(code, &cf->constant_pool, &editor);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    if (is_constructor)
    {
        u4 safe_offset = 0;
        const constructor_init_result init = constructor_init_offset(cf, &editor, &safe_offset);
        if (init != CONSTRUCTOR_INIT_FOUND)
        {
            code_editor_free(&editor);
            return init == CONSTRUCTOR_INIT_AMBIGUOUS ? TRANSFORM_ERR_AMBIGUOUS_INIT
                                                      : TRANSFORM_ERR_INITIALIZER;
        }
        if (at_offset < safe_offset)
            at_offset = safe_offset;
    }

    /* The prologue is straight-line code that pushes the id and every argument
       and then hands the lot to invokestatic, which pops all of it again. The
       body therefore starts on exactly the stack it always did, and because
       nothing branches there is no frame to invent -- that is what makes
       forwarding arguments tractable at all. Locals are only read, so max_locals
       is untouched. */
    forwarded_parameter parameters[64];
    u2 parameter_count = 0;
    u2 parameter_slots = 0;
    if (!describe_parameters(descriptor, is_static, parameters,
                             (u2)(sizeof(parameters) / sizeof(parameters[0])),
                             &parameter_count, &parameter_slots))
    {
        code_editor_free(&editor);
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    }

    if (!forward_arguments)
        parameter_count = 0;

    char callee_descriptor[1024];
    if (!build_callee_descriptor(parameters, parameter_count, callee_descriptor,
                                 sizeof(callee_descriptor)))
    {
        code_editor_free(&editor);
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    }

    u2 methodref = 0;
    status = classFile_intern_methodref(cf, owner_index, callee, callee_descriptor, &methodref);
    if (status != CLASSFILE_OK)
    {
        code_editor_free(&editor);
        return fail(status, out_cause);
    }

    instruction nodes[66];
    memset(nodes, 0, sizeof(nodes));
    u4 count = 0;

    nodes[count].opcode = JVM_OPC_sipush;
    nodes[count].kind = OPERAND_IMMEDIATE;
    nodes[count].u.immediate.value = argument;
    count++;

    u2 pushed = 1;
    for (u2 i = 0; i < parameter_count; i++)
    {
        nodes[count].opcode = parameters[i].load_opcode;
        nodes[count].kind = OPERAND_LOCAL;
        nodes[count].u.local.index = parameters[i].slot;
        count++;
        pushed = (u2)(pushed + (parameters[i].load_opcode == JVM_OPC_lload ||
                                        parameters[i].load_opcode == JVM_OPC_dload
                                    ? 2
                                    : 1));
    }

    nodes[count].opcode = JVM_OPC_invokestatic;
    nodes[count].kind = OPERAND_CP_INDEX;
    nodes[count].u.cp.index = methodref;
    count++;

    status = code_editor_insert(&editor, at_offset, nodes, count, pushed);
    if (status == CLASSFILE_OK)
        status = code_editor_store(&editor, code);
    code_editor_free(&editor);

    if (status != CLASSFILE_OK)
        return status == CLASSFILE_ERR_BAD_OFFSET ? TRANSFORM_ERR_BAD_OFFSET
                                                  : fail(status, out_cause);

    return TRANSFORM_OK;
}


transform_status class_transform_insert_guarded_call(ClassFile *cf,
                                                     const char *name,
                                                     const char *descriptor,
                                                     const char *owner,
                                                     const char *callee,
                                                     int argument,
                                                     classfile_status *out_cause)
{
    if (out_cause != NULL)
        *out_cause = CLASSFILE_OK;
    if (cf == NULL || name == NULL || descriptor == NULL || owner == NULL || callee == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    if (argument < -32768 || argument > 32767)
        return TRANSFORM_ERR_BAD_OFFSET;

    const member_info *found = classFile_find_method(cf, name, descriptor);
    if (found == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;

    const u2 index = (u2)(found - cf->methods.items);

    /* Only at method entry, and not in an initialiser. Everywhere else the
       branch target's frame would need real dataflow, and in <init> the
       receiver is still uninitialised at offset 0. */
    if (is_initializer(cf, &cf->methods.items[index]))
        return TRANSFORM_ERR_INITIALIZER;
    if ((cf->methods.items[index].access_flags & JVM_ACC_ABSTRACT) != 0)
        return TRANSFORM_ERR_ABSTRACT;
    if ((cf->methods.items[index].access_flags & JVM_ACC_NATIVE) != 0)
        return TRANSFORM_ERR_ALREADY_NATIVE;

    attribute_info *code = attribute_list_find(&cf->methods.items[index].attributes,
                                               &cf->constant_pool, "Code");
    if (code == NULL)
        return TRANSFORM_ERR_NO_CODE;

    const bool is_static = (cf->methods.items[index].access_flags & JVM_ACC_STATIC) != 0;

    const char *closing = strchr(descriptor, JVM_SIGNATURE_ENDFUNC);
    if (closing == NULL || closing[1] == 0)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    const char returns = closing[1];

    u2 owner_index = 0;
    classfile_status status = classFile_intern_class(cf, owner, &owner_index);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    code_editor editor;
    status = code_editor_load(code, &cf->constant_pool, &editor);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    forwarded_parameter parameters[64];
    u2 parameter_count = 0;
    u2 parameter_slots = 0;
    if (!describe_parameters(descriptor, is_static, parameters,
                             (u2)(sizeof(parameters) / sizeof(parameters[0])),
                             &parameter_count, &parameter_slots))
    {
        code_editor_free(&editor);
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    }

    char callee_descriptor[1024];
    if (!build_callee_descriptor(parameters, parameter_count, callee_descriptor,
                                 sizeof(callee_descriptor)))
    {
        code_editor_free(&editor);
        return TRANSFORM_ERR_METHOD_NOT_FOUND;
    }
    /* The guard answers a question, so it returns a boolean rather than void. */
    callee_descriptor[strlen(callee_descriptor) - 1] = JVM_SIGNATURE_BOOLEAN;

    u2 methodref = 0;
    status = classFile_intern_methodref(cf, owner_index, callee, callee_descriptor, &methodref);
    if (status != CLASSFILE_OK)
    {
        code_editor_free(&editor);
        return fail(status, out_cause);
    }

    /* sipush id, one load per argument, invokestatic, ifeq over the body, then
       the value to return in its place. */
    instruction nodes[70];
    memset(nodes, 0, sizeof(nodes));
    u4 count = 0;

    nodes[count].opcode = JVM_OPC_sipush;
    nodes[count].kind = OPERAND_IMMEDIATE;
    nodes[count].u.immediate.value = argument;
    count++;

    u2 pushed = 1;
    for (u2 i = 0; i < parameter_count; i++)
    {
        nodes[count].opcode = parameters[i].load_opcode;
        nodes[count].kind = OPERAND_LOCAL;
        nodes[count].u.local.index = parameters[i].slot;
        count++;
        pushed = (u2)(pushed + (parameters[i].load_opcode == JVM_OPC_lload ||
                                        parameters[i].load_opcode == JVM_OPC_dload
                                    ? 2
                                    : 1));
    }

    nodes[count].opcode = JVM_OPC_invokestatic;
    nodes[count].kind = OPERAND_CP_INDEX;
    nodes[count].u.cp.index = methodref;
    count++;

    const u4 branch_node = count;
    nodes[count].opcode = JVM_OPC_ifeq;
    nodes[count].kind = OPERAND_BRANCH;
    nodes[count].u.branch.target = 0; /* patched once the offsets are known */
    count++;

    if (returns != JVM_SIGNATURE_VOID)
    {
        nodes[count].opcode = default_opcode_for(returns);
        nodes[count].kind = OPERAND_NONE;
        count++;
        if (pushed < 2)
            pushed = 2;
    }

    nodes[count].opcode = return_opcode_for(returns);
    nodes[count].kind = OPERAND_NONE;
    count++;

    status = code_editor_insert(&editor, 0, nodes, count, pushed);
    if (status != CLASSFILE_OK)
    {
        code_editor_free(&editor);
        return status == CLASSFILE_ERR_BAD_OFFSET ? TRANSFORM_ERR_BAD_OFFSET
                                                  : fail(status, out_cause);
    }

    /* The body now starts at index count, and that is where the branch goes and
       where the frame belongs. Both are settled here rather than before the
       splice, because only now are the offsets final. */
    if (editor.instructions.count <= count)
    {
        code_editor_free(&editor);
        return fail(CLASSFILE_ERR_UNSUPPORTED, out_cause);
    }

    const i4 body_offset = (i4)editor.instructions.items[count].offset;
    editor.instructions.items[branch_node].u.branch.target = body_offset;

    stack_map_frame frame;
    status = build_entry_frame(cf, descriptor, is_static, body_offset, &frame);
    if (status == CLASSFILE_OK)
    {
        /* The table takes the frame unless that offset was already described,
           in which case the existing one already says what this one would. */
        bool taken = false;
        status = insert_frame(&editor.stack_map, &frame, &taken);
        if (!taken)
            free(frame.locals);
    }
    if (status != CLASSFILE_OK)
    {
        code_editor_free(&editor);
        return fail(status, out_cause);
    }

    if (!editor.has_stack_map)
    {
        u2 attribute_name = 0;
        status = classFile_intern_utf8(cf, "StackMapTable", &attribute_name);
        if (status != CLASSFILE_OK)
        {
            code_editor_free(&editor);
            return fail(status, out_cause);
        }
        editor.stack_map_name_index = attribute_name;
        editor.has_stack_map = true;
    }

    code = attribute_list_find(&cf->methods.items[index].attributes,
                               &cf->constant_pool, "Code");
    status = code_editor_store(&editor, code);
    code_editor_free(&editor);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    return TRANSFORM_OK;
}

transform_status class_transform_insert_call(ClassFile *cf,
                                             const char *name,
                                             const char *descriptor,
                                             u4 at_offset,
                                             const char *hook_name,
                                             classfile_status *out_cause)
{
    if (out_cause != NULL)
        *out_cause = CLASSFILE_OK;
    if (cf == NULL || name == NULL || descriptor == NULL || hook_name == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;

    if ((cf->access_flags & JVM_ACC_INTERFACE) != 0)
        return TRANSFORM_ERR_INTERFACE;
    if (classFile_find_method(cf, hook_name, "()V") != NULL)
        return TRANSFORM_ERR_NAME_IN_USE;

    const member_info *found = classFile_find_method(cf, name, descriptor);
    if (found == NULL)
        return TRANSFORM_ERR_METHOD_NOT_FOUND;

    const u2 index = (u2)(found - cf->methods.items);
    const bool is_constructor = is_instance_initializer(cf, &cf->methods.items[index]);
    if (classFile_utf8_equals(cf, cf->methods.items[index].name_index, "<clinit>"))
        return TRANSFORM_ERR_INITIALIZER;
    if ((cf->methods.items[index].access_flags & JVM_ACC_ABSTRACT) != 0)
        return TRANSFORM_ERR_ABSTRACT;
    if ((cf->methods.items[index].access_flags & JVM_ACC_NATIVE) != 0)
        return TRANSFORM_ERR_ALREADY_NATIVE;

    attribute_info *code = attribute_list_find(&cf->methods.items[index].attributes,
                                               &cf->constant_pool, "Code");
    if (code == NULL)
        return TRANSFORM_ERR_NO_CODE;

    const bool is_static = (cf->methods.items[index].access_flags & JVM_ACC_STATIC) != 0;

    u2 hook_name_index = 0;
    classfile_status status = classFile_intern_utf8(cf, hook_name, &hook_name_index);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    u2 void_index = 0;
    status = classFile_intern_utf8(cf, "()V", &void_index);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    u2 methodref = 0;
    status = classFile_intern_methodref(cf, cf->this_class, hook_name, "()V", &methodref);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    /* The callee is declared before its call site is written. Doing it the
       other way round left a body invoking a method that does not exist if the
       append then failed, and that only surfaces when the class is linked. */
    member_info *hook = NULL;
    status = member_list_append(&cf->methods, &hook);
    if (status != CLASSFILE_OK)
        return fail(status, out_cause);

    const u2 hook_index = (u2)(cf->methods.count - 1);
    hook->access_flags = (u2)(JVM_ACC_PRIVATE | JVM_ACC_FINAL | JVM_ACC_NATIVE |
                              (is_static ? JVM_ACC_STATIC : 0));
    hook->name_index = hook_name_index;
    hook->descriptor_index = void_index;

    /* Interning and the append may have reallocated the pool and the method
       list, so the Code attribute is resolved again from the index. */
    code = attribute_list_find(&cf->methods.items[index].attributes,
                               &cf->constant_pool, "Code");

    code_editor editor;
    status = code_editor_load(code, &cf->constant_pool, &editor);
    if (status != CLASSFILE_OK)
    {
        member_list_remove(&cf->methods, hook_index);
        return fail(status, out_cause);
    }

    if (is_constructor)
    {
        u4 safe_offset = 0;
        const constructor_init_result init = constructor_init_offset(cf, &editor, &safe_offset);
        if (init != CONSTRUCTOR_INIT_FOUND)
        {
            code_editor_free(&editor);
            member_list_remove(&cf->methods, hook_index);
            return init == CONSTRUCTOR_INIT_AMBIGUOUS ? TRANSFORM_ERR_AMBIGUOUS_INIT
                                                      : TRANSFORM_ERR_INITIALIZER;
        }
        if (at_offset < safe_offset)
            at_offset = safe_offset;
    }

    instruction nodes[2];
    memset(nodes, 0, sizeof(nodes));
    u4 count = 0;

    if (!is_static)
    {
        nodes[count].opcode = JVM_OPC_aload_0;
        nodes[count].kind = OPERAND_NONE;
        count++;
    }
    nodes[count].opcode = is_static ? JVM_OPC_invokestatic : JVM_OPC_invokespecial;
    nodes[count].kind = OPERAND_CP_INDEX;
    nodes[count].u.cp.index = methodref;
    count++;

    status = code_editor_insert(&editor, at_offset, nodes, count, 1);
    if (status == CLASSFILE_OK)
        status = code_editor_store(&editor, code);
    code_editor_free(&editor);

    if (status != CLASSFILE_OK)
    {
        member_list_remove(&cf->methods, hook_index);
        return status == CLASSFILE_ERR_BAD_OFFSET ? TRANSFORM_ERR_BAD_OFFSET
                                                  : fail(status, out_cause);
    }

    return TRANSFORM_OK;
}
