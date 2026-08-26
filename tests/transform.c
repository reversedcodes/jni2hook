#include "class_file_parser.h"
#include "class_transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u1 *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    const long length = ftell(f);
    if (length < 0) { fclose(f); return NULL; }
    rewind(f);

    u1 *data = malloc((size_t)length ? (size_t)length : 1);
    if (data == NULL) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)length, f) != (size_t)length) { free(data); fclose(f); return NULL; }
    fclose(f);

    *size = (size_t)length;
    return data;
}

static int write_file(const char *path, const u1 *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return 1;
    const int failed = fwrite(data, 1, size, f) != size;
    fclose(f);
    return failed;
}

static int hook_every_method(ClassFile *cf, const char *suffix, bool quiet)
{
    const u2 original_count = cf->methods.count;

    char (*names)[256] = calloc(original_count, sizeof(*names));
    char (*descriptors)[256] = calloc(original_count, sizeof(*descriptors));
    if (names == NULL || descriptors == NULL)
    {
        free(names);
        free(descriptors);
        return -1;
    }

    u2 candidates = 0;
    for (u2 i = 0; i < original_count; i++)
    {
        const member_info *method = &cf->methods.items[i];
        const u1 *bytes;
        u2 length;

        if (!constant_pool_utf8(&cf->constant_pool, method->name_index, &bytes, &length))
            continue;
        if (length >= sizeof(names[0]))
            continue;
        memcpy(names[candidates], bytes, length);
        names[candidates][length] = 0;

        if (!constant_pool_utf8(&cf->constant_pool, method->descriptor_index, &bytes, &length))
            continue;
        if (length >= sizeof(descriptors[0]))
            continue;
        memcpy(descriptors[candidates], bytes, length);
        descriptors[candidates][length] = 0;

        candidates++;
    }

    int hooked = 0;
    for (u2 i = 0; i < candidates; i++)
    {
        char copy_name[320];
        snprintf(copy_name, sizeof(copy_name), "%s%s", names[i], suffix);

        classfile_status cause = CLASSFILE_OK;
        const transform_status result =
            class_transform_make_native(cf, names[i], descriptors[i], copy_name, &cause);

        if (result == TRANSFORM_OK)
            hooked++;
        else if (result == TRANSFORM_ERR_CLASSFILE)
        {
            if (!quiet)
                fprintf(stderr, "  %s%s: %s\n", names[i], descriptors[i],
                        classfile_status_message(cause));
            free(names);
            free(descriptors);
            return -1;
        }
    }

    free(names);
    free(descriptors);
    return hooked;
}

