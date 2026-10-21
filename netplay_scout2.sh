#!/usr/bin/env bash
# Netplay scouting, pass 2. Run from the repo root (same folder as build.cmd), after the build.
# Usage: bash netplay_scout2.sh      -> writes netplay_scout2.txt (text only)

set -u
if [ ! -f conker.toml ] || [ ! -d RecompiledFuncs ]; then
    echo "Wrong folder or build not finished: $(pwd)"
    echo "Run from the repo root (has build.cmd, conker.toml and RecompiledFuncs/)."
    exit 1
fi
RF=RecompiledFuncs
OUT=netplay_scout2.txt
: > "$OUT"
section() { printf '\n===== %s =====\n' "$1" >> "$OUT"; }

# For each call site of $1, print "enclosing_function: call text" (unique functions, with counts).
callers() {
    local pat="$1"
    section "Functions calling ${pat}* (enclosing function : times)"
    # awk tracks the current function definition line: ...name(uint8_t* rdram, recomp_context* ctx) {
    find "$RF" -name '*.c' -print0 2>/dev/null | xargs -0 awk -v pat="$pat" '
        /\(uint8_t\* *rdram, *recomp_context\* *ctx\) *\{/ {
            if (match($0, /[A-Za-z_0-9]+\(uint8_t/)) { fn = substr($0, RSTART, RLENGTH - 8) }
        }
        index($0, pat) && !/^[^(]*\(uint8_t/ { cnt[fn " -> " substr($0, index($0, pat), 40)]++ }
        END { for (k in cnt) print cnt[k], k }
    ' 2>/dev/null | sort -rn | head -60 >> "$OUT"
}
for p in osGetCount osGetTime osSetTimer osViSetEvent osContStartReadData osContGetReadData osContInit osViGetCurrentFramebuffer; do
    callers "$p"
done

section "Raw sample lines: how a libultra call looks in the generated C"
grep -rhE "osGetCount|osGetTime|osContGetReadData" "$RF" --include='*.c' 2>/dev/null | head -8 >> "$OUT"

section "random_u32 definition (recompiled body)"
f=$(grep -rlE "^[A-Za-z_ ]*\brandom_u32\(uint8_t" "$RF" --include='*.c' 2>/dev/null | head -1)
echo "file: ${f:-not found}" >> "$OUT"
if [ -n "${f:-}" ]; then
    awk '/[ \t*]random_u32\(uint8_t/{p=1} p{print; n++} p&&/^}/{exit} n>150{exit}' "$f" >> "$OUT"
fi

section "random_float definition (recompiled body)"
f=$(grep -rlE "^[A-Za-z_ ]*\brandom_float\(uint8_t" "$RF" --include='*.c' 2>/dev/null | head -1)
echo "file: ${f:-not found}" >> "$OUT"
if [ -n "${f:-}" ]; then
    awk '/[ \t*]random_float\(uint8_t/{p=1} p{print; n++} p&&/^}/{exit} n>150{exit}' "$f" >> "$OUT"
fi

section "Functions that call random_u32 most (top 25)"
find "$RF" -name '*.c' -print0 2>/dev/null | xargs -0 awk '
    /\(uint8_t\* *rdram, *recomp_context\* *ctx\) *\{/ { if (match($0, /[A-Za-z_0-9]+\(uint8_t/)) fn = substr($0, RSTART, RLENGTH - 8) }
    /random_u32\(rdram/ { cnt[fn]++ }
    END { for (k in cnt) print cnt[k], k }
' 2>/dev/null | sort -rn | head -25 >> "$OUT"

section "Hook targets in conker.toml (full function + the hook it calls)"
grep -nE '^(func|before_vram|after_vram|text|hook)' conker.toml 2>/dev/null | head -120 >> "$OUT"

echo "Wrote $OUT ($(wc -c < "$OUT") bytes)"
