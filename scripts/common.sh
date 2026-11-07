# Sourced by every numbered script: loads kit.env and the derived paths. Not run on its own.
KIT=$(cd "$(dirname "$0")/.." && pwd)
ENV_FILE=${KIT_ENV:-$KIT/scripts/kit.env}
[ -f "$ENV_FILE" ] || { echo "Missing $ENV_FILE: copy scripts/kit.env.example to scripts/kit.env and fill it in." >&2; exit 1; }
. "$ENV_FILE"
: "${WORK:?set WORK in kit.env}"
case "$WORK" in /path/to/*) echo "WORK in $ENV_FILE is still the example value." >&2; exit 1;; esac
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
JOBS=${JOBS:-$(nproc)}

SHIM=$WORK/ndk-shim                                         # NDK-shaped dir the panvk scripts accept as ANDROID_NDK_ROOT
BIN=$SHIM/toolchains/llvm/prebuilt/linux-$(uname -m)/bin     # aarch64-linux-android29-clang, llvm-readelf, onn-cc
PANVK=$WORK/panvk-kbase-android
BUNDLE=$WORK/bundle                                         # bionic runtime from the GSI (20)
PB=$WORK/pb                                                 # everything else pushed to the box (30, 40, 50)
BOX_PB=/data/local/tmp/pb
SDL_VER=2.30.11
SDL_SRC=$WORK/SDL2-$SDL_VER
mkdir -p "$PB" "$WORK/logs"

say() { printf '\n== %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
need_shim() { [ -x "$BIN/aarch64-linux-android29-clang" ] || die "no toolchain at $BIN; run scripts/10-toolchain.sh first"; }
# needed FILE: the DT_NEEDED names of an ELF, one per line
needed() { "$BIN/llvm-readelf" -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
