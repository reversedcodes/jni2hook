#include "jni2hook/utils/visitors/visitor.h"

const char *classfile_status_message(classfile_status status)
{
    switch (status)
    {
    case CLASSFILE_OK:
        return "ok";
    case CLASSFILE_ERR_TRUNCATED:
        return "class file truncated";
    case CLASSFILE_ERR_MAGIC:
        return "bad magic, not a class file";
    case CLASSFILE_ERR_VERSION:
        return "unsupported class file version";
    case CLASSFILE_ERR_CONSTANT_TAG:
        return "unknown constant pool tag";
    case CLASSFILE_ERR_CONSTANT_INDEX:
        return "constant pool index out of range";
    case CLASSFILE_ERR_TRAILING_BYTES:
        return "extra bytes after end of class file";
    case CLASSFILE_ERR_LIMIT_EXCEEDED:
        return "class file limit exceeded";
    case CLASSFILE_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case CLASSFILE_ERR_OPCODE:
        return "reserved or unknown opcode in the code array";
    case CLASSFILE_ERR_BAD_OFFSET:
        return "the offset is not an instruction boundary";
    case CLASSFILE_ERR_BRANCH_RANGE:
        return "a branch no longer reaches its target";
    case CLASSFILE_ERR_CODE_TOO_LARGE:
        return "the method body exceeds 65535 bytes";
    case CLASSFILE_ERR_UNSUPPORTED:
        return "the structure cannot be written back";
    }
    return "unknown error";
}
