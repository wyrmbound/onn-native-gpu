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
export SDL_GAMECONTROLLERCONFIG='05009b514c050000cc09000000800000,PS4 Controller,a:b0,b:b1,x:b3,y:b2,back:b8,guide:b10,start:b9,leftshoulder:b4,rightshoulder:b5,leftstick:b11,rightstick:b12,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,platform:Linux,'
# SDL3 renamed the hints, so each is set under both names: SDL2 name first, SDL3 name second
# env goes AFTER nohup/sh: Android's 32-bit tools must never inherit our 64-bit LD_LIBRARY_PATH
{ env ONNM4=1 SDL_VIDEODRIVER=offscreen SDL_VIDEO_DRIVER=offscreen SDL_VULKAN_LIBRARY="$PB/libvulkan.so" \
      SDL_VIDEO_GL_DRIVER="$PB/libGLESv2.so" SDL_OPENGL_LIBRARY="$PB/libGLESv2.so" \
      SDL_VIDEO_EGL_DRIVER="$PB/libEGL.so" SDL_EGL_LIBRARY="$PB/libEGL.so" \
      SDL_AUDIODRIVER=disk SDL_AUDIO_DRIVER=disk SDL_DISKAUDIOFILE=/proc/self/fd/3 SDL_AUDIO_DISK_OUTPUT_FILE=/proc/self/fd/3 \
      MESA_LOADER_DRIVER_OVERRIDE=zink ZINK_VK_ICD="$PB/libvulkan_panfrost.so" \
      APKRUN_DATA="$G/data" HOME="$G/data" LD_LIBRARY_PATH="$G:$PB" \
      "$PB/linker64" "$PB/apkrun" "$G/$LIB" "$@" 3>&1 1>>"$G/game.log" 2>&1; } | toybox nc 127.0.0.1 7332