static int insert_into_every_method(ClassFile *cf)
{
    const u2 original_count = cf->methods.count;

    char (*names)[256] = calloc(original_count, sizeof(*names));
    char (*descriptors)[512] = calloc(original_count, sizeof(*descriptors));
    if (names == NULL || descriptors == NULL)
    {
        free(names);
        free(descriptors);
        return -1;
    }

    u2 candidates = 0;
    for (u2 i = 0; i < original_count; i++)
    {
        const member_info *method = &cf->methods.items[i];
        const u1 *bytes;
        u2 length;

        if (!constant_pool_utf8(&cf->constant_pool, method->name_index, &bytes, &length) ||
            length >= sizeof(names[0]))
            continue;
        memcpy(names[candidates], bytes, length);
        names[candidates][length] = 0;

        if (!constant_pool_utf8(&cf->constant_pool, method->descriptor_index, &bytes, &length) ||
            length >= sizeof(descriptors[0]))
            continue;
        memcpy(descriptors[candidates], bytes, length);
        descriptors[candidates][length] = 0;

        candidates++;
    }

    int inserted = 0;
    for (u2 i = 0; i < candidates; i++)
    {
        char hook_name[320];
        snprintf(hook_name, sizeof(hook_name), "hook$%u", (unsigned)i);

        classfile_status cause = CLASSFILE_OK;
        const transform_status result =
            class_transform_insert_call(cf, names[i], descriptors[i], 0, hook_name, &cause);

        if (result == TRANSFORM_OK)
            inserted++;
        else if (result == TRANSFORM_ERR_CLASSFILE)
        {
            free(names);
            free(descriptors);
            return -1;
        }
    }

    free(names);
    free(descriptors);
    return inserted;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[2], "--insert-all") == 0)
    {
        size_t size = 0;
        u1 *data = read_file(argv[1], &size);
        if (data == NULL)
            return 1;

        ClassFile *cf = NULL;
        classfile_status status = classfile_parse(data, size, &cf);
        free(data);
        if (status != CLASSFILE_OK)
            return 1;

        const int inserted = insert_into_every_method(cf);
        if (inserted < 0)
        {
            classFile_destroy(cf);
            return 1;
        }

        u1 *out = NULL;
        size_t out_size = 0;
        status = classfile_serialize(cf, &out, &out_size);
        classFile_destroy(cf);
        if (status != CLASSFILE_OK)
            return 1;

        const int failed = write_file(argv[4], out, out_size);
        free(out);
        if (failed)
            return 1;

        printf("%d %s\n", inserted, argv[4]);
        return 0;
    }

    if (argc == 5 && strcmp(argv[2], "--all") == 0)
    {
        size_t size = 0;
        u1 *data = read_file(argv[1], &size);
        if (data == NULL)
        {
            fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }

        ClassFile *cf = NULL;
        classfile_status status = classfile_parse(data, size, &cf);
        free(data);
        if (status != CLASSFILE_OK)
        {
            fprintf(stderr, "parse %s: %s\n", argv[1], classfile_status_message(status));
            return 1;
        }

        const int hooked = hook_every_method(cf, argv[3], true);
        if (hooked < 0)
        {
            classFile_destroy(cf);
            return 1;
        }

        u1 *out = NULL;
        size_t out_size = 0;
        status = classfile_serialize(cf, &out, &out_size);
        classFile_destroy(cf);
        if (status != CLASSFILE_OK)
        {
            fprintf(stderr, "serialize %s: %s\n", argv[1], classfile_status_message(status));
            return 1;
        }

        const int failed = write_file(argv[4], out, out_size);
        free(out);
        if (failed)
        {
            fprintf(stderr, "cannot write %s\n", argv[4]);
            return 1;
        }

        printf("%d %s\n", hooked, argv[4]);
        return 0;
    }

    if (argc == 6 && strcmp(argv[2], "--insert") == 0)
    {
        size_t size = 0;
        u1 *data = read_file(argv[1], &size);
        if (data == NULL)
        {
            fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }

        ClassFile *cf = NULL;
        classfile_status status = classfile_parse(data, size, &cf);
        free(data);
        if (status != CLASSFILE_OK)
        {
            fprintf(stderr, "parse: %s\n", classfile_status_message(status));
            return 1;
        }

        char method[256], descriptor[512];
        unsigned offset = 0;
        if (sscanf(argv[3], "%255[^:]:%511[^@]@%u", method, descriptor, &offset) != 3)
        {
            fprintf(stderr, "expected name:descriptor@offset, got %s\n", argv[3]);
            classFile_destroy(cf);
            return 2;
        }

        classfile_status cause = CLASSFILE_OK;
        const transform_status result =
            class_transform_insert_call(cf, method, descriptor, offset, argv[4], &cause);
        if (result != TRANSFORM_OK)
        {
            fprintf(stderr, "insert: %s", transform_status_message(result));
            if (result == TRANSFORM_ERR_CLASSFILE)
                fprintf(stderr, " (%s)", classfile_status_message(cause));
            fprintf(stderr, "\n");
            classFile_destroy(cf);
            return 1;
        }

        u1 *out = NULL;
        size_t out_size = 0;
        status = classfile_serialize(cf, &out, &out_size);
        classFile_destroy(cf);
        if (status != CLASSFILE_OK)
        {
            fprintf(stderr, "serialize: %s\n", classfile_status_message(status));
            return 1;
        }

        const int failed = write_file(argv[5], out, out_size);
        free(out);
        if (failed)
        {
            fprintf(stderr, "cannot write %s\n", argv[5]);
            return 1;
        }

        printf("inserted %s at %s offset %u -> %s (%zu bytes)\n",
               argv[4], method, offset, argv[5], out_size);
        return 0;
    }

    if (argc != 6 && argc != 7)
    {
        fprintf(stderr,
                "usage: %s <in.class> <method> <descriptor> <copy-name> <out.class> [--restore]\n"
                "       %s <in.class> --all <copy-suffix> <out.class>\n"
                "       %s <in.class> --insert <name:descriptor@offset> <hook-name> <out.class>\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }

    const bool restore = argc == 7 && strcmp(argv[6], "--restore") == 0;

    size_t size = 0;
    u1 *data = read_file(argv[1], &size);
    if (data == NULL)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    ClassFile *cf = NULL;
    classfile_status status = classfile_parse(data, size, &cf);
    free(data);
    if (status != CLASSFILE_OK)
    {
        fprintf(stderr, "parse: %s\n", classfile_status_message(status));
        return 1;
    }

    classfile_status cause = CLASSFILE_OK;
    const transform_status result =
        restore ? class_transform_restore(cf, argv[2], argv[3], argv[4], &cause)
                : class_transform_make_native(cf, argv[2], argv[3], argv[4], &cause);
    if (result != TRANSFORM_OK)
    {
        fprintf(stderr, "transform: %s", transform_status_message(result));
        if (result == TRANSFORM_ERR_CLASSFILE)
            fprintf(stderr, " (%s)", classfile_status_message(cause));
        fprintf(stderr, "\n");
        classFile_destroy(cf);
        return 1;
    }

    u1 *out = NULL;
    size_t out_size = 0;
    status = classfile_serialize(cf, &out, &out_size);
    classFile_destroy(cf);
    if (status != CLASSFILE_OK)
    {
        fprintf(stderr, "serialize: %s\n", classfile_status_message(status));
        return 1;
    }

    const int failed = write_file(argv[5], out, out_size);
    free(out);
    if (failed)
    {
        fprintf(stderr, "cannot write %s\n", argv[5]);
        return 1;
    }

    printf("%s %s%s -> %s (%zu bytes)\n",
           restore ? "restored" : "hooked", argv[2], argv[3], argv[5], out_size);
    return 0;
}
