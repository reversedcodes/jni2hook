#!/usr/bin/env bash
# The loader and remapper API against a live VM.
#
# RemapPlugin is compiled against RemapApi and RemapValue and then run on a
# class path that has neither, only the obfuscated ra and rv. Every step after
# DefineClass therefore depends on the remapper having rewritten the super
# class, the inherited method reference and the descriptors first: without it
# the class would not even link.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
library="${LOADER_API_LIBRARY:-$root/build/libj2h_loader_api.so}"
work="${WORK:-${TMPDIR:-/tmp}/jni2hook-loader-api}"

if [ ! -f "$library" ]; then
    echo "build j2h_loader_api first (cmake --build build)" >&2
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
mkdir -p "$work/compile" "$work/runtime" "$work/plugin" "$work/app"

# The readable API the plugin is built against, kept away from the run.
"$jdk/bin/javac" -d "$work/compile" \
    "$here/runtime/RemapApi.java" "$here/runtime/RemapValue.java" || exit 2

# What the VM will actually have.
"$jdk/bin/javac" -d "$work/runtime" \
    "$here/runtime/ra.java" "$here/runtime/rv.java" || exit 2

"$jdk/bin/javac" -cp "$work/compile" -d "$work/plugin" "$here/runtime/RemapPlugin.java" || exit 2
"$jdk/bin/javac" -d "$work/app" "$here/runtime/LoaderApiTest.java" || exit 2

native_access=""
"$jdk/bin/java" --enable-native-access=ALL-UNNAMED -version >/dev/null 2>&1 \
    && native_access="--enable-native-access=ALL-UNNAMED"

# $work/compile is deliberately absent from this class path.
"$jdk/bin/java" $native_access -cp "$work/app:$work/runtime" LoaderApiTest \
    "$library" "$work/plugin/RemapPlugin.class"
