#!/usr/bin/env bash
# Netplay scouting, pass 3. Run from the repo root (same folder as build.cmd).
# Usage: bash netplay_scout3.sh      -> writes netplay_scout3.txt (text only)

set -u
if [ ! -f conker.toml ] || [ ! -d RecompiledFuncs ]; then
    echo "Wrong folder or build not finished: $(pwd)"
    exit 1
fi
RF=RecompiledFuncs
OUT=netplay_scout3.txt
: > "$OUT"
section() { printf '\n===== %s =====\n' "$1" >> "$OUT"; }
allc() { find "$RF" -name '*.c' -print0 2>/dev/null; }

# body NAME MAXLINES: print a recompiled function's body (without the per-instruction comment lines).
body() {
    local name="$1" max="$2"
    section "Body of $name (comments stripped, max $max lines)"
    allc | xargs -0 awk -v n="$name" -v max="$max" '
        $0 ~ ("RECOMP_FUNC void " n "\\(") { p = 1; c = 0 }
        p {
            if ($0 !~ /^[ \t]*\/\/ 0x/ && $0 !~ /^[ \t]*$/ && $0 !~ /CHECK_FR|NAN_CHECK/) { print; c++ }
            if ($0 ~ /^;?}/ || c >= max) { exit }
        }' 2>/dev/null | head -"$max" >> "$OUT"
}

# ctx PATTERN: every line matching PATTERN with its enclosing function and 5 lines of context (comments stripped).
ctx() {
    local pat="$1" label="$2" max="$3"
    section "$label"
    allc | xargs -0 awk -v pat="$pat" -v max="$max" '
        /RECOMP_FUNC void / { if (match($0, /void [A-Za-z_0-9]+\(/)) fn = substr($0, RSTART + 5, RLENGTH - 6); delete hist; nh = 0 }
        { line = $0
          if (line !~ /^[ \t]*\/\/ 0x/ && line !~ /^[ \t]*$/ && line !~ /CHECK_FR|NAN_CHECK/) { hist[nh++] = line }
          if (index(line, pat) && total < max) {
              total++
              printf "--- %s\n", fn
              for (i = (nh > 6 ? nh - 6 : 0); i < nh; i++) print "    " hist[i]
          } }
    ' 2>/dev/null >> "$OUT"
}

ctx "func_150ADACC(rdram" "Who seeds the RNG: calls to func_150ADACC (RNG state = arg + 1), with preceding lines" 12

section "All code touching the RNG state (offset -0X7A50 after lui 0X8009): enclosing functions"
allc | xargs -0 awk '
    /RECOMP_FUNC void / { if (match($0, /void [A-Za-z_0-9]+\(/)) fn = substr($0, RSTART + 5, RLENGTH - 6) }
    /-0X7A50/ { c[fn]++ }
    END { for (k in c) print c[k], k }' 2>/dev/null | sort -rn | head -40 >> "$OUT"

body getTime2 60
ctx "osGetCount_recomp" "osGetCount call sites with preceding lines (first 30)" 30
body func_1501BBB8 160
body func_100049E0 220
body func_1501BB20 120

section "Multiplayer flag D_800BE616 (offset -0X19EA) and camera array D_800BE628 (offset -0X19D8): functions using those offsets"
allc | xargs -0 awk '
    /RECOMP_FUNC void / { if (match($0, /void [A-Za-z_0-9]+\(/)) fn = substr($0, RSTART + 5, RLENGTH - 6) }
    /-0X19EA/ { a[fn]++ }
    /-0X19D8/ { b[fn]++ }
    END { for (k in a) print "flag  ", a[k], k; for (k in b) print "camera", b[k], k }' 2>/dev/null | sort | head -80 >> "$OUT"

body func_15015F40 120

echo "Wrote $OUT ($(wc -c < "$OUT") bytes)"
