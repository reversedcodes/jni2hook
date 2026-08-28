#!/usr/bin/env bash
# The bridge, against targets that are not where the easy test put them.
#
# Everything else runs with the target on the application class path, in the
# unnamed module, with nothing in front of it. Nothing in a real host looks like
# that, and two departures from it are worth having a permanent answer for:
#
#   loader   the target belongs to a class loader of its own
#   module   the target sits in a named module
#
# The second one is why this exists. It caught a class_cache defect that no
# other test could reach: a class loaded while a retransform is in flight
# arrives with class_being_redefined pointing at the class being retransformed
# rather than NULL, so matching on that identity alone cached
# java/lang/Module$ReflectionData under the target's name. The original body
# would have been gone for good, and the symptom would have been a bytecode
# signature that stopped matching, a long way from the cause.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
library="${BRIDGE_LIBRARY:-$root/build/libj2h_java_bridge.so}"
work="${WORK:-${TMPDIR:-/tmp}/jni2hook-loader-cases}"

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
mkdir -p "$work/app" "$work/target" "$work/bridge" "$work/msrc/bridgetarget/target" "$work/mods"

"$jdk/bin/javac" -d "$work/target" "$here/runtime/BridgeTarget.java" || exit 2
"$jdk/bin/javac" -d "$work/app"    "$here/runtime/LoaderCasesTest.java" || exit 2
"$jdk/bin/javac" -d "$work/bridge" "$here/runtime/HandleBridge.java" || exit 2

sed '1i package target;' "$here/runtime/BridgeTarget.java" \
    > "$work/msrc/bridgetarget/target/BridgeTarget.java"
printf 'module bridgetarget {\n    exports target;\n}\n' \
    > "$work/msrc/bridgetarget/module-info.java"
"$jdk/bin/javac" --module-source-path "$work/msrc" -d "$work/mods" --module bridgetarget || exit 2

native_access=""
"$jdk/bin/java" --enable-native-access=ALL-UNNAMED -version >/dev/null 2>&1 \
    && native_access="--enable-native-access=ALL-UNNAMED"

status=0

echo "== the target behind a class loader of its own =="
"$jdk/bin/java" $native_access -cp "$work/app" LoaderCasesTest \
    "$library" loader "$work/bridge/HandleBridge.class" \
    "$work/target/BridgeTarget.class" || status=1

echo
echo "== the target inside a named module =="
"$jdk/bin/java" $native_access --module-path "$work/mods" --add-modules bridgetarget \
    -cp "$work/app" LoaderCasesTest \
    "$library" module "$work/bridge/HandleBridge.class" target.BridgeTarget || status=1

exit $status
