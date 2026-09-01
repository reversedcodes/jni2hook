/* The remapper without a VM. Reads a compiled class, rewrites it, and checks
   the result by parsing it back and reading the constant pool, so a claim like
   "the super class moved" is answered from the class file rather than from the
   remapper's own bookkeeping.

   usage: j2h_class_remap <RemapPlugin.class> */

#include "jni2hook/jni2hook.h"

#include "jni2hook/utils/class_file_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(const char *what, int condition)
{
    printf("  %-58s %s\n", what, condition ? "ok" : "FAIL");
    if (!condition)
        failures++;
}

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

/* The name a CONSTANT_Class entry carries, as a terminated copy. */
static char *class_name_at(const ClassFile *cf, u2 index)
{
    const cp_info *entry = constant_pool_at(&cf->constant_pool, index);
    if (entry == NULL || entry->tag != JVM_CONSTANT_Class)
        return NULL;

    const u1 *bytes = NULL;
    u2 length = 0;
    if (!constant_pool_utf8(&cf->constant_pool, entry->u.class_info.name_index, &bytes, &length))
        return NULL;

    char *copy = malloc((size_t)length + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, bytes, length);
    copy[length] = 0;
    return copy;
}

/* Whether any CONSTANT_Class still resolves to this name. Orphaned Utf8 text
   is not interesting: remapping repoints indices and leaves the old entry
   behind, so what matters is that nothing references it any more. */
static bool has_class_entry(const ClassFile *cf, const char *name)
{
    for (u2 i = 1; i < cf->constant_pool.count; i++)
    {
        char *entry = class_name_at(cf, i);
        if (entry == NULL)
            continue;

        const bool match = strcmp(entry, name) == 0;
        free(entry);
        if (match)
            return true;
    }
    return false;
}

/* Whether any Methodref names owner.name with descriptor. */
static bool has_methodref(const ClassFile *cf, const char *owner, const char *name,
                          const char *descriptor)
{
    for (u2 i = 1; i < cf->constant_pool.count; i++)
    {
        const cp_info *entry = constant_pool_at(&cf->constant_pool, i);
        if (entry == NULL || entry->tag != JVM_CONSTANT_Methodref)
            continue;

        char *ref_owner = class_name_at(cf, entry->u.ref.class_index);
        if (ref_owner == NULL)
            continue;

        const cp_info *nat = constant_pool_at(&cf->constant_pool, entry->u.ref.nat_index);
        const bool match =
            strcmp(ref_owner, owner) == 0 && nat != NULL && nat->tag == JVM_CONSTANT_NameAndType &&
            constant_pool_utf8_equals(&cf->constant_pool, nat->u.nat.name_index, name) &&
            constant_pool_utf8_equals(&cf->constant_pool, nat->u.nat.descriptor_index, descriptor);

        free(ref_owner);
        if (match)
            return true;
    }

    return false;
}

static bool has_method_descriptor(const ClassFile *cf, const char *name, const char *descriptor)
{
    for (u2 i = 0; i < cf->methods.count; i++)
    {
        const member_info *method = &cf->methods.items[i];
        if (constant_pool_utf8_equals(&cf->constant_pool, method->name_index, name) &&
            constant_pool_utf8_equals(&cf->constant_pool, method->descriptor_index, descriptor))
            return true;
    }

    return false;
}

/* An empty mapping must be an exact no-op on any class file, however exotic.
   That is the same bar the corpus roundtrip holds the parser and writer to, and
   it is what catches an interning bug that only shows up on an attribute this
   test's own fixture never contains. */
static int roundtrip_only(char **paths, int count)
{
    int checked = 0;

    for (int i = 0; i < count; i++)
    {
        size_t size = 0;
        u1 *input = read_file(paths[i], &size);
        if (input == NULL)
            continue;

        unsigned char *output = NULL;
        size_t output_size = 0;
        const jni2hook_status status =
            JNI2Hook_RemapClass(input, size, NULL, 0, NULL, 0, NULL, 0, &output, &output_size);

        if (status != JNI2HOOK_OK || output_size != size || memcmp(output, input, size) != 0)
        {
            printf("  %s did not survive an empty remap\n", paths[i]);
            failures++;
        }

        JNI2Hook_FreeClassBytes(output);
        free(input);
        checked++;
    }

    printf("%d files, %d failures\n", checked, failures);
    return failures == 0 ? 0 : 1;
}

/* The mutating path over arbitrary input. Renaming a type nearly every class
   mentions forces an intern and an index rewrite in each of them, which is
   where a stale cp_info pointer across a pool reallocation would show up. The
   result only has to parse again: what it should say is the other test's job. */
