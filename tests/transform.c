#include "jni2hook/utils/class_file_parser.h"
#include "jni2hook/utils/class_transform.h"
#include "jni2hook/utils/visitors/code_editor.h"

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

/* One rewritten class per instruction boundary in the file, so that a whole
   method can be swept rather than only offset 0. Inserting at an interior
   offset is where the reference shifting actually gets exercised: a branch
   target, a StackMapTable frame or an exception range boundary sitting exactly
   at the insertion point is what a plain offset 0 hook never reaches. */
static int insert_at_every_offset(const u1 *data, size_t size, const char *directory)
{
    ClassFile *probe = NULL;
    if (classfile_parse(data, size, &probe) != CLASSFILE_OK)
        return -1;

    int written = 0;

    for (u2 m = 0; m < probe->methods.count; m++)
    {
        char name[256], descriptor[512];
        const u1 *bytes;
        u2 length;

        if (!constant_pool_utf8(&probe->constant_pool, probe->methods.items[m].name_index,
                                &bytes, &length) || length >= sizeof(name))
            continue;
        memcpy(name, bytes, length);
        name[length] = 0;

        if (!constant_pool_utf8(&probe->constant_pool, probe->methods.items[m].descriptor_index,
                                &bytes, &length) || length >= sizeof(descriptor))
            continue;
        memcpy(descriptor, bytes, length);
        descriptor[length] = 0;

        attribute_info *code = attribute_list_find(&probe->methods.items[m].attributes,
                                                   &probe->constant_pool, "Code");
        if (code == NULL)
            continue;

        code_editor editor;
        if (code_editor_load(code, &probe->constant_pool, &editor) != CLASSFILE_OK)
            continue;

        for (u4 k = 0; k < editor.instructions.count; k++)
        {
            const u4 offset = editor.instructions.items[k].offset;

            ClassFile *cf = NULL;
            if (classfile_parse(data, size, &cf) != CLASSFILE_OK)
                continue;

            char hook_name[64];
            snprintf(hook_name, sizeof(hook_name), "hook$%u$%u", (unsigned)m, (unsigned)offset);

            classfile_status cause = CLASSFILE_OK;
            const transform_status result =
                class_transform_insert_call(cf, name, descriptor, offset, hook_name, &cause);
            if (result != TRANSFORM_OK)
            {
                if (result == TRANSFORM_ERR_CLASSFILE)
                    fprintf(stderr, "  %s%s@%u: %s (%s)\n", name, descriptor, (unsigned)offset,
                            transform_status_message(result), classfile_status_message(cause));
                classFile_destroy(cf);
                continue;
            }

            u1 *out = NULL;
            size_t out_size = 0;
            const classfile_status serialized = classfile_serialize(cf, &out, &out_size);
            classFile_destroy(cf);
            if (serialized != CLASSFILE_OK)
                continue;

            char path[1024];
            snprintf(path, sizeof(path), "%s/m%u_o%u.class", directory, (unsigned)m,
                     (unsigned)offset);
            if (write_file(path, out, out_size) == 0)
                written++;
            free(out);
        }

        code_editor_free(&editor);
    }

    classFile_destroy(probe);
    return written;
}

/* Checks what an insertion does to a reference that names the insertion point
 * exactly, which is the half of the shifting the verifier cannot catch.
 *
 * A branch whose target moved past the inserted code still produces a class
 * that verifies; the hook simply never runs on that path. So the property is
 * asserted directly instead:
 *
 *   a position reference (branch or switch target, StackMapTable frame,
 *   exception range bound or handler, line number, local variable scope) that
 *   named the insertion point keeps naming it, and therefore now names the
 *   inserted code
 *
 *   an instruction reference (an Uninitialized verification type naming its
 *   new) follows the instruction it named to its new offset
 */
typedef struct
{
    u4 positions;
    u4 instructions;
} shift_expectation;

static shift_expectation collect_at(const code_editor *editor, i4 value)
{
    shift_expectation found = {0, 0};

    for (u4 i = 0; i < editor->instructions.count; i++)
    {
        const instruction *node = &editor->instructions.items[i];
        if (node->kind == OPERAND_BRANCH || node->kind == OPERAND_BRANCH_WIDE)
        {
            if (node->u.branch.target == value)
                found.positions++;
        }
        else if (node->kind == OPERAND_TABLE_SWITCH)
        {
            if (node->u.table_switch.default_target == value)
                found.positions++;
            const u4 entries = instruction_switch_entries(node);
            for (u4 k = 0; k < entries; k++)
                if (node->u.table_switch.targets[k] == value)
                    found.positions++;
        }
        else if (node->kind == OPERAND_LOOKUP_SWITCH)
        {
            if (node->u.lookup_switch.default_target == value)
                found.positions++;
            const u4 pairs = instruction_switch_entries(node);
            for (u4 k = 0; k < pairs; k++)
                if (node->u.lookup_switch.pairs[k].target == value)
                    found.positions++;
        }
    }

    for (u2 i = 0; i < editor->exceptions.count; i++)
    {
        const exception_entry *entry = &editor->exceptions.items[i];
        found.positions += (u4)(entry->start_pc == value);
        found.positions += (u4)(entry->end_pc == value);
        found.positions += (u4)(entry->handler_pc == value);
    }

    for (u2 i = 0; i < editor->line_number_count; i++)
        found.positions += (u4)(editor->line_numbers[i].start_pc == value);

    for (u2 i = 0; i < editor->stack_map.count; i++)
    {
        const stack_map_frame *frame = &editor->stack_map.frames[i];
        found.positions += (u4)(frame->offset == value);

        for (u2 k = 0; k < frame->locals_count; k++)
            if (frame->locals[k].tag == JVM_ITEM_Uninitialized &&
                frame->locals[k].offset == value)
                found.instructions++;
        for (u2 k = 0; k < frame->stack_count; k++)
            if (frame->stack[k].tag == JVM_ITEM_Uninitialized && frame->stack[k].offset == value)
                found.instructions++;
    }

    return found;
}

