#!/usr/bin/env bash
# Asserts what an insertion does to a reference that names the insertion point
# exactly. This is the half of the reference shifting the verifier cannot see.
#
# A branch whose target was moved past the inserted code still produces a class
# that verifies perfectly. The hook simply never runs when that branch is taken,
# which on a loop back edge means the hook fires once instead of once per
# iteration, silently. So the property is asserted directly rather than through
# a verifier:
#
#   a position reference   keeps naming the insertion point, so a jump to that
#   (branch or switch      offset now runs the inserted code
#   target, frame,
#   handler, range bound,
#   line, local scope)
#
#   an instruction         follows the instruction it named, because an
#   reference (an          Uninitialized verification type has to keep naming
#   Uninitialized frame    its own new
#   entry)
#
# Run it over the corpus built by run_corpus.sh, or point CLASSES at any tree.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
transform="${TRANSFORM:-$root/build/j2h_transform}"
classes="${CLASSES:-${WORK:-${TMPDIR:-/tmp}/jni2hook-corpus}}"
limit="${LIMIT:-2000}"

if [ ! -x "$transform" ]; then
    echo "build j2h_transform first (cmake --build build)" >&2
    exit 2
fi
if [ ! -d "$classes" ]; then
    echo "no class tree at $classes, run tests/run_corpus.sh first or set CLASSES" >&2
    exit 2
fi

points=0
anchored=0
moved=0
failed=0
files=0

while read -r file; do
    files=$((files + 1))
    if ! output="$("$transform" "$file" --verify-shift 2>&1)"; then
        failed=$((failed + 1))
        echo "$output" | sed "s|^|  $(basename "$file"): |" >&2
        continue
    fi
    read -r p a m <<<"$(echo "$output" | tr -dc '0-9 \n' | awk '{print $1, $2, $3}')"
    points=$((points + ${p:-0}))
    anchored=$((anchored + ${a:-0}))
    moved=$((moved + ${m:-0}))
done < <(find "$classes" -name '*.class' | head -"$limit")

echo "== shift semantics =="
printf '  %d class files, %d insertion points\n' "$files" "$points"
printf '  %d position references stayed on the insertion point\n' "$anchored"
printf '  %d instruction references followed their instruction\n' "$moved"
printf '  %d files with a violation\n' "$failed"

# Read the two coverage numbers, not just the failure count. Zero anchored
# references means nothing was actually exercised.
if [ "$anchored" -eq 0 ]; then
    echo "FAIL: no reference ever landed on an insertion point, nothing was tested" >&2
    exit 1
fi
if [ "$moved" -eq 0 ]; then
    echo "warning: no Uninitialized frame entry landed on an insertion point" >&2
fi

exit $((failed == 0 ? 0 : 1))
