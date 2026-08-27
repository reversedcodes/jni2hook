#ifndef JNI2HOOK_UTILS_CLASS_FILE_H
#define JNI2HOOK_UTILS_CLASS_FILE_H

/* The class file reader, writer and editor on their own, without the JVMTI half
 * of jni2hook. Everything here works on a byte buffer and needs no running JVM,
 * so it is equally usable for tooling that inspects or rewrites class files
 * offline.
 *
 * The design is worth knowing before using it:
 *
 * - Nodes own their bytes. classfile_parse copies what it needs, so the input
 *   buffer can be released immediately and the tree can be edited freely.
 *
 * - Attributes stay opaque. An attribute_info carries its payload as bytes and
 *   is only interpreted when something asks. That is what lets the parser
 *   handle class file versions it has never seen, and it is why an unknown
 *   attribute survives a rewrite untouched instead of being silently dropped.
 *   Attributes that are modelled — Code, StackMapTable, the offset bearing
 *   tables — are parsed out of that payload on demand and written back into it.
 *
 * - Re-serializing an unmodified tree reproduces the input byte for byte. That
 *   is not a nicety, it is the correctness test the whole library rests on.
 *
 * - Branch targets are absolute offsets rather than the relative deltas the
 *   format stores, which is what makes code_editor_insert able to move five
 *   different kinds of offset reference at once.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include "jni2hook/utils/visitors/attribute_info.h"
#include "jni2hook/utils/visitors/class_file.h"
#include "jni2hook/utils/visitors/code_attribute.h"
#include "jni2hook/utils/visitors/code_editor.h"
#include "jni2hook/utils/visitors/cp_info.h"
#include "jni2hook/utils/visitors/instruction.h"
#include "jni2hook/utils/visitors/member_info.h"
#include "jni2hook/utils/visitors/stack_map_table.h"
#include "jni2hook/utils/visitors/visitor.h"

#include "class_file_parser.h"
#include "class_transform.h"

#ifdef __cplusplus
}
#endif

#endif
