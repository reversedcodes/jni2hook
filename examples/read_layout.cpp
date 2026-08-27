/* Reading the methods of a class file without a JVM anywhere in sight.
 *
 *     j2h_example_layout <file.class>
 *
 * JNI2Hook_ReadMethodLayout works on plain bytes, so it is usable from a build
 * step or a tool. The order it reports is the order the methods appear in the
 * class file, which is what a slot based wrapper has to agree with. */

#include <jni2hook/jni2hook.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <file.class>\n", argv[0]);
        return 2;
    }

    std::FILE *file = std::fopen(argv[1], "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);

    std::vector<unsigned char> bytes(static_cast<size_t>(size > 0 ? size : 0));
    const bool read =
        !bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);

    if (!read)
    {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    jni2hook_method_layout layout{};
    const jni2hook_status status = JNI2Hook_ReadMethodLayout(bytes.data(), bytes.size(), &layout);
    if (status != JNI2HOOK_OK)
    {
        std::fprintf(stderr, "%s\n", JNI2Hook_StatusMessage(status));
        return 1;
    }

    for (size_t i = 0; i < layout.count; i++)
        std::printf("  slot %2zu  %s%s\n", i, layout.methods[i].name, layout.methods[i].descriptor);

    std::printf("%zu methods\n", layout.count);
    JNI2Hook_FreeMethodLayout(&layout);
    return 0;
}