static int stress(char **paths, int count)
{
    const jni2hook_class_mapping classes[] = {
        {"java/lang/Object", "java/lang/Objecz"},
        {"java/lang/String", "java/lang/Strinz"},
    };

    int checked = 0;

    for (int i = 0; i < count; i++)
    {
        size_t size = 0;
        u1 *input = read_file(paths[i], &size);
        if (input == NULL)
            continue;

        unsigned char *output = NULL;
        size_t output_size = 0;
        const jni2hook_status status =
            JNI2Hook_RemapClass(input, size, classes, sizeof(classes) / sizeof(classes[0]), NULL, 0,
                                NULL, 0, &output, &output_size);

        if (status != JNI2HOOK_OK)
        {
            printf("  %s: %s\n", paths[i], JNI2Hook_StatusMessage(status));
            failures++;
        }
        else
        {
            ClassFile *cf = NULL;
            if (classfile_parse(output, output_size, &cf) != CLASSFILE_OK)
            {
                printf("  %s did not parse after remapping\n", paths[i]);
                failures++;
            }
            classFile_destroy(cf);
        }

        JNI2Hook_FreeClassBytes(output);
        free(input);
        checked++;
    }

    printf("%d files, %d failures\n", checked, failures);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--roundtrip") == 0)
        return roundtrip_only(argv + 2, argc - 2);

    if (argc >= 3 && strcmp(argv[1], "--stress") == 0)
        return stress(argv + 2, argc - 2);

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <RemapPlugin.class>\n"
                        "       %s --roundtrip <file.class>...\n",
                argv[0], argv[0]);
        return 2;
    }

    size_t size = 0;
    u1 *input = read_file(argv[1], &size);
    if (input == NULL)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    const jni2hook_class_mapping classes[] = {
        {"RemapApi", "ra"},
        {"RemapValue", "rv"},
    };

    const jni2hook_method_mapping methods[] = {
        {"RemapApi", "greet", "(LRemapValue;)Ljava/lang/String;", NULL, "b", NULL},
        {"RemapValue", "text", "()Ljava/lang/String;", NULL, "d", NULL},
    };

    unsigned char *output = NULL;
    size_t output_size = 0;
    const jni2hook_status status =
        JNI2Hook_RemapClass(input, size, classes, sizeof(classes) / sizeof(classes[0]), methods,
                            sizeof(methods) / sizeof(methods[0]), NULL, 0, &output, &output_size);

    printf("== remapping ==\n");
    check("JNI2Hook_RemapClass", status == JNI2HOOK_OK);
    if (status != JNI2HOOK_OK)
    {
        fprintf(stderr, "remap: %s\n", JNI2Hook_StatusMessage(status));
        free(input);
        return 1;
    }

    ClassFile *cf = NULL;
    const classfile_status parsed = classfile_parse(output, output_size, &cf);
    check("the result parses", parsed == CLASSFILE_OK);
    if (parsed != CLASSFILE_OK)
    {
        JNI2Hook_FreeClassBytes(output);
        free(input);
        return 1;
    }

    printf("== inheritance ==\n");
    char *super_name = class_name_at(cf, cf->super_class);
    check("super class is ra", super_name != NULL && strcmp(super_name, "ra") == 0);
    free(super_name);

    char *this_name = class_name_at(cf, cf->this_class);
    check("the class keeps its own name", this_name != NULL && strcmp(this_name, "RemapPlugin") == 0);
    free(this_name);

    printf("== references ==\n");
    /* The owner of the inherited call stays RemapPlugin, because that is the
       static type javac wrote and method resolution walks up to ra from there.
       Only the name and the descriptor move. */
    check("the inherited call became b(Lrv;)",
          has_methodref(cf, "RemapPlugin", "b", "(Lrv;)Ljava/lang/String;"));
    check("the value call became rv.d", has_methodref(cf, "rv", "d", "()Ljava/lang/String;"));

    printf("== descriptors ==\n");
    check("run takes an rv", has_method_descriptor(cf, "run", "(Lrv;)Ljava/lang/String;"));

    printf("== no reference to a compile-time name is left ==\n");
    check("no class entry for RemapApi", !has_class_entry(cf, "RemapApi"));
    check("no class entry for RemapValue", !has_class_entry(cf, "RemapValue"));
    check("the inherited call no longer names the plugin itself",
          !has_methodref(cf, "RemapPlugin", "greet", "(Lrv;)Ljava/lang/String;"));
    check("no call keeps the compile-time descriptor",
          !has_methodref(cf, "ra", "b", "(LRemapValue;)Ljava/lang/String;"));

    printf("== unmapped input is left alone ==\n");
    unsigned char *untouched = NULL;
    size_t untouched_size = 0;
    const jni2hook_status idle =
        JNI2Hook_RemapClass(input, size, NULL, 0, NULL, 0, NULL, 0, &untouched, &untouched_size);
    check("an empty mapping round trips byte for byte",
          idle == JNI2HOOK_OK && untouched_size == size && memcmp(untouched, input, size) == 0);
    JNI2Hook_FreeClassBytes(untouched);

    classFile_destroy(cf);
    JNI2Hook_FreeClassBytes(output);
    free(input);

    printf("%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
