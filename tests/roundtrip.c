#include "jni2hook/utils/class_file_parser.h"
#include "jni2hook/utils/visitors/code_attribute.h"
#include "jni2hook/utils/visitors/instruction.h"
#include "jni2hook/utils/visitors/stack_map_table.h"
#include "jni2hook/utils/visitors/code_editor.h"

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

static size_t g_seen_frames = 0;
static size_t g_seen_uninitialized = 0;
static size_t g_seen_chop = 0;
static size_t g_seen_append = 0;
static size_t g_seen_full = 0;
static size_t g_seen_wide = 0;
static size_t g_seen_table_switch = 0;
static size_t g_seen_lookup_switch = 0;
static size_t g_seen_ldc = 0;
static size_t g_seen_ldc_w = 0;
static size_t g_seen_branch_wide = 0;
static size_t g_seen_invoke_dynamic = 0;
static size_t g_seen_invoke_interface = 0;
static size_t g_seen_multianewarray = 0;

static void tally(const instruction_list *list)
{
    for (u4 i = 0; i < list->count; i++)
    {
        const instruction *node = &list->items[i];
        if (node->wide) g_seen_wide++;
        switch (node->kind)
        {
        case OPERAND_TABLE_SWITCH:     g_seen_table_switch++;     break;
        case OPERAND_LOOKUP_SWITCH:    g_seen_lookup_switch++;    break;
        case OPERAND_BRANCH_WIDE:      g_seen_branch_wide++;      break;
        case OPERAND_INVOKE_DYNAMIC:   g_seen_invoke_dynamic++;   break;
        case OPERAND_INVOKE_INTERFACE: g_seen_invoke_interface++; break;
        case OPERAND_MULTIANEWARRAY:   g_seen_multianewarray++;   break;
        case OPERAND_CONSTANT:
            if (node->opcode == JVM_OPC_ldc) g_seen_ldc++; else g_seen_ldc_w++;
            break;
        default: break;
        }
    }
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
static int check_code_attributes(const ClassFile *cf, const char *path, size_t *checked,
                                 size_t *instruction_count)
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

            instruction_list instructions;
            status = instruction_list_parse(code.code, code.code_length, &instructions);
            if (status != CLASSFILE_OK)
            {
                fprintf(stderr, "FAIL %s: bytecode parse in method %u: %s\n",
                        path, i, classfile_status_message(status));
                failed = 1;
            }
            else
            {
                u1 *encoded = NULL;
                u4  encoded_length = 0;
                status = instruction_list_encode(&instructions, &encoded, &encoded_length);
                if (status != CLASSFILE_OK)
                {
                    fprintf(stderr, "FAIL %s: bytecode encode in method %u: %s\n",
                            path, i, classfile_status_message(status));
                    failed = 1;
                }
                else if (encoded_length != code.code_length)
                {
                    fprintf(stderr, "FAIL %s: bytecode length in method %u: %u -> %u\n",
                            path, i, code.code_length, encoded_length);
                    failed = 1;
                }
                else
                {
                    const size_t at = first_difference(code.code, encoded, code.code_length);
                    if (at != code.code_length)
                    {
                        fprintf(stderr, "FAIL %s: bytecode byte %zu in method %u (opcode %02X)\n",
                                path, at, i, code.code[at]);
                        failed = 1;
                    }
                }
                tally(&instructions);
                *instruction_count += instructions.count;
                free(encoded);
                instruction_list_free(&instructions);
            }

            for (u2 k = 0; k < code.attributes.count; k++)
            {
                const attribute_info *nested = &code.attributes.items[k];
                if (!constant_pool_utf8_equals(&cf->constant_pool,
                                               nested->attribute_name_index, "StackMapTable"))
                    continue;

                stack_map_table table;
                status = stack_map_table_parse(nested, &table);
                if (status != CLASSFILE_OK)
                {
                    fprintf(stderr, "FAIL %s: StackMapTable parse in method %u: %s\n",
                            path, i, classfile_status_message(status));
                    failed = 1;
                    continue;
                }

                for (u2 f = 0; f < table.count; f++)
                {
                    g_seen_frames++;
                    if (table.frames[f].kind == FRAME_CHOP)   g_seen_chop++;
                    if (table.frames[f].kind == FRAME_APPEND) g_seen_append++;
                    if (table.frames[f].kind == FRAME_FULL)   g_seen_full++;
                    for (u2 t = 0; t < table.frames[f].locals_count; t++)
                        if (table.frames[f].locals[t].tag == JVM_ITEM_Uninitialized)
                            g_seen_uninitialized++;
                    for (u2 t = 0; t < table.frames[f].stack_count; t++)
                        if (table.frames[f].stack[t].tag == JVM_ITEM_Uninitialized)
                            g_seen_uninitialized++;
                }

                attribute_info smt;
                memset(&smt, 0, sizeof(smt));
                smt.attribute_name_index = nested->attribute_name_index;
                status = stack_map_table_write(&table, &smt);
                if (status != CLASSFILE_OK)
                {
                    fprintf(stderr, "FAIL %s: StackMapTable write in method %u: %s\n",
                            path, i, classfile_status_message(status));
                    failed = 1;
                }
                else if (smt.attribute_length != nested->attribute_length)
                {
                    fprintf(stderr, "FAIL %s: StackMapTable length in method %u: %u -> %u\n",
                            path, i, nested->attribute_length, smt.attribute_length);
                    failed = 1;
                }
                else
                {
                    const size_t at = first_difference(nested->info, smt.info,
                                                       nested->attribute_length);
                    if (at != nested->attribute_length)
                    {
                        fprintf(stderr, "FAIL %s: StackMapTable byte %zu in method %u\n",
                                path, at, i);
                        failed = 1;
                    }
                }

                attribute_info_free(&smt);
                stack_map_table_free(&table);
            }

            {
                code_editor editor;
                status = code_editor_load(attribute, &cf->constant_pool, &editor);
                if (status != CLASSFILE_OK)
                {
                    fprintf(stderr, "FAIL %s: editor load in method %u: %s\n",
                            path, i, classfile_status_message(status));
                    failed = 1;
                }
                else
                {
                    attribute_info back;
                    memset(&back, 0, sizeof(back));
                    back.attribute_name_index = attribute->attribute_name_index;
                    status = code_editor_store(&editor, &back);
                    if (status != CLASSFILE_OK)
                    {
                        fprintf(stderr, "FAIL %s: editor store in method %u: %s\n",
                                path, i, classfile_status_message(status));
                        failed = 1;
                    }
                    else if (back.attribute_length != attribute->attribute_length)
                    {
                        fprintf(stderr, "FAIL %s: editor length in method %u: %u -> %u\n",
                                path, i, attribute->attribute_length, back.attribute_length);
                        failed = 1;
                    }
                    else
                    {
                        const size_t at = first_difference(attribute->info, back.info,
                                                           attribute->attribute_length);
                        if (at != attribute->attribute_length)
                        {
                            fprintf(stderr, "FAIL %s: editor byte %zu in method %u\n",
                                    path, at, i);
                            failed = 1;
                        }
                    }
                    attribute_info_free(&back);
                    code_editor_free(&editor);
                }
            }

            (*checked)++;
            attribute_info_free(&rebuilt);
            code_attribute_free(&code);
        }
    }

    return failed;
}

static int check(const char *path, bool verbose, size_t *code_attributes, size_t *instructions)
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

    failed |= check_code_attributes(cf, path, code_attributes, instructions);

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
    size_t instructions = 0;
    for (int i = first; i < argc; i++) {
        total++;
        failures += check(argv[i], verbose, &code_attributes, &instructions);
    }

    printf("%d/%d roundtrips byte-identical, %zu Code attributes, %zu instructions\n",
           total - failures, total, code_attributes, instructions);
    printf("COVER wide=%zu tableswitch=%zu lookupswitch=%zu ldc=%zu ldc_w=%zu "
           "goto_w=%zu invokedynamic=%zu invokeinterface=%zu multianewarray=%zu\n",
           g_seen_wide, g_seen_table_switch, g_seen_lookup_switch, g_seen_ldc, g_seen_ldc_w,
           g_seen_branch_wide, g_seen_invoke_dynamic, g_seen_invoke_interface,
           g_seen_multianewarray);
    printf("FRAMES total=%zu chop=%zu append=%zu full=%zu uninitialized=%zu\n",
           g_seen_frames, g_seen_chop, g_seen_append, g_seen_full, g_seen_uninitialized);
    return failures == 0 ? 0 : 1;
}
