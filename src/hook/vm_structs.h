#ifndef JNI2HOOK_HOOK_VM_STRUCTS_H
#define JNI2HOOK_HOOK_VM_STRUCTS_H

#include <stdbool.h>

/* Reads HotSpot's self-describing VMStructs tables instead of hardcoding
   version-specific offsets into the JVMFlag table. */

bool vm_structs_init(void);

/* Sets a product flag of type bool in the VM's flag table. out_previous, when
   given, receives the value the flag had. */
bool vm_structs_set_bool_flag(const char *name, bool value, bool *out_previous);

#endif
