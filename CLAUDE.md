# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

jni2hook rewrites Java class files from C so that a method can be hooked from
native code, and applies the rewrite to a **running** JVM through JVMTI.

The runtime library deliberately has no JDK dependency. The JVM constants live
in `include/jni2hook/utils/class_file_constant.h` and the JNI and JVMTI headers
in `include/jni2hook/jni/`, all vendored from OpenJDK. `JAVA_HOME` is never
consulted by the build. A JDK is only needed to run the tests and examples.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build -j"$(nproc)"
```

Warnings are on and clean: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`
(`/W4` on MSVC). Keep it that way, `-Wconversion` in particular catches the u2
arithmetic mistakes this codebase is full of opportunities for.

Sanitizer build, used for every corpus run that touches allocation:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -G Ninja \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
```

## Testing

There are no unit tests. Correctness rests on two loops, and both matter.

### 1. Byte-identical roundtrip over a generated corpus

```bash
./tests/run_corpus.sh                 # builds the corpus on first run, then checks
WORK=/some/scratch ./tests/run_corpus.sh
ROUNDTRIP=./build-asan/j2h_roundtrip ./tests/run_corpus.sh
```

The script assembles a corpus from every JDK under `/usr/lib/jvm`: `jimage
extract` of each module image, `jar xf lib/ct.sym` for the historical signature
classes (those reach class file versions the installed JDKs cannot produce), the
sources in `tests/corpus/` compiled at every `--release` each javac accepts, and
a generated class large enough to force javac to emit `goto_w`. Roughly 300k
class files, major versions 50 through 70.

`j2h_roundtrip` parses each file and writes it back out, and the result has to
match byte for byte, at four levels: the class file, every Code attribute, every
bytecode instruction, and every StackMapTable. A single file:

```bash
./build/j2h_roundtrip -v path/to/Some.class
```

**Read the `covered:` and `frames:` lines, not just the failure count.** Zero
failures means nothing if the hard encodings never appeared. That check already
paid for itself once: `goto_w` was at 0 across the entire JDK, because javac only
emits it past 32 KB of bytecode, so the wide branch path was completely
untested until the corpus grew a class built for it.

### 2. A real JVM judging the rewritten classes

`defineClass` and `ClassFile.verify` both emit findings that have nothing to do
with us, mostly class hierarchies they cannot resolve from the test classpath.
So a finding only counts **as a delta against a control run over the untouched
originals**:

```bash
# rewrite, then judge, then judge the originals the same way
./build/j2h_transform in.class --all '$jni2hook' out.class      # method -> native + copy
./build/j2h_transform in.class --insert-all x out.class         # call inserted at offset 0
javac -d /tmp/v tests/corpus/VerifyAll.java tests/corpus/DefineAll.java
java -cp /tmp/v VerifyAll /dir/of/rewritten
java -cp /tmp/v VerifyAll /dir/of/originals    # must show the same findings
```

Use the newest installed JDK for this; an older one rejects class files with a
major version it does not know, which looks like a failure but is not.

Both real bugs found so far surfaced only this way, as one extra finding in
20 000 classes: an interface method may never be `ACC_NATIVE`, and the copy has
to inherit `ACC_BRIDGE` and `ACC_SYNCHRONIZED`.

### 3. Inserting at every offset, not just at zero

```bash
./tests/run_insert_offsets.sh      # every offset must still verify
./tests/run_shift_semantics.sh     # and every reference must land where it should
```

Both run under `ctest` when a JDK is present. Offset 0 is the easy case:
everything shifts by the same amount and no reference lands on the insertion
point. The interesting offsets are interior ones, and the two scripts split the
work because a verifier can only see half of it. `run_insert_offsets.sh` rewrites
the corpus classes once per instruction boundary and demands the same
`VerifyAll` findings as the untouched originals. `run_shift_semantics.sh` asserts
where each reference ended up, which is what catches a branch that now jumps over
the hook — a class that verifies perfectly and silently does not fire.

Read the `anchored` and `moved` counts, not just the failure count, for the same
reason as `covered:` above.

### 4. End to end against a live JVM

```bash
javac -d /tmp/rt tests/runtime/HookRuntimeTest.java
java --enable-native-access=ALL-UNNAMED -cp /tmp/rt \
     HookRuntimeTest ./build/libj2h_runtime_test.so

