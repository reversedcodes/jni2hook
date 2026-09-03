#ifndef JNI2HOOK_TRAMPOLINE_H
#define JNI2HOOK_TRAMPOLINE_H

#include <stdbool.h>

/* A resident forwarder for a hooked method.
 *
 * The VM may still reach a detour after JNI2Hook_Uninstall: a JIT compiled
 * caller behind a MethodHandle call site keeps the old target. Binding the
 * caller's function directly therefore means a caller that unloads itself
 * leaves the VM pointing into unmapped memory.
 *
 * A trampoline is a page that is never unmapped. While armed it jumps to the
 * caller's function; once disarmed it returns a zero of the method's return
 * type. Stale call sites then reach a no-op instead of a hole.
 *
 * The page is deliberately leaked. Freeing it would reopen the very window it
 * exists to close, and one page per hook is bounded by the number of hooks.
 *
 * Emitting instructions makes this the only architecture dependent code in the
 * library. Where no emitter exists, trampoline_create returns NULL and the
 * caller binds its function directly, which is the behaviour of every release
 * before this one. */

/* return_kind is the JVM descriptor letter of the return type: V, Z, B, C, S,
   I, J, F, D, or L and [ for references. */
void *trampoline_create(void *target, char return_kind);

/* Idempotent, and safe on NULL. After this the page returns without calling. */
void trampoline_disarm(void *trampoline);

bool trampoline_supported(void);

#endif
