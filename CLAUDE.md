# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

jni2hook rewrites Java class files from C so that a method can be hooked from
native code, and applies the rewrite to a **running** JVM through JVMTI. It is a
from-scratch replacement for `rdbo/jnihook`, without that project's C++ and jnif
dependency.

Two things it deliberately does not have: a JDK dependency and any Java code.
The JVM constants live in `src/class_file_constant.h` and the JNI and JVMTI
headers in `include/jni2hook/jni/`, all vendored from OpenJDK. `JAVA_HOME` is
never consulted by the build. A JDK is only needed to *test*.

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

### 3. End to end against a live JVM

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

Five structures point into the bytecode by offset: branches and switches,
`exception_table`, `LineNumberTable`, `LocalVariableTable` (a span, not a point)
and `StackMapTable` (a delta chain, plus `Uninitialized` entries that carry an
offset of their own).

`code_editor_insert` turns every one of those references into the index of the
instruction it names, splices, recomputes offsets, and reads the references back
out. Adding a delta would be wrong, because the code does not grow by a fixed
amount: moving a switch changes its padding, which changes its length, which
moves everything behind it again. `instruction_list_recompute_offsets` loops
until it settles for exactly that reason.

A reference to the offset being inserted at ends up on the first inserted
instruction, so a jump to that point runs the inserted code.

Offsets only shift; the frames themselves stay valid because the inserted call
is stack-neutral. Inserting code that changes the stack or locals state at a
branch target would need the StackMapTable recomputed, which means dataflow
analysis and does not exist here.

### The two rewrites

`class_transform_make_native` marks a method native, drops its Code, and appends
a private final copy holding the original body. `class_transform_insert_call`
leaves the body alone and inserts a call to a fresh native method at a bytecode
offset.

The copy's flags are not a style choice. They are forced by HotSpot, and the
copy also has to inherit every flag that describes the body it took over:
`SYNCHRONIZED` or the moved code loses its monitor, `BRIDGE` or the verifier
stops being lenient about a javac bridge's covariant return.

`<init>`, `<clinit>` and any method of an interface are rejected: none of them
may be native, and a hook at offset 0 of a constructor would sit before the
`super()` call where `this` is still uninitialized.

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
retransforming it and listening on `ClassFileLoadHook`. Three things about that
are easy to get wrong, and all three are load-bearing:

1. **Capture exactly once, before the first redefinition.** `RedefineClasses`
   replaces the baseline a later retransformation starts from, so capturing again
   after a hook is installed hands back our own rewrite and the original body is
   gone for good.
2. **Switch the hook off again immediately.** It fires for every class the VM
   loads while enabled, on every thread, and leaving it on breaks unrelated
   `DefineClass` calls in the process.
3. What arrives is the class **as it currently stands**, after Mixin and friends
   have had their turn — not the bytes from the jar. That is the point.

`jni2hook.c` keeps a registry per class and always **rebuilds from the cached
original**, applying every hook currently registered, rather than editing
incrementally. That is what lets hooks be added and removed in any order.
Threads are suspended between `RedefineClasses` and `RegisterNatives`, because
in that window the method is native but unbound and any thread reaching it would
get an `UnsatisfiedLinkError`.

Capabilities are requested only for what `GetPotentialCapabilities` still offers
in the current phase; asking for an ungrantable one fails the entire
`AddCapabilities` call.

## What the library cannot solve for its caller

After `JNI2Hook_Uninstall` the VM may still enter the detour for a while — a JIT
compiled caller reached through a MethodHandle call site keeps the old target.
Anyone who unloads the library needs a trampoline that stays mapped and can be
disarmed. This is documented in the public header and is not a bug to fix here.

## Conventions

- C11, no dependencies beyond libc and `dlfcn`/`windows.h`.
- Include paths are rooted at `src/`; the vendored JNI headers resolve as
  `<jni.h>` because `include/jni2hook/jni` is on the include path.
- The vendored OpenJDK files carry Oracle's GPLv2-with-Classpath-Exception
  header, which has to stay intact. Provenance and the reason for pinning to
  JDK 21 are recorded in the comment at the top of `include/jni2hook/jni/jni_md.h`.
- `jsr`, `jsr_w` and `ret` are implemented but untested: no class file at major
  version 51 or above may contain them, so the corpus cannot reach that path.