javac -g -d /tmp/mid tests/corpus/MidHookTarget.java
javac    -d /tmp/mid tests/runtime/MidHookTest.java
./build/j2h_transform /tmp/mid/MidHookTarget.class --insert "compute:(I)I@0" entryHook /tmp/mid/entry.class
java -cp /tmp/mid MidHookTest ./build/libj2h_runtime_test.so \
     /tmp/mid/MidHookTarget.class /tmp/mid/entry.class entryHook
```

`HookRuntimeTest` loads the library into a running VM the way an injected
library would, so the JVMTI environment is created in the live phase with only
the capabilities that phase grants. Run it against **every** installed JDK, not
just the default one.

`MidHookTest` compares against the untouched class and *calls* the method, which
forces linking and therefore real bytecode verification. A stale StackMapTable
frame or a wrong exception table offset shows up as a `VerifyError` there and
nowhere earlier.

## Architecture

### Layers

```
visitors/         an owning node tree for the class file format, JVMS chapter 4
class_transform   the two rewrites, expressed against that tree
hook/ + jni2hook  applying a rewrite to a class the JVM has already loaded
```

`byte_stream.{h,c}` sits under all of it: a bounds checked big endian reader and
a growable writer, both of which **latch into a failed state** rather than
reporting per call, so a whole structure can be read and checked once at the end.

### The visitors tree

Every node owns its bytes, so the tree can be edited and written back. The name
is the repository's, the shape is a tree rather than an ASM-style visitor.

The load-bearing decision is that **attributes stay opaque**. An `attribute_info`
carries its payload as bytes and is never interpreted unless something needs it.
That is the whole reason the parser works on class file versions it has never
seen: an attribute is self describing through its length, so an unknown one
survives a rewrite untouched. Do not "improve" this by parsing attributes
eagerly — `BootstrapMethods`, `NestMembers`, `Record` and `PermittedSubclasses`
are all things a naive writer silently drops, and the class breaks far from the
cause.

Attributes that *are* modelled get their own node, parsed on demand out of the
opaque payload and written back into it: `code_attribute`, `stack_map_table`,
and the offset-bearing tables inside `code_editor`.

`instruction.{h,c}` decodes bytecode. Branch targets are stored as **absolute
offsets**, not the relative deltas the format uses. Nothing is lost and
insertion becomes tractable. Three encodings escape the opcode length table:
`wide`, and the two switches, whose padding counts from the start of the code
array and therefore depends on where the instruction sits.

### code_editor, and why insertion is not a matter of adding a delta

Six structures point into the bytecode by offset: branches and switches,
`exception_table`, `LineNumberTable`, `LocalVariableTable` (a span, not a point),
`StackMapTable` (a delta chain, plus `Uninitialized` entries that carry an offset
of their own) and the `localvar`, `offset` and `type_argument` targets inside a
Code attribute's `RuntimeVisible`/`RuntimeInvisibleTypeAnnotations`. The last one
is the single exception to the opaque-attribute rule: the attribute is still
carried as bytes, but `type_annotation.c` records where each offset sits inside
that payload so it can travel with the code and be written straight back.

`code_editor_insert` turns every one of those references into the index of the
instruction it names, splices, recomputes offsets, and reads the references back
out. Adding a delta would be wrong, because the code does not grow by a fixed
amount: moving a switch changes its padding, which changes its length, which
moves everything behind it again. `instruction_list_recompute_offsets` loops
until it settles for exactly that reason.

**Two kinds of reference behave differently at the insertion point itself, and
getting that equality wrong is a VerifyError, not a subtlety.**

A reference that names a *position* — a branch or switch target, a StackMapTable
frame, an exception range bound or handler, a line number, a local variable
scope — stays put and therefore ends up on the first inserted instruction, so a
jump to that offset runs the inserted code. That is `shift_after`, with `>`.

A reference that names one specific *instruction* — an `Uninitialized`
verification type naming its `new`, a type annotation naming the opcode it was
written on — follows that instruction instead. That is `shift_with`, with `>=`.

Using `>=` for the first group is what the code did until it was fixed, and both
halves of the damage are worth knowing. If the instruction before the insertion
point is an unconditional branch, the inserted code becomes unreachable with no
frame covering it and the class is rejected outright. Otherwise the class
verifies perfectly and the branch simply jumps over the hook, so a hook on a loop
back edge fires once instead of once per iteration, with nothing to see. The
corpus roundtrip cannot catch either, because a roundtrip inserts nothing;
`tests/run_insert_offsets.sh` covers the first and `tests/run_shift_semantics.sh`
the second.

Offsets only shift; the frames themselves stay valid because the inserted call
is stack-neutral. Inserting code that changes the stack or locals state at a
branch target would need the StackMapTable recomputed, which means dataflow
analysis and does not exist here.

`instruction_list_encode` rejects rather than truncates. A plain branch carries a
signed 16 bit delta and there is no wide form to grow into for anything but
`goto` and `jsr`, so an insertion between a branch and a target more than 32767
bytes away returns `CLASSFILE_ERR_BRANCH_RANGE`. The same applies to the 65535
byte limit on a method body, which insertion can push a near-limit method over.

### Reserved opcodes

`instruction_list_parse` rejects the three opcodes JVMS 6.2 reserves for the VM
(`breakpoint`, `impdep1`, `impdep2`). They may not appear in a class file, and
they also sit past the end of the opcode length table, which `instruction_length_at`
indexes with a `u1`. That was an out of bounds read reachable from any code array
containing one of them.

### The two rewrites

`class_transform_make_native` marks a method native, drops its Code, and appends
a private final copy holding the original body. `class_transform_insert_call`
leaves the body alone and inserts a call to a fresh native method at a bytecode
offset.

The copy's flags are not a style choice. They are forced by HotSpot, and the
copy also has to inherit every flag that describes the body it took over:
`SYNCHRONIZED` or the moved code loses its monitor, `BRIDGE` or the verifier
stops being lenient about a javac bridge's covariant return.

The native-replacement transform rejects `<init>`, `<clinit>` and interface
methods. Inserted callbacks support instance constructors: an offset before the
initializing `this()` or `super()` call is moved immediately after the call,
where the verifier considers `this` initialized. Static initializers remain
unsupported, and a constructor whose branches reach a *different* initializing
call on each path is refused with `TRANSFORM_ERR_AMBIGUOUS_INIT` rather than
instrumented on whichever path happened to come first. Resolving that properly
needs the common post-dominator of those calls, which is not computed here.

Both transforms leave the class file untouched when they fail. That matters
because the utility API hands the same `ClassFile` back to its caller: a failed
`class_transform_insert_call` used to leave a body invoking a method that was
never appended, which only surfaces at link time.

### The runtime layer, and the flag that gates all of it

`RedefineClasses` refuses to **add** a method unless a VM flag is on:

```cpp
static bool can_add_or_delete(Method* m) {
  return (AllowRedefinitionToAddDeleteMethods &&
          (m->is_private() && (m->is_static() || m->is_final())));
}
```

`AllowRedefinitionToAddDeleteMethods` is `false` by default on JDK 17, 21, 25 and
26, is a product flag so `jcmd VM.set_flag` cannot touch it, and has been
deprecated since 13.0. A library injected into a running game cannot add a JVM
argument, so `hook/vm_structs.c` walks HotSpot's `gHotSpotVMStructs` and
`gHotSpotVMTypes` tables — exported from libjvm for the Serviceability Agent, on
Windows too — finds the `JVMFlag` table and writes the boolean directly. The
tables describe their own layout, which is what makes this survive version
changes; the type was called `Flag` before JDK 13 and both names are tried.

**If installs start failing with `JVMTI_ERROR_UNSUPPORTED_REDEFINITION_METHOD_ADDED`
(63), this is why.** `JNI2Hook_ForcedRedefinitionFlag()` reports `1` forced,
`0` already on, `-1` unreachable. Check it right after `Init`.

If HotSpot ever removes the flag, the copy design dies with it. The fallback is
a nestmate sidecar class holding the original bodies as static methods; the
bytecode stays valid because `aload_0` in a static method with the receiver as
first parameter names the same slot.

### hook/class_cache.c

A loaded class does not hand its class file back, so the bytes are caught by
retransforming it and listening on `ClassFileLoadHook`. Four things about that
are easy to get wrong, and all four are load-bearing:

1. **Capture exactly once, before the first redefinition.** `RedefineClasses`
   replaces the baseline a later retransformation starts from, so capturing again
   after a hook is installed hands back our own rewrite and the original body is
   gone for good.
2. **Switch capture-only mode off again immediately.** A method watch is the
   deliberate exception: while unresolved watches exist, the load callback
   stays enabled, treats initial loads separately from retransforms, parses but
   never rewrites their bytes, and is paired with `ClassPrepare`. The runtime
   test keeps an unmatched watch active across an unrelated `DefineClass` call.
3. What arrives is the class **as it currently stands**, after Mixin and friends
   have had their turn — not the bytes from the jar. That is the point.
4. **A binary name is not an identity, and the callback runs on other threads.**
   Two loaders may each define a class of the same name, and a modded game has
   plenty of loaders, so the cache and the hook registry are both keyed on the
   `jclass` and the callback matches on `class_being_redefined`, not on the name
   it is handed. The callback fires on whichever thread happens to be loading a
   class, so the cache takes a recursive lock of its own; the thread waits there
   in native, which is why holding it across `RetransformClasses` does not stop
   the VM reaching a safepoint.

### hook/class_watch.c

`JNI2Hook_WatchMethod` closes the unloaded-class race in this order: compile and
register the pattern, enable `ClassFileLoadHook` plus `ClassPrepare`, and only
then take a snapshot of already loaded classes. An initial load is parsed from
the raw class-file bytes and records the defining loader, internal class name,
method name, descriptor, static flag, and bytecode offset. `ClassPrepare`
matches both loader identity and class name and turns that record into a
`jmethodID`. `JNI2Hook_GetWatchedMethod` only reads this prepared result; it
never rescans the VM.

`jni2hook.c` keeps a registry per class and always **rebuilds from the cached
original**, applying every hook currently registered, rather than editing
incrementally. That is what lets hooks be added and removed in any order.
Threads are suspended between `RedefineClasses` and `RegisterNatives`, because
in that window the method is native but unbound and any thread reaching it would
get an `UnsatisfiedLinkError`. `SuspendThreadList` reports per thread and only
the threads it actually suspended may be resumed — one that came back
`JVMTI_ERROR_THREAD_SUSPENDED` was already suspended by somebody else. The
result array is allocated before the suspend and kept for the resume, because
failing to allocate it on the way out would leave every one of them suspended
for good.

**Uninstall and shutdown are the part that has to be transactional.** Until
`RedefineClasses` has actually put the body back, the VM is still running the
hooked class, so `JNI2Hook_Uninstall` moves the entries aside instead of freeing
them and only drops them once the rebuild succeeded. On failure the registry
still describes the hook, `JNI2Hook_IsInstalled` still reports it, and the call
can be retried. Freeing first meant a failed rebuild left the detour live while
the registry said it was gone, with nothing left to retry from — exactly the
state a caller must never unload the library in. `JNI2Hook_Shutdown` returns the
first failure any of its restores reported for the same reason, and puts
`AllowRedefinitionToAddDeleteMethods` back the way it found it.

Capabilities are requested only for what `GetPotentialCapabilities` still offers
in the current phase; asking for an ungrantable one fails the entire
`AddCapabilities` call.

## What the library cannot solve for its caller

After `JNI2Hook_Uninstall` the VM may still enter the detour for a while — a JIT
compiled caller reached through a MethodHandle call site keeps the old target.
`hook/trampoline.c` closes that window: `RegisterNatives` binds a page that is
never unmapped, uninstall disarms it, and a late call returns a zero of the
method's return type. The page is leaked on purpose, one per hook.

This is the only architecture dependent code in the library. `trampoline_create`
returns NULL where no emitter exists, the caller's function is then bound
directly, and unloading carries the old risk. x86-64 is emitted today.

What the library *does* now tell its caller is whether the restore happened at
all. A non-OK result from `JNI2Hook_Uninstall` or `JNI2Hook_Shutdown` means a
class is still native and still bound to a function pointer in the library, which
is a separate and much harder failure than the one above: do not unload on it.

## Conventions

- C11, no dependencies beyond libc plus `pthread`/`dlfcn` on Unix or
  `windows.h` on Windows. Compiler extensions stay enabled deliberately for
  `PTHREAD_MUTEX_RECURSIVE` and `RTLD_DEFAULT`.
- Public class-file utilities live below `include/jni2hook/utils/`. Private
  runtime headers remain rooted at `src/`; vendored JNI headers resolve as
  `<jni.h>` because `include/jni2hook/jni` is on the include path.
- The vendored OpenJDK files carry Oracle's GPLv2-with-Classpath-Exception
  header, which has to stay intact. Provenance and the reason for pinning to
  JDK 21 are recorded in the comment at the top of `include/jni2hook/jni/jni_md.h`.
- `jsr`, `jsr_w` and `ret` are implemented but untested: no class file at major
  version 51 or above may contain them, so the corpus cannot reach that path.
