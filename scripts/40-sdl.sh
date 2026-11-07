#!/bin/sh
# 40-sdl.sh: SDL 2.30.11 for arm64 bionic, built as "Linux" (onn-cc's -U__ANDROID__), with the kit's patch:
# offscreen video on EGL surfaceless + the zero-copy present into OnnShell's buffers, disk audio driver on.
# Also builds the SDL harnesses m6 (GLES), m7 (audio), m8 (gamepad). Outputs staged in $WORK/pb.
# Re-runnable: the source is re-extracted and re-patched, the build dir rebuilt (about a minute).
set -eu
. "$(dirname "$0")/common.sh"
need_shim

SDL_SHA256=8b8d4aef2038533da814965220f88f77d60dfa0f32685f80ead65e501337da7f
TGZ=$WORK/SDL2-$SDL_VER.tar.gz
BLD=$WORK/build-sdl
LOG=$WORK/logs/sdl.log

say "SDL2-$SDL_VER source + patches/sdl"
if ! echo "$SDL_SHA256  $TGZ" | sha256sum -c - >/dev/null 2>&1; then
  curl -fL --retry 5 -o "$TGZ" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
  echo "$SDL_SHA256  $TGZ" | sha256sum -c - || die "SDL tarball sha256 mismatch"
fi
rm -rf "$SDL_SRC"
tar xzf "$TGZ" -C "$WORK"
for p in "$KIT"/patches/sdl/*.patch; do echo "patch $(basename "$p")"; patch -s -p1 -d "$SDL_SRC" < "$p"; done
ln -sfn . "$SDL_SRC/include/SDL2"   # so <SDL2/SDL.h> (what sm64ex includes) resolves to the flat include/

say "cmake + ninja (log $LOG)"
cat > "$WORK/bionic-aarch64.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $BIN/aarch64-linux-android29-clang)
set(CMAKE_CXX_COMPILER $BIN/aarch64-linux-android29-clang++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
rm -rf "$BLD"
cmake -S "$SDL_SRC" -B "$BLD" -G Ninja --toolchain "$WORK/bionic-aarch64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-U__ANDROID__ -DUSE_OZONE" -DCMAKE_CXX_FLAGS="-U__ANDROID__ -DUSE_OZONE" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL2_DISABLE_SDL2MAIN=ON \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VIVANTE=OFF -DSDL_RPI=OFF -DSDL_DIRECTFB=OFF \
  -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_OFFSCREEN=ON \
  -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_SNDIO=OFF -DSDL_JACK=OFF -DSDL_OSS=OFF -DSDL_NAS=OFF \
  -DSDL_DISKAUDIO=ON -DSDL_DUMMYAUDIO=ON \
  -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_HIDAPI=OFF -DSDL_LIBICONV=OFF \
  -DSDL_PTHREADS=ON -DSDL_JOYSTICK=ON -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF \
  > "$LOG" 2>&1 || { tail -n 25 "$LOG"; die "SDL cmake configure failed"; }
ninja -j"$JOBS" -C "$BLD" >> "$LOG" 2>&1 || { grep -m10 -iE 'error:|undefined' "$LOG"; tail -n 15 "$LOG"; die "SDL ninja failed"; }
SO=$(find "$BLD" -name "libSDL2-2.0.so.0.*" -type f | head -n1)
[ -n "$SO" ] || die "no libSDL2-2.0.so.0.* produced"
cp "$SO" "$PB/libSDL2-2.0.so.0"; "$BIN/llvm-strip" "$PB/libSDL2-2.0.so.0"
grep -q SDL_DISKAUDIOFILE "$PB/libSDL2-2.0.so.0" || die "SDL built without the disk audio driver"
grep -q onnm4 "$PB/libSDL2-2.0.so.0" || grep -q ONNM4 "$PB/libSDL2-2.0.so.0" || die "SDL built without the onnm4 present module"
echo "libSDL2-2.0.so.0 NEEDED: $(needed "$PB/libSDL2-2.0.so.0" | tr '\n' ' ')"

say "test harnesses m6..m8"
for t in 6:m6_sdl_test 7:m7_sdl_audio 8:m8_sdl_input; do
  "$BIN/aarch64-linux-android29-clang" -O1 -DSDL_MAIN_HANDLED "$KIT/tests/${t#*:}.c" -I"$SDL_SRC/include" \
    -o "$PB/m${t%%:*}" "$PB/libSDL2-2.0.so.0" -ldl -lm
done
ls -l "$PB/libSDL2-2.0.so.0" "$PB/m6" "$PB/m7" "$PB/m8"
echo "OK sdl: libSDL2-2.0.so.0 + m6..m8 staged in $PB"
