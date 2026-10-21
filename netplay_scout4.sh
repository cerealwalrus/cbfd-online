#!/usr/bin/env bash
# Netplay scouting, pass 4. Run from the repo root (same folder as build.cmd).
# Usage: bash netplay_scout4.sh      -> writes netplay_scout4.txt (text only)

set -u
if [ ! -f conker.toml ] || [ ! -d RecompiledFuncs ]; then
    echo "Wrong folder or build not finished: $(pwd)"
    exit 1
fi
RF=RecompiledFuncs
OUT=netplay_scout4.txt
: > "$OUT"
section() { printf '\n===== %s =====\n' "$1" >> "$OUT"; }
allc() { find "$RF" -name '*.c' -print0 2>/dev/null; }

# ctx PATTERN LABEL MAXHITS BEFORE AFTER [FUNC]: matching lines with BEFORE lines before and AFTER lines after
# (instruction comments stripped). If FUNC is given, only hits inside that function.
ctx() {
    local pat="$1" label="$2" max="$3" before="$4" after="$5" only="${6:-}"
    section "$label"
    allc | xargs -0 awk -v pat="$pat" -v max="$max" -v before="$before" -v after="$after" -v only="$only" '
        function flush() { }
        /RECOMP_FUNC void / { if (match($0, /void [A-Za-z_0-9]+\(/)) fn = substr($0, RSTART + 5, RLENGTH - 6); nh = 0; pend = 0 }
        {
            line = $0
            skip = (line ~ /^[ \t]*\/\/ 0x/ || line ~ /^[ \t]*$/ || line ~ /CHECK_FR|NAN_CHECK/)
            if (pend > 0 && !skip) { print "    " line; pend-- }
            if (!skip) {
                if (index(line, pat) && total < max && (only == "" || fn == only)) {
                    total++
                    printf "--- %s\n", fn
                    for (i = (nh > before ? nh - before : 0); i < nh; i++) print "    " hist[i]
                    print "  > " line
                    pend = after
                }
                hist[nh++] = line
            }
        }
    ' 2>/dev/null >> "$OUT"
}

# users PATTERN LABEL: enclosing functions that contain PATTERN, with counts.
users() {
    local pat="$1" label="$2"
    section "$label"
    allc | xargs -0 awk -v pat="$pat" '
        /RECOMP_FUNC void / { if (match($0, /void [A-Za-z_0-9]+\(/)) fn = substr($0, RSTART + 5, RLENGTH - 6) }
        index($0, pat) { c[fn]++ }
        END { for (k in c) print c[k], k }' 2>/dev/null | sort -rn | head -50 >> "$OUT"
}

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

# 1. RNG seed values: 45 lines before each seed call, inside the two seeding functions.
ctx "func_150ADACC(rdram" "RNG seed in func_15007830 (45 lines before)" 3 45 4 func_15007830
ctx "func_150ADACC(rdram" "RNG seed in func_1501905C (45 lines before)" 3 45 4 func_1501905C

# 2. Frame-pacing counters (D_8003B238 retraces since last frame, +9 min per frame, +A cooldown).
users "-0X4DC8" "Functions touching D_8003B238 (offset -0X4DC8, lui 0X8004)"
users "-0X4DC7" "Functions touching D_8003B239 (offset -0X4DC7)"
users "-0X4DC6" "Functions touching D_8003B23A (offset -0X4DC6)"

# 3. What osGetCount feeds: lines AFTER the first calls.
ctx "osGetCount_recomp" "After osGetCount in func_1500C2F0 (first 3)" 3 2 14 func_1500C2F0
ctx "osGetCount_recomp" "After osGetCount in func_15008340 (first 3)" 3 2 14 func_15008340
ctx "osGetCount_recomp" "After osGetCount in func_15193CA0 (first 2)" 2 2 14 func_15193CA0
users "osGetCount_recomp" "All osGetCount callers"
ctx "getTime2(rdram" "Callers of getTime2 (the game's own osGetTime copy)" 20 6 6

# 4. Player count and multiplayer flag.
body func_151E5FAC 80
users "-0X1618" "Functions touching 0x800BE9E8 (player count minus 1; offset -0X1618, lui 0X800C)"
ctx "-0X19EA" "Multiplayer flag users: func_15002F40" 4 12 6 func_15002F40
ctx "-0X19EA" "Multiplayer flag users: func_150076BC" 4 12 6 func_150076BC

echo "Wrote $OUT ($(wc -c < "$OUT") bytes)"
