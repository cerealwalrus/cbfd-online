#!/bin/sh
# Every character made from a mesh, rebuilt (tools/objchar/obj_character.py), its options as it needs them. --hints: what a mesh's
# own shape can't settle (tools/objchar/autorig.py's rig()): legs:none (no legs: the body one piece), legs:rigid, hip:F (the legs'
# tops at F of the height), head:spine (a head that is most of the character goes with the body), feet:stiff, arm+1:none, arm-1:none.
cd "$(dirname "$0")/../.."
b() { python3 -I tools/objchar/obj_character.py "$@" 2>&1 | grep -E "own skeleton|arm |leg |tail|four legs|no legs|hint|crotch|written|rror|turned"; }
b "tools/objchar/src/Jam Jars" jamjars --tex=0x2140 --hints=hip:0.1,feet:stiff
b tools/objchar/src/lowpoly banjo --tex=0x2160 --hints=legs:none,arm+1:none,arm-1:none
b tools/objchar/src/mx/metatrix metatrix --tex=0x2180
b "tools/objchar/src/snake/Naked Snake" snake --tex=0x2190
b tools/objchar/src/lowsnake lowsnake --tex=0x21B0
b tools/objchar/src/cloud cloud --tex=0x21D0
b tools/objchar/src/garunda garunda --tex=0x2300 --tail=keep
b tools/objchar/src/scales scales --tex=0x2310 --tail=keep
b tools/objchar/src/king king --tex=0x2330 --tail=keep
b tools/objchar/src/kenny kenny --tex=0x2350 --height=120 --hints=head:spine,legs:none
b tools/objchar/src/rocky rocky --tex=0x2360 --hints=legs:none
b tools/objchar/src/fox fox --tex=0x2380 --height=175 --tail=keep --hints=hip:0.35,feet:stiff
b tools/objchar/src/dreamkasco dreamkasco --tex=0x23A0
