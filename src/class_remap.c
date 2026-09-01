#include "jni2hook/jni2hook.h"

#include "jni2hook/utils/class_file_parser.h"

#include <stdlib.h>
#include <string.h>

/* Remapping never rewrites a Utf8 entry in place. Two references can share one
   Utf8, and a method name is spelled the same way as an unrelated field name or
   an attribute name, so editing the entry would reach further than intended.
   Every changed reference interns the new text and moves its index instead,
   which leaves the original entry for whoever else still points at it. */

typedef struct
{
    const jni2hook_class_mapping *classes;
    size_t class_count;

    const jni2hook_method_mapping *methods;
    size_t method_count;

    const jni2hook_field_mapping *fields;
    size_t field_count;

    /* The name every CONSTANT_Class entry had on entry, indexed by constant
       pool index. Member lookups run against these, so the order in which the
       passes rename things cannot change what a lookup matches. NULL for a
       slot that is not a class. */
    char **original_class_names;
    u2 name_count;
} remap_context;

/* A growable NUL terminated string. The latching ok flag matches byte_buffer,
   so a build can run to the end and be checked once. */
typedef struct
{
    char *data;
    size_t size;
    size_t capacity;
    bool ok;
} strbuf;

static void strbuf_init(strbuf *b)
{
    b->data = NULL;
    b->size = 0;
    b->capacity = 0;
    b->ok = true;
}

static void strbuf_free(strbuf *b)
{
    free(b->data);
    strbuf_init(b);
}

static void strbuf_add(strbuf *b, const char *text, size_t length)
{
    if (!b->ok)
        return;

    if (b->size + length + 1 > b->capacity)
    {
        size_t capacity = b->capacity == 0 ? 64 : b->capacity;
        while (capacity < b->size + length + 1)
            capacity *= 2;

        char *grown = realloc(b->data, capacity);
        if (grown == NULL)
        {
            b->ok = false;
            return;
        }

        b->data = grown;
        b->capacity = capacity;
    }

    memcpy(b->data + b->size, text, length);
    b->size += length;
    b->data[b->size] = 0;
}

static char *duplicate(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy == NULL)
        return NULL;

    memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

/* Utf8 entries are counted, not terminated. Working on terminated copies keeps
   the mapping code from carrying a length through every comparison. */
static char *utf8_duplicate(const constant_pool *pool, u2 index)
{
    const u1 *bytes = NULL;
    u2 length = 0;

    if (!constant_pool_utf8(pool, index, &bytes, &length))
        return NULL;

    return duplicate((const char *)bytes, length);
}

static bool equals(const char *text, const char *other)
{
    return text != NULL && other != NULL && strcmp(text, other) == 0;
}

static const char *lookup_class(const remap_context *ctx, const char *name, size_t length)
{
    for (size_t i = 0; i < ctx->class_count; i++)
    {
        const char *from = ctx->classes[i].from;
        if (from == NULL || ctx->classes[i].to == NULL)
            continue;
        if (strlen(from) == length && memcmp(from, name, length) == 0)
            return ctx->classes[i].to;
    }

    return NULL;
}

/* Rewrites the object types inside a descriptor. Array depth and primitives
   pass through untouched, since only the name between 'L' and ';' can move. */
static char *remap_descriptor(const remap_context *ctx, const char *descriptor, bool *changed)
{
    strbuf out;
    strbuf_init(&out);
    *changed = false;

    const char *p = descriptor;
    while (*p != 0)
    {
        if (*p != 'L')
        {
            strbuf_add(&out, p, 1);
            p++;
            continue;
        }

        const char *end = strchr(p + 1, ';');
        if (end == NULL)
        {
            /* Not a well formed descriptor. Copy the rest verbatim rather than
               guessing where the name was meant to stop. */
            strbuf_add(&out, p, strlen(p));
            break;
        }

        const size_t length = (size_t)(end - (p + 1));
        const char *to = lookup_class(ctx, p + 1, length);

        strbuf_add(&out, "L", 1);
        if (to != NULL)
        {
            strbuf_add(&out, to, strlen(to));
            *changed = true;
        }
        else
        {
            strbuf_add(&out, p + 1, length);
        }
        strbuf_add(&out, ";", 1);

        p = end + 1;
    }

    if (!out.ok)
    {
        strbuf_free(&out);
        return NULL;
    }

    if (out.data == NULL)
        return duplicate("", 0);

    return out.data;
}

