#!/usr/bin/env bash
# Automated level health sweep.
# Loads every level headless via `outlaws --check`, prints a per-level health
# line (sectors/walls/entities/unresolved-INF/missing-textures/start), and a
# PASS/FAIL summary. Use to catch regressions and find levels that need fixing.
#
#   tools/check_levels.sh            # all levels
#   tools/check_levels.sh TOWN TRAIN # specific levels
set -u
cd "$(dirname "$0")/.."
BIN=./build/outlaws
XVFB=(xvfb-run -a -s "-screen 0 320x240x24")

# Story campaign, "A Handful of Missions" DLC, and multiplayer maps.
STORY="TOWN TRAIN CANYON DRYGULCH FORT HIDEOUT WILDERNS MILL MINER RANCH SIMMS CLIFF INDY GRANARY MLTSIMMS"
DLC="SHFORT SHGRANRY SHGULCH SHSIMMS SHWILDNS"
MP="TOWN_2P GALLERY SHOOT OFFICE"
LEVELS="${*:-$STORY $DLC $MP}"

pass=0; fail=0; failed=""
printf "%-10s %-8s %-7s %-6s %-6s %-6s %-9s %-8s %s\n" \
       LEVEL SECTORS WALLS ENTS ENEMIES ITEMS UNRESOLV MISSTEX RESULT
printf '%.0s-' {1..92}; echo
for lvl in $LEVELS; do
    line=$("${XVFB[@]}" "$BIN" --level "$lvl" --check 2>/dev/null | grep "^CHECK" | head -1)
    if [ -z "$line" ]; then
        printf "%-10s %s\n" "$lvl" "NO OUTPUT (crash?)"; fail=$((fail+1)); failed="$failed $lvl"; continue
    fi
    get(){ echo "$line" | grep -oE "$1=[0-9-]+" | cut -d= -f2; }
    res=$(echo "$line" | grep -oE "RESULT=[A-Z]+" | cut -d= -f2)
    printf "%-10s %-8s %-7s %-6s %-6s %-6s %-9s %-8s %s\n" \
        "$lvl" "$(get sectors)" "$(get walls)" "$(get ents)" \
        "$(get enemies)" "$(get items)" "$(get inf_unresolved)" "$(get miss_tex)" "$res"
    if [ "$res" = PASS ]; then pass=$((pass+1)); else fail=$((fail+1)); failed="$failed $lvl"; fi
done
printf '%.0s-' {1..92}; echo
echo "PASS=$pass FAIL=$fail${failed:+  (failed:$failed)}"
[ "$fail" -eq 0 ]
