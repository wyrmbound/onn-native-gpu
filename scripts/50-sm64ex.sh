#!/bin/sh
# 50-sm64ex.sh: sm64ex (nightly, pinned) with the kit's arm64 bionic + GLES2 port, built as PIE.
# Needs YOUR OWN US ROM at $WORK/sm64ex/baserom.us.z64. This script never downloads, copies or moves it;
# the game binary it builds contains the game's assets, so it stays local (never commit or publish it).
# First run clones and patches, then stops and asks for the ROM if it is not there yet.
set -eu
. "$(dirname "$0")/common.sh"
need_shim

SM64EX_URL=https://github.com/sm64pc/sm64ex.git
SM64EX_SHA=d7ca2c04364a6dd0dac58b47151e04e26887e6f0   # nightly as of 2024-12-17, what the patch was made on
ROM_SHA1=9bef1128717f958171a4afac3ed78ee2bb4e86ce
SM=$WORK/sm64ex
[ -f "$PB/libSDL2-2.0.so.0" ] && [ -f "$PB/libGLESv2.so" ] || die "run 30-mesa.sh and 40-sdl.sh first"

say "sm64ex @ $SM64EX_SHA"
[ -d "$SM/.git" ] || { git init -q "$SM"; git -C "$SM" remote add origin "$SM64EX_URL"; }
git -C "$SM" fetch -q --depth 1 origin "$SM64EX_SHA"
git -C "$SM" rev-parse -q --verify HEAD >/dev/null || git -C "$SM" checkout -q --detach "$SM64EX_SHA"
P=$KIT/patches/sm64ex/0001-onn-arm64-bionic-port.patch
if git -C "$SM" apply --reverse --check "$P" 2>/dev/null; then echo "kit patch already applied"
else git -C "$SM" apply "$P"; echo "applied $(basename "$P")"; fi

say "ROM check"
ROM=$SM/baserom.us.z64
if [ ! -f "$ROM" ]; then
  echo "Put your own US Super Mario 64 ROM at:"
  echo "  $ROM"
  echo "(sha1 $ROM_SHA1), then run this script again. The kit never supplies, downloads or copies a ROM."
  exit 2
fi
got=$(sha1sum "$ROM" | cut -d' ' -f1)
[ "$got" = "$ROM_SHA1" ] || die "baserom.us.z64 sha1 is $got, want $ROM_SHA1 (US, big-endian .z64)"
echo "baserom.us.z64 sha1 OK"

say "build (log $WORK/logs/sm64ex.log)"
mkdir -p "$SM/lib"
ln -sf "$PB/libSDL2-2.0.so.0" "$SM/lib/libSDL2.so"
ln -sf "$PB/libGLESv2.so" "$SM/lib/libGLESv2.so"
ln -sf "$PB/libEGL.so" "$SM/lib/libEGL.so"
cat > "$SM/onn-sdl2-config" <<EOF
#!/bin/sh
case "\$1" in
  --cflags) echo "-I$SDL_SRC/include -D_REENTRANT" ;;
  --libs|--static-libs) echo "-L$SM/lib -lSDL2" ;;
  --version) echo "$SDL_VER" ;;
esac
EOF
chmod 755 "$SM/onn-sdl2-config"
# NO_PIE=0: the bundled linker64 only runs ET_DYN. make does not track flag changes: if you change
# USE_GLES or the toolchain, delete $SM/build/us_pc first.
EXE=$SM/build/us_pc/sm64.us.f3dex2e
rm -f "$EXE"
PATH="$BIN:$PATH" make -C "$SM" VERSION=us USE_GLES=1 TARGET_ARCH=armv8-a NO_PIE=0 \
  CC="$BIN/onn-cc" CXX="$BIN/onn-cc" SDLCONFIG="$SM/onn-sdl2-config" -j"$JOBS" > "$WORK/logs/sm64ex.log" 2>&1 \
  || { grep -m10 -E 'error:|undefined reference' "$WORK/logs/sm64ex.log"; tail -n 15 "$WORK/logs/sm64ex.log"; die "sm64ex make failed"; }
"$BIN/llvm-readelf" -h "$EXE" | grep -q 'DYN' || die "game is not PIE (ET_DYN); linker64 would refuse it"
echo "NEEDED: $(needed "$EXE" | tr '\n' ' ')"
cp "$EXE" "$PB/sm64"
cp "$KIT/patches/sm64ex/run_sm64.sh" "$PB/run_sm64.sh"
ls -l "$PB/sm64" "$PB/run_sm64.sh"
echo "OK sm64ex: $PB/sm64 (local only: contains game assets)"
