#!/bin/sh
# 61-tests.sh: run the milestone harnesses m1..m8 on the box in order and print PASS/FAIL for each.
# Run 60-push-run.sh first (it pushes the files and starts Shizuku). Output of each step: $WORK/logs/mN.txt.
#   m1 Vulkan clear  m2 triangle  m3 1280x720 frames (to a file)  m4 zero-copy into OnnShell (on the TV)
#   m5 EGL/GLES on zink  m6 SDL GLES (headless)  m7 SDL audio (440 Hz tone on the TV)  m8 gamepad (pair one)
set -eu
. "$(dirname "$0")/common.sh"
export MSYS_NO_PATHCONV=1
: "${ADB:=adb}"
: "${BOX:?set BOX in kit.env}"
a() { "$ADB" -s "$BOX" "$@" </dev/null; }
"$ADB" connect "$BOX" >/dev/null 2>&1 || true
[ "$(a get-state 2>/dev/null | tr -d '\r')" = device ] || die "box $BOX is not on adb"

ENV="cd $BOX_PB && LD_LIBRARY_PATH=$BOX_PB"
ICD=$BOX_PB/libvulkan_panfrost.so
GL="ZINK_VK_ICD=$ICD MESA_LOADER_DRIVER_OVERRIDE=zink"
SDLV="SDL_VIDEODRIVER=offscreen SDL_VIDEO_GL_DRIVER=$BOX_PB/libGLESv2.so SDL_VIDEO_EGL_DRIVER=$BOX_PB/libEGL.so"
fails=0

# check NAME: PASS if logs/NAME.txt has "NAME PASS"
check() {
  if grep -q "$1 PASS" "$WORK/logs/$1.txt"; then echo "$1 PASS"
  else echo "$1 FAIL  (last lines of $WORK/logs/$1.txt:)"; tail -n 3 "$WORK/logs/$1.txt" | sed 's/^/    /'; fails=$((fails + 1)); fi
}
run() { name=$1; shift; a shell "$*" > "$WORK/logs/$name.txt" 2>&1 || true; check "$name"; }
reset_display() {
  a shell 'kill $(pidof linker64) 2>/dev/null; pkill -9 -f "com.xplsh.onnshell:bridg[e]"' || true
  sleep 1
}
leave_display() { a shell "am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null 2>&1" || true; reset_display; }

reset_display
run M1 "$ENV ./linker64 $BOX_PB/m1 $ICD"
run M2 "$ENV ./linker64 $BOX_PB/m2h $ICD"
run M3 "$ENV PB_OUT=$BOX_PB/m3.raw ./linker64 $BOX_PB/m3h $ICD; rm -f $BOX_PB/m3.raw"

# M4: OnnShell launches m4h itself through Shizuku and logs its output under tag M4
a shell "logcat -c"
a shell "am start -n com.xplsh.onnshell/.display.ZeroCopyActivity --ei frames 300" >/dev/null
i=0
while [ $i -lt 30 ]; do
  sleep 2; i=$((i + 1))
  a shell "logcat -d -s M4" > "$WORK/logs/M4.txt" 2>&1 || true
  grep -qE 'M4 PASS|m4h exited' "$WORK/logs/M4.txt" && break
done
check M4
leave_display

run M5 "$ENV $GL ./linker64 $BOX_PB/m5h"
run M6 "$ENV $GL $SDLV PB_FRAMES=120 ./linker64 $BOX_PB/m6"

# M7: PCM from SDL's disk driver -> anon pipe -> toybox nc -> OnnShell's AudioSink (a FIFO is SELinux-blocked)
a shell "am start -n com.xplsh.onnshell/.display.ZeroCopyActivity --ez launch false --ei rate 32000 --ei channels 2" >/dev/null
sleep 4
run M7 "cd $BOX_PB; { SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/proc/self/fd/3 LD_LIBRARY_PATH=$BOX_PB ./linker64 $BOX_PB/m7 3>&1 1>&2; } | toybox nc 127.0.0.1 7332"
leave_display

run M8 "$ENV PB_SECS=5 ./linker64 $BOX_PB/m8"

echo
[ "$fails" = 0 ] && echo "ALL PASS (m1..m8)" || { echo "$fails step(s) failed"; exit 1; }
