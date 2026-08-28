#ifndef JNI2HOOK_VISITORS_VISITOR_H
#define JNI2HOOK_VISITORS_VISITOR_H

#include "../java_version.h"

#include "../class_file_constant.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The three unsigned widths the class file format is built from (JVMS 4.1). */
typedef uint8_t u1;
typedef uint16_t u2;
typedef uint32_t u4;

/* Bytecode offsets and branch deltas are signed. */
typedef int32_t i4;

typedef enum
{
    CLASSFILE_OK = 0,
    CLASSFILE_ERR_TRUNCATED,
    CLASSFILE_ERR_MAGIC,
    CLASSFILE_ERR_VERSION,
    CLASSFILE_ERR_CONSTANT_TAG,
    CLASSFILE_ERR_CONSTANT_INDEX,
    CLASSFILE_ERR_TRAILING_BYTES,
    CLASSFILE_ERR_LIMIT_EXCEEDED,
    CLASSFILE_ERR_OUT_OF_MEMORY,
    CLASSFILE_ERR_OPCODE,
    CLASSFILE_ERR_BAD_OFFSET,
    CLASSFILE_ERR_BRANCH_RANGE,
    CLASSFILE_ERR_CODE_TOO_LARGE,
    CLASSFILE_ERR_UNSUPPORTED
} classfile_status;

const char *classfile_status_message(classfile_status status);

/* Every count in a class file is a u2, so no list may exceed this. */
#define CLASSFILE_MAX_COUNT 0xFFFFu

/* JVMS 4.7.3: code_length must be greater than zero and less than 65536, which
   is also what caps every bytecode offset at a u2. */
#define JVM_MAX_CODE_LENGTH 65535u

#endif
