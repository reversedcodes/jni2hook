#ifndef JNI2HOOK_CLASSFILE_PARSER_H
#define JNI2HOOK_CLASSFILE_PARSER_H

#include "visitors/class_file.h"

/* Reads a class file into a node tree. The input buffer is not retained: every
   node owns a copy of its own bytes, so the tree can be edited freely and the
   caller may release the input right after the call. */
classfile_status classfile_parse(const u1 *data, size_t size, ClassFile **out);

/* Writes the node tree back out. For an unmodified tree the result is byte for
   byte the input, which is what proves the reader dropped nothing. */
classfile_status classfile_serialize(const ClassFile *cf, u1 **out, size_t *out_size);

#endif
