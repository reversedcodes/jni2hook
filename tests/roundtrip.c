#include "class_file_parser.h"
#include "visitors/code_attribute.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u1 *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long length = ftell(f);
    if (length < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    u1 *data = malloc((size_t)length ? (size_t)length : 1);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    *size = (size_t)length;
    return data;
}

static size_t first_difference(const u1 *a, const u1 *b, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (a[i] != b[i])
            return i;
    }
    return length;
}

/* Parses every Code attribute out of its opaque payload and writes it back,
   which has to reproduce the payload byte for byte. */
static int check_code_attributes(const ClassFile *cf, const char *path, size_t *checked)
{
    int failed = 0;

    for (u2 i = 0; i < cf->methods.count; i++) {
        const member_info *method = &cf->methods.items[i];

        for (u2 j = 0; j < method->attributes.count; j++) {
            const attribute_info *attribute = &method->attributes.items[j];
            if (!constant_pool_utf8_equals(&cf->constant_pool,
                                           attribute->attribute_name_index, "Code"))
                continue;

            code_attribute code;
            classfile_status status = code_attribute_parse(attribute, &code);
            if (status != CLASSFILE_OK) {
                fprintf(stderr, "FAIL %s: Code parse in method %u: %s\n",
                        path, i, classfile_status_message(status));
                failed = 1;
                continue;
            }

            attribute_info rebuilt;
            memset(&rebuilt, 0, sizeof(rebuilt));
            rebuilt.attribute_name_index = attribute->attribute_name_index;

            status = code_attribute_write(&code, &rebuilt);
            if (status != CLASSFILE_OK) {
                fprintf(stderr, "FAIL %s: Code write in method %u: %s\n",
                        path, i, classfile_status_message(status));
                failed = 1;
            } else if (rebuilt.attribute_length != attribute->attribute_length) {
                fprintf(stderr, "FAIL %s: Code length in method %u: %u -> %u\n",
                        path, i, attribute->attribute_length, rebuilt.attribute_length);
                failed = 1;
            } else {
                const size_t at = first_difference(attribute->info, rebuilt.info,
                                                   attribute->attribute_length);
                if (at != attribute->attribute_length) {
                    fprintf(stderr, "FAIL %s: Code byte %zu in method %u\n", path, at, i);
                    failed = 1;
                }
            }

            (*checked)++;
            attribute_info_free(&rebuilt);
            code_attribute_free(&code);
        }
    }

    return failed;
}

static int check(const char *path, bool verbose, size_t *code_attributes)
{
    size_t size = 0;
    u1 *data = read_file(path, &size);
    if (data == NULL) {
        fprintf(stderr, "FAIL %s: cannot read\n", path);
        return 1;
    }

    ClassFile *cf = NULL;
    classfile_status status = classfile_parse(data, size, &cf);
    if (status != CLASSFILE_OK) {
        fprintf(stderr, "FAIL %s: parse: %s\n", path, classfile_status_message(status));
        free(data);
        return 1;
    }

    u1 *rebuilt = NULL;
    size_t rebuilt_size = 0;
    status = classfile_serialize(cf, &rebuilt, &rebuilt_size);
    if (status != CLASSFILE_OK) {
        fprintf(stderr, "FAIL %s: serialize: %s\n", path, classfile_status_message(status));
        classFile_destroy(cf);
        free(data);
        return 1;
    }

    int failed = 0;
    if (rebuilt_size != size) {
        fprintf(stderr, "FAIL %s: size %zu -> %zu\n", path, size, rebuilt_size);
        failed = 1;
    } else {
        const size_t at = first_difference(data, rebuilt, size);
        if (at != size) {
            fprintf(stderr, "FAIL %s: byte %zu: %02X != %02X\n", path, at, data[at], rebuilt[at]);
            failed = 1;
        }
    }

    failed |= check_code_attributes(cf, path, code_attributes);

    if (!failed && verbose) {
        printf("ok   %s  major=%u minor=%u cp=%u fields=%u methods=%u attrs=%u %zu bytes\n",
               path, cf->major_version, cf->minor_version, cf->constant_pool.count,
               cf->fields.count, cf->methods.count, cf->attributes.count, size);
    }

    free(rebuilt);
    classFile_destroy(cf);
    free(data);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s [-v] <file.class>...\n", argv[0]);
        return 2;
    }

    bool verbose = false;
    int first = 1;
    if (strcmp(argv[1], "-v") == 0) {
        verbose = true;
        first = 2;
    }

    int failures = 0;
    int total = 0;
    size_t code_attributes = 0;
    for (int i = first; i < argc; i++) {
        total++;
        failures += check(argv[i], verbose, &code_attributes);
    }

    printf("%d/%d roundtrips byte-identical, %zu Code attributes\n",
           total - failures, total, code_attributes);
    return failures == 0 ? 0 : 1;
}
