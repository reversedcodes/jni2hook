#ifndef JNI2HOOK_UTILS_CLASS_FILE_H
#define JNI2HOOK_UTILS_CLASS_FILE_H

/* Offline class-file API. Parsed nodes own their data, unknown attributes stay
   opaque, and an unmodified tree serializes byte-for-byte identically. */

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
