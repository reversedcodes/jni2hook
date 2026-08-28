#!/usr/bin/env bash
# Inserts a hook at every single instruction boundary of every method in the
# corpus classes and verifies each result.
#
# Offset 0 is the easy case: everything in the method shifts by the same amount
# and no reference lands exactly on the insertion point. The interesting
# offsets are interior ones, where a branch target, a StackMapTable frame, an
# exception range boundary or a local variable scope sits precisely where the
# code is spliced in. Getting the equality wrong there produced a class the
# verifier rejected outright, and the corpus roundtrip could not see it because
# a roundtrip does not insert anything.
#
# A finding only counts as a delta against the untouched class, the same rule
# the VerifyAll loop uses everywhere else.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
transform="${TRANSFORM:-$root/build/j2h_transform}"
work="${WORK:-${TMPDIR:-/tmp}/jni2hook-insert-offsets}"

if [ ! -x "$transform" ]; then
    echo "build j2h_transform first (cmake --build build)" >&2
    exit 2
fi

jdk="${JAVA_HOME:-}"
if [ -z "$jdk" ]; then
    for d in /usr/lib/jvm/*/; do
        [ -x "$d/bin/javac" ] || continue
        jdk="${d%/}"
    done
fi
if [ -z "$jdk" ] || [ ! -x "$jdk/bin/javac" ]; then
    echo "no JDK found, set JAVA_HOME" >&2
    exit 2
fi

rm -rf "$work"
mkdir -p "$work/src" "$work/verify" "$work/control" "$work/rewritten"

echo "== compiling the targets =="
"$jdk/bin/javac" -g -d "$work/src" \
    "$here/corpus/MidHookTarget.java" \
    "$here/corpus/Basic.java" \
    "$here/corpus/Modern.java" \
    "$here/corpus/TypeAnnotated.java" 2>/dev/null
"$jdk/bin/javac" -d "$work/verify" "$here/corpus/VerifyAll.java" "$here/corpus/DefineAll.java" || exit 2

classes=$(find "$work/src" -name '*.class' | sort)
if [ -z "$classes" ]; then
    echo "nothing compiled" >&2
    exit 2
fi

echo "== inserting at every instruction boundary =="
total=0
for class in $classes; do
    name="$(basename "$class" .class)"
    cp "$class" "$work/control/$name.class"

    out="$work/rewritten/$name"
    mkdir -p "$out"
    written="$("$transform" "$class" --insert-each "$out" | cut -d' ' -f1)"
    printf '  %-24s %s offsets\n' "$name" "$written"
    total=$((total + written))
done
echo "  $total rewritten classes"

if [ "$total" -eq 0 ]; then
    echo "no offset produced a rewrite, something is wrong" >&2
    exit 1
fi

echo "== control: the untouched classes =="
"$jdk/bin/java" -cp "$work/verify" VerifyAll "$work/control"
control=$?

echo "== rewritten =="
"$jdk/bin/java" -cp "$work/verify" VerifyAll "$work/rewritten"
rewritten=$?

if [ "$control" -ne "$rewritten" ]; then
    echo "FAIL: the rewrites verify differently from the originals" >&2
    exit 1
fi

echo "ok: every insertion point verifies exactly as the original does"
