/* Uses nothing but the public utils header, so it fails to compile the moment
   that header stops being self sufficient. */

#include <jni2hook/utils/class_file.h>

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
    classfile_status status = classfile_parse(data, size, &cf);
    if (status != CLASSFILE_OK)
    {
        fprintf(stderr, "parse: %s\n", classfile_status_message(status));
        free(data);
        return 1;
    }

    printf("major=%u minor=%u constants=%u fields=%u methods=%u\n",
           cf->major_version, cf->minor_version, cf->constant_pool.count,
           cf->fields.count, cf->methods.count);

    int failures = 0;
    size_t instructions = 0, frames = 0;

    for (u2 i = 0; i < cf->methods.count; i++)
    {
        const member_info *method = &cf->methods.items[i];
        const u1 *name = NULL, *descriptor = NULL;
        u2 name_length = 0, descriptor_length = 0;

        constant_pool_utf8(&cf->constant_pool, method->name_index, &name, &name_length);
        constant_pool_utf8(&cf->constant_pool, method->descriptor_index,
                           &descriptor, &descriptor_length);

        attribute_info *code = attribute_list_find(&method->attributes,
                                                   &cf->constant_pool, "Code");
        printf("  %.*s%.*s%s\n", name_length, name, descriptor_length, descriptor,
               code == NULL ? "  (no body)" : "");
        if (code == NULL)
            continue;

        code_editor editor;
        if (code_editor_load(code, &cf->constant_pool, &editor) != CLASSFILE_OK)
        {
            printf("    editor load failed\n");
            failures++;
            continue;
        }

        instructions += editor.instructions.count;
        frames       += editor.stack_map.count;

        attribute_info back;
        memset(&back, 0, sizeof(back));
        back.attribute_name_index = code->attribute_name_index;
        if (code_editor_store(&editor, &back) != CLASSFILE_OK ||
            back.attribute_length != code->attribute_length ||
            memcmp(back.info, code->info, code->attribute_length) != 0)
        {
            printf("    Code did not survive a load/store round trip\n");
            failures++;
        }

        attribute_info_free(&back);
        code_editor_free(&editor);
    }

    u1 *rebuilt = NULL;
    size_t rebuilt_size = 0;
    status = classfile_serialize(cf, &rebuilt, &rebuilt_size);
    if (status != CLASSFILE_OK || rebuilt_size != size ||
        memcmp(rebuilt, data, size) != 0)
    {
        printf("class file did not survive a round trip\n");
        failures++;
    }

    printf("%zu instructions, %zu stack map frames, %d failures\n",
           instructions, frames, failures);

    free(rebuilt);
    classFile_destroy(cf);
    free(data);
    return failures == 0 ? 0 : 1;
}
