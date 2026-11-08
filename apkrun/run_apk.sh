#!/system/bin/sh
# Run an Android SDL2 or SDL3 port's own arm64 code through apkrun.
# Usage: sh run_apk.sh <game dir> [game lib, default libmain.so] [game args...]
# The game dir holds the APK's arm64 libs except libSDL2.so/libSDL3.so; libSDL2.so there is a link to our SDL in pb,
# libSDL3.so is found in pb directly.
# Start ZeroCopyActivity first (--ez launch false, --ei rate/channels matching the game's audio).
PB=/data/local/tmp/pb
G=${1:?game dir}; shift
# entry lib: from apkinstall's game.json unless given
if [ $# -gt 0 ] && [ "${1%.so}" != "$1" ]; then LIB=$1; shift; else LIB=$(sed -n 's/.*"entry": "\([^"]*\)".*/\1/p' "$G/game.json"); fi
cd "$G" || exit 1
[ -e "$G/libSDL2.so" ] || ln -s "$PB/libSDL2-2.0.so.0" "$G/libSDL2.so"
mkdir -p "$G/data"
: > "$G/game.log"
# What an Android app has loaded before its game lib (onnandroid.c): emutls, __cxa_demangle and no-op window calls
CXX=; [ -f "$PB/libonnandroid.so" ] && CXX=":$PB/libonnandroid.so"
# An Android app loads its own libc++_shared.so from Java before the game lib, and a game lib may lean on that without
# naming it (DKR-R takes __cxa_init_primary_exception from it). Load it first here too.
[ -f "$G/libc++_shared.so" ] && CXX="$CXX:$G/libc++_shared.so"
# A port linked against the system libGLESv3.so got an empty stand-in for it (the system one is 32-bit); its GL calls
# then had nothing to bind to (SoH, 2Ship). Preload our Mesa GLES, which has the GLES 3 calls, for those.
[ -f "$G/libGLESv3.so" ] && CXX="$CXX:$PB/libGLESv2.so"
export SDL_GAMECONTROLLERCONFIG='05009b514c050000cc09000000800000,PS4 Controller,a:b0,b:b1,x:b3,y:b2,back:b8,guide:b10,start:b9,leftshoulder:b4,rightshoulder:b5,leftstick:b11,rightstick:b12,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,platform:Linux,'
# Android has OpenGL ES 3; zink on this driver reports ES 2 because one ES 3 feature is missing (transform feedback).
# A port built for ES 3 (the ones with the libGLESv3.so stand-in) cannot make its window at all without ES 3 configs,
# and SoH and 2Ship draw correctly with them, so those ports get ES 3.0 announced.
ES3=; [ -f "$G/libGLESv3.so" ] && ES3=3.0
# ZINK_DEBUG=noreorder (set below): zink moves texture uploads into a second command buffer that runs before the
# frame's own. On this driver that loses the GPU (CSF exception 0x88, then "zink: DEVICE LOST"): within two seconds
# in Super Metroid every time, at random in Tyrian, both games that upload a texture every frame. With reordering
# off Super Metroid ran 9780 frames clean. Workaround, not a fix: the fault is in how PanVK runs those buffers.
# Guard: a game that loses the GPU ("zink: DEVICE LOST", seen on Tyrian and Super Metroid) keeps running and eats
# memory, about 200 MB a second, until the whole box stops answering. Stop the game when that starts: the message
# shows in its log, or free memory falls under 350 MB (a working game leaves three times that).
( sleep 3
  while P=$(pidof linker64); do
    M=$(sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' /proc/meminfo)
    if [ "$M" -lt 350000 ] || grep -q 'DEVICE LOST' "$G/game.log" 2>/dev/null; then
      echo "run_apk: stopped the game (free memory $M kB): the GPU was lost or memory ran out" >> "$G/game.log"
      kill -9 $P; break
    fi
    sleep 1
  done ) >/dev/null 2>&1 &
# SDL3 renamed the hints, so each is set under both names: SDL2 name first, SDL3 name second
# env goes AFTER nohup/sh: Android's 32-bit tools must never inherit our 64-bit LD_LIBRARY_PATH
{ env ONNM4=1 SDL_VIDEODRIVER=offscreen SDL_VIDEO_DRIVER=offscreen SDL_VULKAN_LIBRARY="$PB/libvulkan.so" \
      SDL_VIDEO_GL_DRIVER="$PB/libGLESv2.so" SDL_OPENGL_LIBRARY="$PB/libGLESv2.so" \
      SDL_VIDEO_EGL_DRIVER="$PB/libEGL.so" SDL_EGL_LIBRARY="$PB/libEGL.so" \
      SDL_AUDIODRIVER=disk SDL_AUDIO_DRIVER=disk SDL_DISKAUDIOFILE=/proc/self/fd/3 SDL_AUDIO_DISK_OUTPUT_FILE=/proc/self/fd/3 \
      MESA_LOADER_DRIVER_OVERRIDE=zink ZINK_VK_ICD="$PB/libvulkan_panfrost.so" ${ES3:+MESA_GLES_VERSION_OVERRIDE=$ES3} \
      ZINK_DEBUG=noreorder \
      APKRUN_DATA="$G/data" HOME="$G/data" LD_LIBRARY_PATH="$G:$PB" \
      LD_PRELOAD="$PB/libonngrab.so$CXX" \
      "$PB/linker64" "$PB/apkrun" "$G/$LIB" "$@" 3>&1 1>>"$G/game.log" 2>&1; } | toybox nc 127.0.0.1 7332
