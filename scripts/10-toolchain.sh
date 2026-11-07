#!/bin/sh
# 10-toolchain.sh: Android NDK r27d sysroot + a clang shim in $WORK/ndk-shim.
#
# The shim is the host's clang-18/lld-18 pointed at the NDK's sysroot and clang resource dir (r27d ships
# clang 18, so the builtins match), laid out like an NDK so panvk-kbase-android's scripts accept it as
# ANDROID_NDK_ROOT. It also writes onn-cc, the compiler SDL and sm64ex are built with: the shim plus
# -U__ANDROID__ -DUSE_OZONE, so they take their Linux code paths on bionic.
#
# aarch64 host (what we validated, WSL on Windows ARM): Google ships no linux-aarch64 NDK, so the
#   shim is the only way. The NDK zip's linux-x86_64 folder is used for its sysroot and builtins only.
# x86_64 host: the same shim works unchanged (the sysroot is host-independent), and that is what this
#   script builds. The NDK's own prebuilt/linux-x86_64/bin clang would also work but is not what we
#   validated. Differences: the shim folder is named linux-x86_64 (the panvk scripts look for
#   linux-$(uname -m)), and Mesa's host tools (mesa_clc etc., 30-mesa.sh) are built for x86_64.
#   Package names in the apt line below are the same on both.
set -eu
. "$(dirname "$0")/common.sh"

NDK_VER=r27d
NDK_SHA1=22105e410cf29afcf163760cc95522b9fb981121

say "host tools"
miss=""
for c in clang-18 clang++-18 lld-18 llvm-ar-18 llvm-strip-18 llvm-readelf-18 llvm-nm-18 llvm-objdump-18 \
         git curl unzip sha1sum sha256sum file python3 meson ninja cmake pkg-config bison flex make patch \
         glslangValidator simg2img debugfs; do
  command -v "$c" >/dev/null 2>&1 || miss="$miss $c"
done
python3 -c 'import mako, yaml, ply' 2>/dev/null || miss="$miss python3-mako/yaml/ply"
if command -v meson >/dev/null 2>&1 && ! python3 -c "import sys; v=tuple(int(x) for x in sys.argv[1].split('.')[:2]); sys.exit(v < (1, 4))" "$(meson --version)"; then
  miss="$miss meson>=1.4(have $(meson --version))"
fi
if [ -n "$miss" ]; then
  echo "missing:$miss"
  cat <<'EOF'
On Debian/Ubuntu 24.04:
  sudo apt-get install -y build-essential git curl wget unzip xz-utils file python3 python3-pip python3-mako \
    python3-yaml python3-ply bison flex cmake ninja-build pkg-config clang-18 lld-18 llvm-18 llvm-18-dev \
    libclang-18-dev libllvmspirvlib-18-dev spirv-tools glslang-tools libelf-dev zlib1g-dev libexpat1-dev \
    libx11-dev libx11-xcb-dev libxcb1-dev libxshmfence-dev android-sdk-libsparse-utils e2fsprogs erofs-utils
  pip install --break-system-packages 'meson>=1.4'     # Ubuntu 24.04's meson is 1.3; Mesa needs 1.4+
EOF
  exit 1
fi
echo "all present (meson $(meson --version), $(clang-18 --version | head -n1))"

say "NDK $NDK_VER sysroot"
if [ -z "${NDK:-}" ]; then
  NDK=$WORK/android-ndk-$NDK_VER
  P=$NDK/toolchains/llvm/prebuilt/linux-x86_64
  if [ ! -f "$P/sysroot/usr/include/vulkan/vulkan.h" ]; then
    Z=$WORK/android-ndk-$NDK_VER-linux.zip
    if ! echo "$NDK_SHA1  $Z" | sha1sum -c - >/dev/null 2>&1; then
      echo "downloading android-ndk-$NDK_VER-linux.zip (664 MB) ..."
      curl -fL --retry 5 -C - -o "$Z" "https://dl.google.com/android/repository/android-ndk-$NDK_VER-linux.zip"
      echo "$NDK_SHA1  $Z" | sha1sum -c - || die "NDK zip sha1 mismatch; delete $Z and retry"
    fi
    echo "extracting sysroot + clang resource dir only ..."
    unzip -q -o "$Z" "android-ndk-$NDK_VER/toolchains/llvm/prebuilt/linux-x86_64/sysroot/*" \
                     "android-ndk-$NDK_VER/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/*" -d "$WORK"
  fi
fi
P=$NDK/toolchains/llvm/prebuilt/linux-x86_64
[ -f "$P/sysroot/usr/include/vulkan/vulkan.h" ] || die "NDK=$NDK has no $P/sysroot (want an extracted NDK $NDK_VER)"
CLANGVER=$(ls "$P/lib/clang" | head -n1)
[ "$CLANGVER" = 18 ] || echo "warning: NDK clang resource dir is $CLANGVER, the shim uses host clang-18 (validated with r27d = 18)"
ls "$P/lib/clang/$CLANGVER/lib/linux/libclang_rt.builtins-aarch64-android.a" >/dev/null
echo "sysroot: $P/sysroot"

say "clang shim -> $BIN"
mkdir -p "$BIN"
for api in 29 35; do
  for cc in clang clang++; do
    stdlib=""; [ "$cc" = clang++ ] && stdlib=" -stdlib=libc++"
    # -Qunused-arguments: meson's compile-only probes use -Werror=unused-command-line-argument and -fuse-ld is
    # unused there; without it probes fail, Mesa misdetects struct timespec and the build breaks.
    cat > "$BIN/aarch64-linux-android$api-$cc" <<EOF
#!/bin/sh
exec $cc-18 --target=aarch64-linux-android$api --sysroot=$P/sysroot -resource-dir=$P/lib/clang/$CLANGVER -rtlib=compiler-rt -unwindlib=libunwind -fuse-ld=lld-18 -Qunused-arguments$stdlib "\$@"
EOF
  done
done
cat > "$BIN/onn-cc" <<EOF
#!/bin/sh
exec "$BIN/aarch64-linux-android29-clang" -U__ANDROID__ -DUSE_OZONE "\$@"
EOF
chmod 755 "$BIN"/aarch64-linux-android* "$BIN/onn-cc"
for t in llvm-ar llvm-strip llvm-readelf llvm-nm llvm-objdump; do ln -sf "$(command -v $t-18)" "$BIN/$t"; done
ls "$BIN"

say "smoke test"
T=$WORK/logs/smoke
printf 'int main(void){return 0;}\n' > "$T.c"
"$BIN/aarch64-linux-android29-clang" "$T.c" -o "$T"
"$BIN/llvm-readelf" -h "$T" | grep -q 'AArch64' || die "smoke binary is not AArch64"
"$BIN/llvm-readelf" -h "$T" | grep -q 'DYN' || die "smoke binary is not PIE (ET_DYN)"
echo "aarch64 PIE OK, NEEDED: $(needed "$T" | tr '\n' ' ')"
echo "OK toolchain: ANDROID_NDK_ROOT=$SHIM"
