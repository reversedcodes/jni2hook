#!/usr/bin/env bash
# Compiles the corpus sources at every --release each installed JDK accepts,
# extracts each JDK's own module image and its historical ct.sym signatures,
# then checks that every resulting class file survives a parse/serialize
# roundtrip byte for byte.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
roundtrip="${ROUNDTRIP:-$root/build/j2h_roundtrip}"
work="${WORK:-${TMPDIR:-/tmp}/jni2hook-corpus}"

if [ ! -x "$roundtrip" ]; then
    echo "build j2h_roundtrip first (cmake --build build)" >&2
    exit 2
fi

mkdir -p "$work"
jdks=()
seen=""
for d in /usr/lib/jvm/*/; do
    [ -x "$d/bin/javac" ] || continue
    real="$(readlink -f "${d%/}")"
    case " $seen " in *" $real "*) continue;; esac
    seen="$seen $real"
    jdks+=("$real")
done
if [ "${#jdks[@]}" -eq 0 ]; then
    echo "no JDK found under /usr/lib/jvm" >&2
    exit 2
fi

echo "== compiling corpus at every supported --release =="
for jdk in "${jdks[@]}"; do
    name="$(basename "$jdk")"
    for release in $(seq 7 30); do
        out="$work/release/$release"
        [ -d "$out" ] && continue
        mkdir -p "$out"
        if ! "$jdk/bin/javac" --release "$release" -nowarn -d "$out" \
             "$here/corpus/Basic.java" >/dev/null 2>&1; then
            rmdir "$out" 2>/dev/null
            continue
        fi
        "$jdk/bin/javac" --release "$release" -nowarn -d "$out" \
             "$here/corpus/Modern.java" >/dev/null 2>&1
        printf '  release %-2s -> %s (%s classes)\n' \
               "$release" "$name" "$(find "$out" -name '*.class' | wc -l)"
    done
done

echo "== generating the wide branch case =="
if [ ! -f "$work/wide/WideBranch.class" ]; then
    mkdir -p "$work/wide"
    {
        echo "public class WideBranch {"
        echo "    public static int run(int n) {"
        echo "        int x = 0;"
        echo "        while (x < n) {"
        i=0
        while [ "$i" -lt 12000 ]; do echo "            x += 1;"; i=$((i+1)); done
        echo "        }"
        echo "        return x;"
        echo "    }"
        echo "}"
    } > "$work/wide/WideBranch.java"
    "${jdks[0]}/bin/javac" -nowarn -d "$work/wide" "$work/wide/WideBranch.java" >/dev/null 2>&1 \
        && echo "  goto_w is only emitted past 32 KB of bytecode, so this class is built for it"
fi

echo "== extracting module images and ct.sym =="
for jdk in "${jdks[@]}"; do
    name="$(basename "$jdk")"
    if [ -f "$jdk/lib/modules" ] && [ ! -d "$work/modules/$name" ]; then
        "$jdk/bin/jimage" extract --dir "$work/modules/$name" "$jdk/lib/modules" >/dev/null 2>&1 \
            && printf '  %-24s %s classes\n' "$name modules" \
                      "$(find "$work/modules/$name" -name '*.class' | wc -l)"
    fi
    if [ -f "$jdk/lib/ct.sym" ] && [ ! -d "$work/ctsym/$name" ]; then
        mkdir -p "$work/ctsym/$name"
        (cd "$work/ctsym/$name" && "$jdk/bin/jar" xf "$jdk/lib/ct.sym") >/dev/null 2>&1
        find "$work/ctsym/$name" -type f ! -name '*.sig' -delete 2>/dev/null
        count="$(find "$work/ctsym/$name" -name '*.sig' | wc -l)"
        [ "$count" -gt 0 ] && printf '  %-24s %s signature classes\n' "$name ct.sym" "$count"
    fi
done

echo "== class file versions present =="
find "$work" \( -name '*.class' -o -name '*.sig' \) -print0 \
  | xargs -0 -P "$(nproc)" -n 200 -- sh -c '
      for f; do
        v=$(od -An -tu1 -j6 -N2 "$f" 2>/dev/null | tr -s " " | sed "s/^ //")
        [ -n "$v" ] && echo "$v" | awk "{print \$1*256+\$2}"
      done' _ | sort -n | uniq -c | awk '{printf "  major %-4s %6d files\n", $2, $1}'

echo "== roundtrip =="
find "$work" \( -name '*.class' -o -name '*.sig' \) -print0 \
  | xargs -0 -n 400 "$roundtrip" 2>&1 \
  | awk '/FAIL/{f++; if (f<=10) print "  "$0}
         /^COVER/{for(i=2;i<=NF;i++){split($i,kv,"="); cover[kv[1]]+=kv[2]}}
         /^FRAMES/{for(i=2;i<=NF;i++){split($i,kv,"="); frame[kv[1]]+=kv[2]}}
         /roundtrips/{split($1,a,"/"); ok+=a[1]; tot+=a[2]; code+=$4; ins+=$7}
         END{printf "  %d/%d class files, %d Code attributes, %d instructions, %d failures\n",
                    ok, tot, code, ins, f+0
             printf "  covered:"
             n=split("wide tableswitch lookupswitch ldc ldc_w goto_w invokedynamic invokeinterface multianewarray", k, " ")
             for (i=1;i<=n;i++) printf " %s=%d", k[i], cover[k[i]]
             printf "\n  frames: "
             m=split("total chop append full uninitialized", g, " ")
             for (i=1;i<=m;i++) printf " %s=%d", g[i], frame[g[i]]
             printf "\n"; exit (f>0)}'