static int verify_shift(const u1 *data, size_t size)
{
    ClassFile *probe = NULL;
    if (classfile_parse(data, size, &probe) != CLASSFILE_OK)
        return -1;

    u4 checked = 0, anchored = 0, moved = 0;
    int failures = 0;

    for (u2 m = 0; m < probe->methods.count; m++)
    {
        char name[256], descriptor[512];
        const u1 *bytes;
        u2 length;

        if (!constant_pool_utf8(&probe->constant_pool, probe->methods.items[m].name_index,
                                &bytes, &length) || length >= sizeof(name))
            continue;
        memcpy(name, bytes, length);
        name[length] = 0;

        if (!constant_pool_utf8(&probe->constant_pool, probe->methods.items[m].descriptor_index,
                                &bytes, &length) || length >= sizeof(descriptor))
            continue;
        memcpy(descriptor, bytes, length);
        descriptor[length] = 0;

        /* A constructor is deliberately not inserted at the requested offset:
           an offset before the initialising this()/super() call is moved past
           it, so the reference at that offset is left alone on purpose and
           there is nothing to assert here. HookRuntimeTest covers that path. */
        if (strcmp(name, "<init>") == 0)
            continue;

        attribute_info *code = attribute_list_find(&probe->methods.items[m].attributes,
                                                   &probe->constant_pool, "Code");
        if (code == NULL)
            continue;

        code_editor before;
        if (code_editor_load(code, &probe->constant_pool, &before) != CLASSFILE_OK)
            continue;

        for (u4 k = 0; k < before.instructions.count; k++)
        {
            const u4 offset = before.instructions.items[k].offset;
            const shift_expectation expected = collect_at(&before, (i4)offset);
            if (expected.positions == 0 && expected.instructions == 0)
                continue;

            ClassFile *cf = NULL;
            if (classfile_parse(data, size, &cf) != CLASSFILE_OK)
                continue;

            classfile_status cause = CLASSFILE_OK;
            if (class_transform_insert_call(cf, name, descriptor, offset, "probe$hook", &cause) !=
                TRANSFORM_OK)
            {
                classFile_destroy(cf);
                continue;
            }

            member_info *method = classFile_find_method(cf, name, descriptor);
            attribute_info *rewritten = method == NULL
                                            ? NULL
                                            : attribute_list_find(&method->attributes,
                                                                  &cf->constant_pool, "Code");
            code_editor after;
            if (rewritten == NULL ||
                code_editor_load(rewritten, &cf->constant_pool, &after) != CLASSFILE_OK)
            {
                classFile_destroy(cf);
                continue;
            }

            /* The instruction that used to sit at offset moved along by however
               many nodes were spliced in. Taking the first instruction past the
               offset instead would land on the second inserted node, not on the
               relocated original. */
            i4 relocated = -1;
            if (after.instructions.count > before.instructions.count)
            {
                const u4 inserted = after.instructions.count - before.instructions.count;
                if (k + inserted < after.instructions.count)
                    relocated = (i4)after.instructions.items[k + inserted].offset;
            }

            const shift_expectation stayed = collect_at(&after, (i4)offset);
            const shift_expectation followed =
                relocated < 0 ? (shift_expectation){0, 0} : collect_at(&after, relocated);

            checked++;
            if (stayed.positions != expected.positions)
            {
                fprintf(stderr,
                        "  %s%s@%u: %u position references named the insertion point, %u still do\n",
                        name, descriptor, (unsigned)offset, (unsigned)expected.positions,
                        (unsigned)stayed.positions);
                failures++;
            }
            else
            {
                anchored += expected.positions;
            }

            if (followed.instructions != expected.instructions)
            {
                fprintf(stderr,
                        "  %s%s@%u: %u instruction references should have followed to %d, %u did\n",
                        name, descriptor, (unsigned)offset, (unsigned)expected.instructions,
                        relocated, (unsigned)followed.instructions);
                failures++;
            }
            else
            {
                moved += expected.instructions;
            }

            code_editor_free(&after);
            classFile_destroy(cf);
        }

        code_editor_free(&before);
    }

    classFile_destroy(probe);
    printf("  %u insertion points checked, %u references anchored, %u moved with their "
           "instruction, %d failures\n",
           checked, anchored, moved, failures);
    return failures;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[2], "--verify-shift") == 0)
    {
        size_t size = 0;
        u1 *data = read_file(argv[1], &size);
        if (data == NULL)
        {
            fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }

        const int failures = verify_shift(data, size);
        free(data);
        return failures == 0 ? 0 : 1;
    }

    if (argc == 4 && strcmp(argv[2], "--insert-each") == 0)
    {
        size_t size = 0;
        u1 *data = read_file(argv[1], &size);
        if (data == NULL)
        {
            fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }

        const int written = insert_at_every_offset(data, size, argv[3]);
        free(data);
        if (written < 0)
        {
            fprintf(stderr, "parse %s failed\n", argv[1]);
            return 1;
        }

        printf("%d %s\n", written, argv[3]);
        return 0;
    }

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
                "       %s <in.class> --insert <name:descriptor@offset> <hook-name> <out.class>\n"
                "       %s <in.class> --insert-all <unused> <out.class>\n"
                "       %s <in.class> --insert-each <out-directory>\n"
                "       %s <in.class> --verify-shift\n",
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
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
