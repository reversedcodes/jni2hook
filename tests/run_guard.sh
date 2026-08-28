#!/usr/bin/env bash
# A hook that decides whether the original body runs at all.
#
# Everything else inserted is observation: the call runs and the body runs after
# it. Skipping the body means branching over it, and a branch target needs a
# StackMapTable frame. Computing one in general is dataflow analysis this
# library does not do -- but at method entry the frame is the state the method
# starts in, readable straight off the descriptor, so that one place is
# tractable.
#
# Whether the derivation is right is not something the writer can answer. The
# JVM's verifier answers it, which is why this runs the methods rather than
# only rewriting them, and why it covers the shapes that make a frame
# interesting: a long and a double take two local slots but one frame entry
# each, an array's verification type names the descriptor rather than a class, a
# void method has nothing to return and a static one has no receiver.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
library="${BRIDGE_LIBRARY:-$root/build/libj2h_java_bridge.so}"
work="${WORK:-${TMPDIR:-/tmp}/jni2hook-guard}"

if [ ! -f "$library" ]; then
    echo "build j2h_java_bridge first (cmake --build build)" >&2
    exit 2
fi

jdk="${JAVA_HOME:-}"
if [ -z "$jdk" ] || [ ! -x "$jdk/bin/javac" ]; then
    for d in /usr/lib/jvm/*/; do
        [ -x "${d%/}/bin/javac" ] && jdk="${d%/}"
    done
fi
if [ -z "$jdk" ] || [ ! -x "$jdk/bin/javac" ]; then
    echo "no javac found, skipping" >&2
    exit 2
fi

rm -rf "$work"
mkdir -p "$work/app" "$work/bridge"

"$jdk/bin/javac" -d "$work/app" "$here/runtime/BridgeTarget.java" \
                                "$here/runtime/GuardTest.java" || exit 2
"$jdk/bin/javac" -d "$work/bridge" "$here/runtime/HandleBridge.java" || exit 2

native_access=""
"$jdk/bin/java" --enable-native-access=ALL-UNNAMED -version >/dev/null 2>&1 \
    && native_access="--enable-native-access=ALL-UNNAMED"

exec "$jdk/bin/java" $native_access -cp "$work/app" GuardTest \
    "$library" "$work/bridge/HandleBridge.class"
