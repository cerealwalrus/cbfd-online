#!/usr/bin/env bash
# Netplay scouting, pass 6. Run from the repo root (same folder as build.cmd).
# Usage: bash netplay_scout6.sh      -> writes netplay_scout6.txt (text only)

set -u
if [ ! -f conker.toml ] || [ ! -d RecompiledFuncs ]; then
    echo "Wrong folder or build not finished: $(pwd)"
    exit 1
fi
RF=RecompiledFuncs
OUT=netplay_scout6.txt
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

# The scheduler function that starts a frame (it sets the frame length D_800BE9E4 from the retrace counter).
body func_10004F00 80

# The one game-side reader of the retrace counter (D_8003B238): is it the game's "frames since last frame"?
body func_151D57F8 140
ctx "func_151D57F8(rdram" "Callers of func_151D57F8 (first 25), with context" 25 8 6
users "func_151D57F8(rdram" "Functions calling func_151D57F8"

# Who else reads the value func_151D57F8 returns is visible in the call sites above.
# What the main game-frame function (func_15007830) does with the player count and the mode byte.
users "-0X1618" "Functions touching 0x800BE9E8 again (for the record)"

echo "Wrote $OUT ($(wc -c < "$OUT") bytes)"
