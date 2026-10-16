#!/bin/bash
# Launch the game, capture its window at given seconds, quit.
# Usage: tools/capture_window.sh OUTPREFIX "T1 T2 ..." [extra env, e.g. SWE1R_VIFIX=vspan]
# Needs an unlocked, awake screen (screencapture -l fails on a locked Mac).
set -e
cd "$(dirname "$0")/.."
OUT=${1:?prefix}; TIMES=${2:-"8 25"}; shift 2 || true
SW=$(mktemp -d); trap 'rm -rf "$SW"' EXIT
cat > "$SW/winid.swift" <<'SWIFT'
import CoreGraphics
for w in (CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as! [[String:Any]])
  where (w["kCGWindowOwnerName"] as? String) == "SWE1Racer" { print(w["kCGWindowNumber"]!) }
SWIFT
swiftc -O "$SW/winid.swift" -o "$SW/winid" 2>/dev/null
LAST=$(echo $TIMES | awk '{print $NF}')
env "$@" SWE1R_AUTOINPUT="${SWE1R_AUTOINPUT:-}" SWE1R_QUIT_AFTER=$((LAST+5)) ./build/SWE1Racer > "$OUT.log" 2>&1 &
PREV=0
for t in $TIMES; do
  sleep $((t-PREV)); PREV=$t
  ID=$("$SW/winid" | head -1)
  screencapture -x -l "$ID" "$OUT-t$t.png" && sips -g pixelWidth -g pixelHeight "$OUT-t$t.png" | tail -2
done
wait
