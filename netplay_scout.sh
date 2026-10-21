#!/usr/bin/env bash
# Netplay scouting for CBFD-Recompiled.
# Run from the repo root (WSL or Linux), after the decomp build and `sh recomp/run.sh`.
# Writes netplay_scout.txt: small, text only, no game data beyond function names and a few call sites.
# Usage: bash netplay_scout.sh

set -u
OUT=netplay_scout.txt
RF=RecompiledFuncs
ASM=conker/conker/asm
SYMS=conker/conker/symbol_addrs.us.txt
UNDEF=conker/conker/undefined_syms.us.txt
: > "$OUT"

section() { printf '\n===== %s =====\n' "$1" >> "$OUT"; }

section "Layout"
{ ls -d "$RF" "$ASM" 2>&1; ls "$RF" 2>/dev/null | wc -l; } >> "$OUT"

section "Symbols: RNG, timing, input, VI"
grep -hE "random_u32|random_float|osGetCount|osGetTime|osSetTimer|osViSetEvent|osContStartReadData|osContGetReadData|osContInit|osViSwapBuffer" \
    "$SYMS" "$UNDEF" 2>/dev/null >> "$OUT"

# Which functions call a given libultra/RNG function, in the recompiled C.
callers() {
    local name="$1"
    section "Callers of $name (recompiled C: function file + count)"
    grep -rlE "\b${name}\b" "$RF" --include='*.c' 2>/dev/null | head -60 >> "$OUT"
    printf 'total files: ' >> "$OUT"
    grep -rlE "\b${name}\b" "$RF" --include='*.c' 2>/dev/null | wc -l >> "$OUT"
}
for f in osGetCount osGetTime osSetTimer osViSetEvent osContStartReadData osContGetReadData; do
    callers "$f"
done

section "Callers of random_u32 / random_float (count only)"
for f in random_u32 random_float; do
    printf '%s: ' "$f" >> "$OUT"
    grep -rhoE "\b${f}\b" "$RF" --include='*.c' 2>/dev/null | wc -l >> "$OUT"
done

section "Decomp asm: where D_800BE616 (MP flag) and D_800BE628 (cameras) are touched"
if [ -d "$ASM" ]; then
    for sym in D_800BE616 D_800BE628; do
        printf '%s:\n' "$sym" >> "$OUT"
        grep -rlE "\b${sym}\b" "$ASM" 2>/dev/null | head -40 >> "$OUT"
    done
else
    echo "asm dir missing: run 'make extract' / 'make -C conker extract' first" >> "$OUT"
fi

section "Decomp asm: random_u32 body and its seed (writes near the generator)"
if [ -d "$ASM" ]; then
    f=$(grep -rlE "glabel (func_150ADA20|random_u32)\b" "$ASM" 2>/dev/null | head -1)
    echo "file: ${f:-not found}" >> "$OUT"
    if [ -n "${f:-}" ]; then
        awk '/glabel (func_150ADA20|random_u32)\b/{p=1} p{print} /endlabel|\.size/{if(p)exit}' "$f" | head -80 >> "$OUT"
    fi
fi

section "Decomp asm: first 40 call sites of osGetCount / osGetTime (caller file : line)"
if [ -d "$ASM" ]; then
    grep -rnE "jal +(osGetCount|osGetTime)\b" "$ASM" 2>/dev/null | head -40 >> "$OUT"
fi

section "host/ hooks that read the wall clock (sim-affecting risk)"
grep -rnE "steady_clock|high_resolution_clock|system_clock|SDL_GetTicks" host/src 2>/dev/null | head -40 >> "$OUT"

section "conker.toml hooks (function names only)"
grep -E '^func = ' conker.toml | sort | uniq -c | sort -rn | head -80 >> "$OUT"

echo "Wrote $OUT ($(wc -c < "$OUT") bytes)"
