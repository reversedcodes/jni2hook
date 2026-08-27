#ifndef JNI2HOOK_VISITORS_CODE_ATTRIBUTE_H
#define JNI2HOOK_VISITORS_CODE_ATTRIBUTE_H

#include "attribute_info.h"

/* One row of the exception_table of JVMS 4.7.3. A catch_type of zero means the
   handler is a finally block and catches everything. */
typedef struct {
    u2 start_pc;
    u2 end_pc;
    u2 handler_pc;
    u2 catch_type;
} exception_entry;

typedef struct {
    exception_entry *items;
    u2               count;
    u2               capacity;
} exception_table;

void             exception_table_init(exception_table *table);
void             exception_table_free(exception_table *table);
classfile_status exception_table_reserve(exception_table *table, u2 capacity);
classfile_status exception_table_append(exception_table *table, exception_entry entry);

/* Code_attribute, JVMS 4.7.3. Not part of the class file node tree: attributes
   stay opaque there, and this node is parsed out of one on demand and written
   back into it after editing. The nested attributes, StackMapTable among them,
   are themselves kept opaque. */
typedef struct {
    u2              max_stack;
    u2              max_locals;
    u4              code_length;
    u1             *code;           /* owned */
    exception_table exceptions;
    attribute_list  attributes;
} code_attribute;

void code_attribute_init(code_attribute *code);
void code_attribute_free(code_attribute *code);

/* Reads the payload of a Code attribute. The caller is responsible for having
   checked that the attribute really is named "Code". */
classfile_status code_attribute_parse(const attribute_info *attribute, code_attribute *out);

/* Writes the node back into the payload of an attribute, keeping its name. */
classfile_status code_attribute_write(const code_attribute *code, attribute_info *attribute);

classfile_status code_attribute_set_code(code_attribute *code, const u1 *bytes, u4 length);

#endif
