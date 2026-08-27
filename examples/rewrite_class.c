/* Applies both rewrites to a class file on disk and writes the result out.
 *
 *     j2h_example_rewrite native <in.class> <method> <descriptor> <out.class>
 *     j2h_example_rewrite call   <in.class> <method> <descriptor> <offset> <out.class>
 *
 * native   makes the method native and parks its body in a private copy, which
 *          is what a hook that replaces a method needs.
 * call     leaves the body alone and inserts a call to a fresh native method at
 *          a bytecode offset, which is what a hook that only observes needs.
 *
 * No JVM is involved. Run javap -c on the output to see what changed, or hand
 * the file to a JVM and let its verifier judge it. */

#include <jni2hook/utils/class_file.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int write_file(const char *path, const u1 *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return 1;
    const int failed = fwrite(data, 1, size, f) != size;
    fclose(f);
    return failed;
}

int main(int argc, char **argv)
{
    const bool insert = argc >= 2 && strcmp(argv[1], "call") == 0;
    const bool native = argc >= 2 && strcmp(argv[1], "native") == 0;

    if ((!native || argc != 6) && (!insert || argc != 7))
    {
        fprintf(stderr,
                "usage: %s native <in.class> <method> <descriptor> <out.class>\n"
                "       %s call   <in.class> <method> <descriptor> <offset> <out.class>\n",
                argv[0], argv[0]);
        return 2;
    }

    const char *input = argv[2];
    const char *method = argv[3];
    const char *descriptor = argv[4];
    const char *output = insert ? argv[6] : argv[5];

    size_t size = 0;
    u1 *data = read_file(input, &size);
    if (data == NULL)
    {
        fprintf(stderr, "cannot read %s\n", input);
        return 1;
    }

    ClassFile *cf = NULL;
    classfile_status status = classfile_parse(data, size, &cf);
    free(data);
    if (status != CLASSFILE_OK)
    {
        fprintf(stderr, "%s: %s\n", input, classfile_status_message(status));
        return 1;
    }

    classfile_status cause = CLASSFILE_OK;
    transform_status result;

    if (insert)
    {
        /* strtoul reports "abc" as 0, which would silently insert at the start
           of the method instead of refusing. The end pointer is what tells the
           two apart. Whether the offset actually lands on an instruction
           boundary is then checked by the library itself. */
        char *end = NULL;
        errno = 0;
        const unsigned long offset = strtoul(argv[5], &end, 0);

        if (end == argv[5] || *end != '\0' || errno == ERANGE || offset > 0xFFFFu)
        {
            fprintf(stderr, "not a bytecode offset: %s\n", argv[5]);
            classFile_destroy(cf);
            return 2;
        }

        result = class_transform_insert_call(cf, method, descriptor, (u4)offset, "hook$jni2hook",
                                             &cause);
    }
    else
    {
        result = class_transform_make_native(cf, method, descriptor, "original$jni2hook", &cause);
    }

    if (result != TRANSFORM_OK)
    {
        fprintf(stderr, "%s%s: %s", method, descriptor, transform_status_message(result));
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
        fprintf(stderr, "%s\n", classfile_status_message(status));
        return 1;
    }

    const int failed = write_file(output, out, out_size);
    free(out);
    if (failed)
    {
        fprintf(stderr, "cannot write %s\n", output);
        return 1;
    }

    printf("%s %s%s -> %s (%zu bytes)\n", insert ? "inserted a call into" : "made native", method,
           descriptor, output, out_size);
    return 0;
}
