#ifndef JNI2HOOK_HOOK_VM_STRUCTS_H
#define JNI2HOOK_HOOK_VM_STRUCTS_H

#include <stdbool.h>

/* Reads HotSpot's VMStructs tables, the self describing type and field
 * directory that libjvm exports for the Serviceability Agent.
 *
 * It exists here for one reason. RedefineClasses refuses to add a method
 * unless the VM flag AllowRedefinitionToAddDeleteMethods is on, and that flag
 * defaults to off on every current JDK:
 *
 *     static bool can_add_or_delete(Method* m) {
 *       return (AllowRedefinitionToAddDeleteMethods &&
 *               (m->is_private() && (m->is_static() || m->is_final())));
 *     }
 *
 * Since the flag is a product flag and not manageable, no supported interface
 * can change it in a running VM, and a library injected into a game that is
 * already running cannot restart it with a different command line. What is left
 * is to find the flag in HotSpot's own flag table and set it there.
 *
 * Walking VMStructs rather than hardcoding offsets is what makes this survive
 * across JDK versions: the tables carry their own entry layout, the type names,
 * the field names and the offsets, so nothing has to be known in advance except
 * the names being looked for.
 */

bool vm_structs_init(void);

/* Sets a product flag of type bool in the VM's flag table. out_previous, when
   given, receives the value the flag had. */
bool vm_structs_set_bool_flag(const char *name, bool value, bool *out_previous);

#endif