/* A CONSTANT_Class name is an internal name, except for an array type, where
   the entry holds a descriptor instead (JVMS 4.4.1). */
static char *remap_class_name(const remap_context *ctx, const char *name, bool *changed)
{
    if (name[0] == '[')
        return remap_descriptor(ctx, name, changed);

    const char *to = lookup_class(ctx, name, strlen(name));
    if (to == NULL)
    {
        *changed = false;
        return NULL;
    }

    *changed = true;
    return duplicate(to, strlen(to));
}

static const jni2hook_member_mapping *find_member(const jni2hook_member_mapping *table,
                                                  size_t count, const char *owner, const char *name,
                                                  const char *descriptor)
{
    if (owner == NULL || name == NULL)
        return NULL;

    for (size_t i = 0; i < count; i++)
    {
        const jni2hook_member_mapping *entry = &table[i];
        if (!equals(entry->owner, owner) || !equals(entry->name, name))
            continue;
        if (entry->descriptor != NULL && !equals(entry->descriptor, descriptor))
            continue;

        return entry;
    }

    return NULL;
}

/* Records what every CONSTANT_Class entry was called before anything moved. */
static classfile_status snapshot_class_names(ClassFile *cf, remap_context *ctx)
{
    const u2 count = cf->constant_pool.count;

    ctx->original_class_names = calloc(count == 0 ? 1 : count, sizeof(char *));
    if (ctx->original_class_names == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    ctx->name_count = count;

    for (u2 i = 1; i < count; i++)
    {
        const cp_info *entry = constant_pool_at(&cf->constant_pool, i);
        if (entry == NULL || entry->tag != JVM_CONSTANT_Class)
            continue;

        ctx->original_class_names[i] =
            utf8_duplicate(&cf->constant_pool, entry->u.class_info.name_index);
        if (ctx->original_class_names[i] == NULL)
            return CLASSFILE_ERR_OUT_OF_MEMORY;
    }

    return CLASSFILE_OK;
}

static const char *original_class_name(const remap_context *ctx, u2 index)
{
    if (index == 0 || index >= ctx->name_count)
        return NULL;

    return ctx->original_class_names[index];
}

/* javac writes the static type of the receiver as the owner of a reference, so
   a call to an inherited method names the subclass and not the class that
   declares it. Looking the declaring class up would then miss, and the call
   would keep its compile-time name and fail to link. A lookup that misses on
   this class therefore tries the super class and the interfaces as well, which
   is as far up the hierarchy as the class file itself describes.

   An owner that is some other class is left at the exact match: resolving that
   would need the class files of its super types, which are not here. */
static const jni2hook_member_mapping *find_member_inherited(const ClassFile *cf,
                                                            const remap_context *ctx,
                                                            const jni2hook_member_mapping *table,
                                                            size_t count, const char *owner,
                                                            const char *name,
                                                            const char *descriptor)
{
    const jni2hook_member_mapping *mapping = find_member(table, count, owner, name, descriptor);
    if (mapping != NULL)
        return mapping;

    const char *self = original_class_name(ctx, cf->this_class);
    if (self == NULL || owner == NULL || strcmp(self, owner) != 0)
        return NULL;

    mapping = find_member(table, count, original_class_name(ctx, cf->super_class), name, descriptor);

    for (u2 i = 0; i < cf->interfaces.count && mapping == NULL; i++)
    {
        mapping = find_member(table, count, original_class_name(ctx, cf->interfaces.items[i]), name,
                              descriptor);
    }

    return mapping;
}

/* Rewrites the NameAndType of one reference. mapping may be NULL, in which
   case only the descriptor moves, which is what a reference to an untouched
   member of a renamed class needs. */
static classfile_status remap_name_and_type(ClassFile *cf, const remap_context *ctx, u2 nat_index,
                                            const jni2hook_member_mapping *mapping, u2 *out_index,
                                            bool *changed)
{
    *changed = false;

    const cp_info *nat = constant_pool_at(&cf->constant_pool, nat_index);
    if (nat == NULL || nat->tag != JVM_CONSTANT_NameAndType)
        return CLASSFILE_OK;

    char *name = utf8_duplicate(&cf->constant_pool, nat->u.nat.name_index);
    char *descriptor = utf8_duplicate(&cf->constant_pool, nat->u.nat.descriptor_index);
    if (name == NULL || descriptor == NULL)
    {
        free(name);
        free(descriptor);
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    }

    const char *new_name = name;
    if (mapping != NULL && mapping->runtime_name != NULL)
    {
        new_name = mapping->runtime_name;
        *changed = true;
    }

    char *mapped_descriptor = NULL;
    const char *new_descriptor = descriptor;

    if (mapping != NULL && mapping->runtime_descriptor != NULL)
    {
        new_descriptor = mapping->runtime_descriptor;
        *changed = true;
    }
    else
    {
        bool descriptor_changed = false;
        mapped_descriptor = remap_descriptor(ctx, descriptor, &descriptor_changed);
        if (mapped_descriptor == NULL)
        {
            free(name);
            free(descriptor);
            return CLASSFILE_ERR_OUT_OF_MEMORY;
        }

        if (descriptor_changed)
        {
            new_descriptor = mapped_descriptor;
            *changed = true;
        }
    }

    classfile_status status = CLASSFILE_OK;
    if (*changed)
        status = classFile_intern_name_and_type(cf, new_name, new_descriptor, out_index);

    free(mapped_descriptor);
    free(name);
    free(descriptor);
    return status;
}

/* Field-, Method- and InterfaceMethodref: owner, member name and descriptor.
   Runs before the class entries move, so the owner read here is the
   compile-time one the mappings are written against. */
static classfile_status remap_references(ClassFile *cf, const remap_context *ctx)
{
    for (u2 i = 1; i < cf->constant_pool.count; i++)
    {
        const cp_info *entry = constant_pool_at(&cf->constant_pool, i);
        if (entry == NULL)
            continue;

        const bool is_field = entry->tag == JVM_CONSTANT_Fieldref;
        const bool is_method =
            entry->tag == JVM_CONSTANT_Methodref || entry->tag == JVM_CONSTANT_InterfaceMethodref;
        const bool is_dynamic =
            entry->tag == JVM_CONSTANT_Dynamic || entry->tag == JVM_CONSTANT_InvokeDynamic;

        if (!is_field && !is_method && !is_dynamic)
            continue;

        const u2 nat_index = is_dynamic ? entry->u.dynamic.nat_index : entry->u.ref.nat_index;
        const u2 class_index = is_dynamic ? 0 : entry->u.ref.class_index;

        const jni2hook_member_mapping *mapping = NULL;
        if (!is_dynamic)
        {
            const cp_info *nat = constant_pool_at(&cf->constant_pool, nat_index);
            if (nat != NULL && nat->tag == JVM_CONSTANT_NameAndType)
            {
                char *name = utf8_duplicate(&cf->constant_pool, nat->u.nat.name_index);
                char *descriptor = utf8_duplicate(&cf->constant_pool, nat->u.nat.descriptor_index);
                const char *owner = original_class_name(ctx, class_index);

                if (name != NULL && descriptor != NULL)
                {
                    mapping = is_field ? find_member_inherited(cf, ctx, ctx->fields,
                                                               ctx->field_count, owner, name,
                                                               descriptor)
                                       : find_member_inherited(cf, ctx, ctx->methods,
                                                               ctx->method_count, owner, name,
                                                               descriptor);
                }

                free(name);
                free(descriptor);
            }
        }

        u2 new_nat = 0;
        bool changed = false;
        const classfile_status status =
            remap_name_and_type(cf, ctx, nat_index, mapping, &new_nat, &changed);
        if (status != CLASSFILE_OK)
            return status;

        /* Interning may have grown the pool, so the entry is addressed again
           rather than through the pointer taken before the call. */
        if (changed)
        {
            cp_info *target = &cf->constant_pool.entries[i];
            if (is_dynamic)
                target->u.dynamic.nat_index = new_nat;
            else
                target->u.ref.nat_index = new_nat;
        }

        /* runtime_owner moves the reference to another class, which no class
           mapping can express. */
        if (!is_dynamic && mapping != NULL && mapping->runtime_owner != NULL)
        {
            u2 owner_index = 0;
            const classfile_status owner_status =
                classFile_intern_class(cf, mapping->runtime_owner, &owner_index);
            if (owner_status != CLASSFILE_OK)
                return owner_status;

            cf->constant_pool.entries[i].u.ref.class_index = owner_index;
        }
    }

    return CLASSFILE_OK;
}

/* Every class name in the file, which also covers this_class, super_class,
   interfaces, reference owners, catch types and the StackMapTable, because
   each of those is an index to a CONSTANT_Class rather than a name of its own. */
static classfile_status remap_class_entries(ClassFile *cf, const remap_context *ctx)
{
    for (u2 i = 1; i < cf->constant_pool.count; i++)
    {
        const cp_info *entry = constant_pool_at(&cf->constant_pool, i);
        if (entry == NULL || entry->tag != JVM_CONSTANT_Class)
            continue;

        const char *name = original_class_name(ctx, i);
        if (name == NULL)
            continue;

        bool changed = false;
        char *mapped = remap_class_name(ctx, name, &changed);
        if (!changed)
        {
            free(mapped);
            continue;
        }

        if (mapped == NULL)
            return CLASSFILE_ERR_OUT_OF_MEMORY;

        u2 utf8_index = 0;
        const classfile_status status = classFile_intern_utf8(cf, mapped, &utf8_index);
        free(mapped);
        if (status != CLASSFILE_OK)
            return status;

        cf->constant_pool.entries[i].u.class_info.name_index = utf8_index;
    }

    return CLASSFILE_OK;
}

/* The class's own fields and methods. A member is matched against this class,
   its super class and its interfaces, so a method that overrides a renamed one
   is renamed with it instead of quietly becoming a new method. */
static classfile_status remap_own_members(ClassFile *cf, const remap_context *ctx,
                                          member_list *list, bool are_fields)
{
    const char *self = original_class_name(ctx, cf->this_class);

    for (u2 i = 0; i < list->count; i++)
    {
        member_info *member = &list->items[i];

        char *name = utf8_duplicate(&cf->constant_pool, member->name_index);
        char *descriptor = utf8_duplicate(&cf->constant_pool, member->descriptor_index);
        if (name == NULL || descriptor == NULL)
        {
            free(name);
            free(descriptor);
            return CLASSFILE_ERR_OUT_OF_MEMORY;
        }

        /* An override is renamed with the method it overrides, which
           find_member_inherited already expresses by falling back to the
           super class and the interfaces. */
        const jni2hook_member_mapping *mapping =
            are_fields ? find_member_inherited(cf, ctx, ctx->fields, ctx->field_count, self, name,
                                               descriptor)
                       : find_member_inherited(cf, ctx, ctx->methods, ctx->method_count, self, name,
                                               descriptor);

        classfile_status status = CLASSFILE_OK;

        if (mapping != NULL && mapping->runtime_name != NULL)
        {
            u2 index = 0;
            status = classFile_intern_utf8(cf, mapping->runtime_name, &index);
            if (status != CLASSFILE_OK)
            {
                free(name);
                free(descriptor);
                return status;
            }
            list->items[i].name_index = index;
        }

        const char *new_descriptor = NULL;
        char *mapped_descriptor = NULL;

        if (mapping != NULL && mapping->runtime_descriptor != NULL)
        {
            new_descriptor = mapping->runtime_descriptor;
        }
        else
        {
            bool changed = false;
            mapped_descriptor = remap_descriptor(ctx, descriptor, &changed);
            if (mapped_descriptor == NULL)
            {
                free(name);
                free(descriptor);
                return CLASSFILE_ERR_OUT_OF_MEMORY;
            }
            if (changed)
                new_descriptor = mapped_descriptor;
        }

        if (new_descriptor != NULL)
        {
            u2 index = 0;
            status = classFile_intern_utf8(cf, new_descriptor, &index);
            if (status == CLASSFILE_OK)
                list->items[i].descriptor_index = index;
        }

        free(mapped_descriptor);
        free(name);
        free(descriptor);

        if (status != CLASSFILE_OK)
            return status;
    }

    return CLASSFILE_OK;
}

static void remap_context_free(remap_context *ctx)
{
    if (ctx->original_class_names == NULL)
        return;

    for (u2 i = 0; i < ctx->name_count; i++)
        free(ctx->original_class_names[i]);

    free(ctx->original_class_names);
    ctx->original_class_names = NULL;
    ctx->name_count = 0;
}

static jni2hook_status status_of(classfile_status status)
{
    if (status == CLASSFILE_OK)
        return JNI2HOOK_OK;
    if (status == CLASSFILE_ERR_OUT_OF_MEMORY)
        return JNI2HOOK_ERR_OUT_OF_MEMORY;

    return JNI2HOOK_ERR_CLASS_FILE;
}

jni2hook_status
JNI2Hook_RemapClass(const unsigned char *input, size_t input_size,

                    const jni2hook_class_mapping *class_mappings, size_t class_mapping_count,

                    const jni2hook_method_mapping *method_mappings, size_t method_mapping_count,

                    const jni2hook_field_mapping *field_mappings, size_t field_mapping_count,

                    unsigned char **out_bytes, size_t *out_size)
{
    if (input == NULL || input_size == 0 || out_bytes == NULL || out_size == NULL)
        return JNI2HOOK_ERR_CLASS_FILE;

    ClassFile *cf = NULL;
    classfile_status status = classfile_parse(input, input_size, &cf);
    if (status != CLASSFILE_OK)
        return status_of(status);

    remap_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.classes = class_mappings;
    ctx.class_count = class_mappings == NULL ? 0 : class_mapping_count;
    ctx.methods = method_mappings;
    ctx.method_count = method_mappings == NULL ? 0 : method_mapping_count;
    ctx.fields = field_mappings;
    ctx.field_count = field_mappings == NULL ? 0 : field_mapping_count;

    status = snapshot_class_names(cf, &ctx);

    /* References first: they are matched on the compile-time owner, which is
       still readable until remap_class_entries runs. */
    if (status == CLASSFILE_OK)
        status = remap_references(cf, &ctx);
    if (status == CLASSFILE_OK)
        status = remap_own_members(cf, &ctx, &cf->fields, true);
    if (status == CLASSFILE_OK)
        status = remap_own_members(cf, &ctx, &cf->methods, false);
    if (status == CLASSFILE_OK)
        status = remap_class_entries(cf, &ctx);

    remap_context_free(&ctx);

    if (status != CLASSFILE_OK)
    {
        classFile_destroy(cf);
        return status_of(status);
    }

    u1 *bytes = NULL;
    size_t size = 0;
    status = classfile_serialize(cf, &bytes, &size);
    classFile_destroy(cf);

    if (status != CLASSFILE_OK)
    {
        free(bytes);
        return status_of(status);
    }

    *out_bytes = bytes;
    *out_size = size;
    return JNI2HOOK_OK;
}

void JNI2Hook_FreeClassBytes(unsigned char *bytes)
{
    free(bytes);
}
