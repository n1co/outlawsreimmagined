#!/usr/bin/env bash
# Automated door test sweep.
# Runs `outlaws --level X --check-doors` on every level: each door is opened
# without the key (locked must refuse, unlocked must open), then with the key
# (must unlock), and the doorway collision is checked to flip solid→passable.
# Prints a per-level door count + PASS/FAIL and a summary. Add a level name to
# print that level's full per-door table.
#
#   tools/check_doors.sh            # summary for all levels
#   tools/check_doors.sh HIDEOUT    # full per-door table for one level
set -u
cd "$(dirname "$0")/.."
BIN=./build/outlaws
XVFB=(xvfb-run -a -s "-screen 0 320x240x24")

STORY="TOWN TRAIN CANYON HIDEOUT WILDERNS MILL MINER RANCH SIMMS CLIFF INDY"
MP="DRYGULCH FORT GRANARY MLTSIMMS TOWN_2P GALLERY SHOOT OFFICE"
DLC="SHFORT SHGRANRY SHGULCH SHSIMMS SHWILDNS"

# Single level → print its full table verbatim.
if [ "$#" -eq 1 ]; then
    "${XVFB[@]}" "$BIN" --level "$1" --check-doors 2>/dev/null | grep -E "DOOR CHECK|SECTOR|PASS|FAIL|SLIDE|SWING|DOOR|INVDOOR"
    exit ${PIPESTATUS[0]}
fi

LEVELS="${*:-$STORY $DLC $MP}"
pass=0; fail=0; failed=""
printf "%-10s %-7s %-7s %-7s %s\n" LEVEL DOORS PASSED FAILED RESULT
printf '%.0s-' {1..48}; echo
for lvl in $LEVELS; do
    line=$("${XVFB[@]}" "$BIN" --level "$lvl" --check-doors 2>/dev/null | grep "^DOOR CHECK:" | head -1)
    if [ -z "$line" ]; then
        printf "%-10s %s\n" "$lvl" "NO DOORS / no output"; continue
    fi
    n=$(echo "$line"    | grep -oE "[0-9]+ doors" | grep -oE "[0-9]+")
    p=$(echo "$line"    | grep -oE "PASS=[0-9]+" | cut -d= -f2)
    f=$(echo "$line"    | grep -oE "FAIL=[0-9]+" | cut -d= -f2)
    res=$(echo "$line"  | grep -oE "RESULT=[A-Z]+" | cut -d= -f2)
    printf "%-10s %-7s %-7s %-7s %s\n" "$lvl" "$n" "$p" "$f" "$res"
    if [ "$res" = PASS ]; then pass=$((pass+1)); else fail=$((fail+1)); failed="$failed $lvl"; fi
done
printf '%.0s-' {1..48}; echo
echo "LEVELS PASS=$pass FAIL=$fail${failed:+  (doors failed in:$failed)}"
[ "$fail" -eq 0 ]
