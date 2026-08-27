/* Reads a class file and prints what is in it.
 *
 *     j2h_example_dump <file.class>
 *
 * Uses only the class file half of jni2hook, so it needs no JVM and no JDK. The
 * point it demonstrates: attributes stay opaque unless something asks for them.
 * Code and StackMapTable are parsed here because this example wants their
 * contents; everything else is listed by name and length and passes through
 * untouched, which is why the parser copes with class file versions it has
 * never seen. */

#include <jni2hook/utils/class_file.h>

#include <stdio.h>
#include <stdlib.h>

static u1 *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    const long length = ftell(f);
    if (length < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);

    u1 *data = malloc((size_t)length ? (size_t)length : 1);
    if (data != NULL && fread(data, 1, (size_t)length, f) != (size_t)length)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data != NULL)
        *size = (size_t)length;
    return data;
}

static void print_utf8(const constant_pool *pool, u2 index)
{
    const u1 *bytes = NULL;
    u2 length = 0;
    if (constant_pool_utf8(pool, index, &bytes, &length))
        printf("%.*s", (int)length, bytes);
    else
        printf("#%u", index);
}

static const char *tag_name(u1 tag)
{
    switch (tag)
    {
    case JVM_CONSTANT_Utf8:
        return "Utf8";
    case JVM_CONSTANT_Integer:
        return "Integer";
    case JVM_CONSTANT_Float:
        return "Float";
    case JVM_CONSTANT_Long:
        return "Long";
    case JVM_CONSTANT_Double:
        return "Double";
    case JVM_CONSTANT_Class:
        return "Class";
    case JVM_CONSTANT_String:
        return "String";
    case JVM_CONSTANT_Fieldref:
        return "Fieldref";
    case JVM_CONSTANT_Methodref:
        return "Methodref";
    case JVM_CONSTANT_InterfaceMethodref:
        return "InterfaceMethodref";
    case JVM_CONSTANT_NameAndType:
        return "NameAndType";
    case JVM_CONSTANT_MethodHandle:
        return "MethodHandle";
    case JVM_CONSTANT_MethodType:
        return "MethodType";
    case JVM_CONSTANT_Dynamic:
        return "Dynamic";
    case JVM_CONSTANT_InvokeDynamic:
        return "InvokeDynamic";
    case JVM_CONSTANT_Module:
        return "Module";
    case JVM_CONSTANT_Package:
        return "Package";
    default:
        return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <file.class>\n", argv[0]);
        return 2;
    }

    size_t size = 0;
    u1 *data = read_file(argv[1], &size);
    if (data == NULL)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    ClassFile *cf = NULL;
    const classfile_status status = classfile_parse(data, size, &cf);
    if (status != CLASSFILE_OK)
    {
        fprintf(stderr, "%s: %s\n", argv[1], classfile_status_message(status));
        free(data);
        return 1;
    }

    printf("class ");
    const cp_info *this_class = constant_pool_at(&cf->constant_pool, cf->this_class);
    if (this_class != NULL)
        print_utf8(&cf->constant_pool, this_class->u.class_info.name_index);
    printf("\n  version   %u.%u (Java %d)\n", cf->major_version, cf->minor_version,
           JDK_VERSION_OF(cf->major_version));
    printf("  constants %u, fields %u, methods %u, attributes %u\n\n", cf->constant_pool.count,
           cf->fields.count, cf->methods.count, cf->attributes.count);

    size_t tags[32] = {0};
    for (u2 i = 1; i < cf->constant_pool.count; i++)
    {
        const cp_info *entry = constant_pool_at(&cf->constant_pool, i);
        if (entry != NULL && entry->tag < 32)
            tags[entry->tag]++;
    }
    printf("  constant pool\n");
    for (u1 t = 1; t < 32; t++)
        if (tags[t] != 0)
            printf("    %-20s %zu\n", tag_name(t), tags[t]);

    printf("\n  methods\n");
    for (u2 i = 0; i < cf->methods.count; i++)
    {
        const member_info *method = &cf->methods.items[i];
        printf("    ");
        print_utf8(&cf->constant_pool, method->name_index);
        print_utf8(&cf->constant_pool, method->descriptor_index);
        if ((method->access_flags & JVM_ACC_STATIC) != 0)
            printf("  static");
        if ((method->access_flags & JVM_ACC_NATIVE) != 0)
            printf("  native");
        if ((method->access_flags & JVM_ACC_ABSTRACT) != 0)
            printf("  abstract");

        attribute_info *code = attribute_list_find(&method->attributes, &cf->constant_pool, "Code");
        if (code == NULL)
        {
            printf("\n");
            continue;
        }

        code_editor editor;
        if (code_editor_load(code, &cf->constant_pool, &editor) != CLASSFILE_OK)
        {
            printf("   (unreadable Code)\n");
            continue;
        }

        printf("\n      %u instructions, max_stack %u, max_locals %u, %u handlers, %u frames\n",
               editor.instructions.count, editor.max_stack, editor.max_locals,
               editor.exceptions.count, editor.stack_map.count);

        for (u2 k = 0; k < editor.other_attributes.count; k++)
        {
            printf("      attribute ");
            print_utf8(&cf->constant_pool, editor.other_attributes.items[k].attribute_name_index);
            printf(" (%u bytes, kept verbatim)\n",
                   editor.other_attributes.items[k].attribute_length);
        }

        code_editor_free(&editor);
    }

    classFile_destroy(cf);
    free(data);
    return 0;
}
