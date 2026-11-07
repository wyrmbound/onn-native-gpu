#!/bin/sh
# 60-push-run.sh: push the runtime bundle, driver, GL/SDL libs, tests and game to /data/local/tmp/pb on the
# box, make sure Shizuku is running, open OnnShell's zero-copy display and start the game.
# Needs on the box: Shizuku and OnnShell installed (CLAUDE.md stage 6). Changes nothing outside
# /data/local/tmp except starting Shizuku. Stop the game with the PS button, or:
#   adb -s <BOX> shell 'kill $(pidof linker64)'
set -eu
. "$(dirname "$0")/common.sh"
export MSYS_NO_PATHCONV=1   # Git Bash: do not rewrite /data/... paths
: "${ADB:=adb}"
: "${BOX:?set BOX in kit.env}"
a() { "$ADB" -s "$BOX" "$@" </dev/null; }

say "box $BOX"
"$ADB" connect "$BOX" >/dev/null 2>&1 || true
[ "$(a get-state 2>/dev/null)" = device ] || die "box $BOX is not on adb (adb debugging on? same network? authorized?)"

say "local NEEDED check"
[ -x "$BIN/llvm-readelf" ] || die "no toolchain; run 10-toolchain.sh first"
[ -f "$BUNDLE/linker64" ] || die "no bundle; run 20-bionic-bundle.sh first"
for f in libvulkan_panfrost.so libEGL.so libGLESv2.so libgallium_dri.so libSDL2-2.0.so.0 sm64 run_sm64.sh; do
  [ -f "$PB/$f" ] || die "missing $PB/$f; run 30, 40 and 50 first"
done
bad=""
for f in "$BUNDLE"/* "$PB"/*; do
  for n in $(needed "$f"); do [ -e "$BUNDLE/$n" ] || [ -e "$PB/$n" ] || bad="$bad $(basename "$f")->$n"; done
done
[ -z "$bad" ] || die "unresolved NEEDED:$bad"
echo "every NEEDED resolves within bundle + pb"

say "push to $BOX_PB"
a shell "mkdir -p $BOX_PB/cfg"
a shell 'kill $(pidof linker64) 2>/dev/null' || true   # a running game holds the libs open
n=0
for f in "$BUNDLE"/* "$PB"/*; do   # bundle first, so our own builds win on a name clash
  a push "$f" "$BOX_PB/" >/dev/null || die "push failed: $f"
  n=$((n + 1))
done
a shell "cd $BOX_PB && chmod 755 linker64 m1 m2h m3h m4h m5h m6 m7 m8 sm64 run_sm64.sh"
echo "pushed $n files"

say "Shizuku"
S=$KIT/onnshell/scripts/shizuku-start.sh
[ -f "$S" ] || die "$S missing; clone the kit with --recursive (git submodule update --init)"
ADB="$ADB" ONN_HOST="${BOX%:*}" ONN_PORT="${BOX##*:}" sh "$S"

say "zero-copy display + game"
# am force-stop is ignored when OnnShell is Device Owner/home; a live :bridge blocks the next buffer offer.
a shell 'pkill -9 -f "com.xplsh.onnshell:bridg[e]"' || true
sleep 1
a shell "am start -n com.xplsh.onnshell/.display.ZeroCopyActivity --ez launch false --ei rate 32000 --ei channels 2"
sleep 4
echo "starting run_sm64.sh (game stderr goes to $BOX_PB/sm64.log; PS button or Ctrl-C ends it)"
a shell "sh $BOX_PB/run_sm64.sh" || true
a pull "$BOX_PB/sm64.log" "$WORK/logs/sm64-box.log" >/dev/null 2>&1 && echo "game log: $WORK/logs/sm64-box.log"
