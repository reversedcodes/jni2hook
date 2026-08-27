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
    }
    return "unknown error";
}
