#!/bin/sh
# 30-mesa.sh: the PanVK Vulkan ICD and the GL stack (EGL surfaceless + GLES2 + zink) for the box.
# panvk-kbase-android at its pinned commit fetches Mesa at the commit in its sources.lock (shallow, ~100 MB),
# the kit's patches/mesa-csf-v10 become its csf-v10 series, apply-patches.sh applies profile g610-v10-csf,
# then two builds: build-android.sh (Vulkan ICD) and a second meson config for GL (options below).
# Also builds the Vulkan/EGL test harnesses m1..m5. Outputs are staged in $WORK/pb.
# Re-runnable: Mesa is reset to the pinned commit and re-patched each run; ninja rebuilds only what changed.
set -eu
. "$(dirname "$0")/common.sh"
need_shim

PANVK_URL=https://github.com/zenithblue-oss/panvk-kbase-android.git
PANVK_SHA=cf70e02b918e1864cbcd746a7fefa57b96c673ce
PROFILE=g610-v10-csf
API=29
LIBDRM_TAG=libdrm-2.4.125
export ANDROID_NDK_ROOT="$SHIM" PANVK_JOBS="$JOBS" CCACHE_DISABLE=1

say "panvk-kbase-android @ $PANVK_SHA"
[ -d "$PANVK/.git" ] || { git init -q "$PANVK"; git -C "$PANVK" remote add origin "$PANVK_URL"; }
git -C "$PANVK" fetch -q --depth 1 origin "$PANVK_SHA"
git -C "$PANVK" checkout -q -f --detach "$PANVK_SHA"
# We validated without the X11 WSI (nothing on the box would use it); upstream builds android,x11.
sed -i 's/-Dplatforms=android,x11 /-Dplatforms=android /' "$PANVK/scripts/build-android.sh"
grep -q -- '-Dplatforms=android ' "$PANVK/scripts/build-android.sh" || die "build-android.sh platforms line changed upstream"
rm -rf "$PANVK/patches/csf-v10"
cp -r "$KIT/patches/mesa-csf-v10" "$PANVK/patches/csf-v10"
echo "kit series: $(ls "$PANVK/patches/csf-v10" | tr '\n' ' ')"
[ "$(ls "$PANVK/patches/csf-v10" | grep -c '^0[01][0-9]-.*\.patch$')" = 12 ] || die "kit csf-v10 series is not 001-012"

say "Mesa per sources.lock"
REPO=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['mesaRepo'])" "$PANVK/sources.lock")
# Pre-create the repo so fetch-mesa.sh only does its depth-1 fetch instead of a full multi-GB clone.
[ -d "$PANVK/work/mesa/.git" ] || { git init -q "$PANVK/work/mesa"; git -C "$PANVK/work/mesa" remote add origin "$REPO"; }
sh "$PANVK/scripts/fetch-mesa.sh"

say "apply-patches --profile $PROFILE"
sh "$PANVK/scripts/apply-patches.sh" --profile "$PROFILE"

say "libdrm $LIBDRM_TAG (headers for Mesa util)"
D=$PANVK/work/libdrm
[ -d "$D/.git" ] || git clone -q --depth 1 -b "$LIBDRM_TAG" https://gitlab.freedesktop.org/mesa/drm.git "$D"
# libdrm's MAJOR_IN_SYSMACROS probe misses bionic's <sys/sysmacros.h> under the shim; always include it.
sed -i 's/^#ifdef MAJOR_IN_SYSMACROS$/#if 1 \/* onn kit: bionic has sys\/sysmacros.h *\//' "$D/xf86drm.c"
grep -q 'onn kit: bionic has sys/sysmacros.h' "$D/xf86drm.c" || die "libdrm sysmacros fix did not apply"

say "Vulkan ICD (build-android.sh, log $PANVK/build/android-bionic.log)"
sh "$PANVK/scripts/build-android.sh" --profile "$PROFILE" --api "$API"
ICD=$PANVK/dist/android-$PROFILE/libvulkan_panfrost.so
"$BIN/llvm-readelf" --dyn-symbols "$ICD" | grep -q vk_icdGetInstanceProcAddr || die "ICD lacks vk_icdGetInstanceProcAddr"
cp "$ICD" "$PB/"

say "GL: zink + EGL surfaceless + GLES2 (log $PANVK/build/android-gl-bionic.log)"
BDIR=$PANVK/build/android-gl-bionic
export PATH="$PANVK/build/host-tools/bin:$BIN:$PATH"
# same cross file build-android.sh made, so the GL build sees the same NDK-built deps
cp "$PANVK/build/android-bionic.cross.ini" "$BDIR.cross.ini"
RECONF=""; [ -f "$BDIR/build.ninja" ] && RECONF="--reconfigure"
meson setup $RECONF "$BDIR" "$PANVK/work/mesa" --cross-file "$BDIR.cross.ini" \
  -Dbuildtype=release -Dplatforms=android -Dandroid-stub=true -Dandroid-strict=false \
  -Dgallium-drivers=zink -Dvulkan-drivers= -Dpanfrost-kmds=kbase \
  -Dmesa-clc=system -Dprecomp-compiler=system -Dxlib-lease=disabled \
  -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Degl-native-platform=surfaceless -Dglvnd=disabled -Dshared-glapi=enabled \
  -Dglx=disabled -Dgbm=disabled -Dlibunwind=disabled -Dzstd=disabled -Dllvm=disabled \
  -Dcpp_link_args=-static-libstdc++ > "$BDIR.log" 2>&1 || { tail -n 25 "$BDIR.log"; die "GL meson setup failed"; }
ninja -j"$JOBS" -C "$BDIR" >> "$BDIR.log" 2>&1 || { grep -m10 -iE 'error:|undefined' "$BDIR.log"; tail -n 15 "$BDIR.log"; die "GL ninja failed"; }
for lib in libEGL.so libGLESv2.so libgallium_dri.so; do
  s=$(find "$BDIR" -name "$lib" -type f | head -n1)
  [ -n "$s" ] || die "GL build produced no $lib"
  cp "$s" "$PB/$lib"; "$BIN/llvm-strip" "$PB/$lib"
done

say "test harnesses m1..m5"
T=$WORK/tests; mkdir -p "$T"
glslangValidator -V --vn tri_vert_spv "$KIT/tests/tri.vert" -o "$T/tri_vert.h" >/dev/null
glslangValidator -V --vn tri_frag_spv "$KIT/tests/tri.frag" -o "$T/tri_frag.h" >/dev/null
CC="$BIN/aarch64-linux-android$API-clang"
"$CC" -O1 -DVK_NO_PROTOTYPES "$KIT/tests/m1_vk_clear.c" -o "$PB/m1" -ldl
for m in 2 3 4; do "$CC" -O1 -DVK_NO_PROTOTYPES -I"$T" "$KIT/tests/m${m}_helper.c" -o "$PB/m${m}h" -ldl -lm; done
"$CC" -O1 "$KIT/tests/m5_helper.c" -o "$PB/m5h" -ldl

ls -l "$PB/libvulkan_panfrost.so" "$PB/libEGL.so" "$PB/libGLESv2.so" "$PB/libgallium_dri.so" "$PB/m1" "$PB/m2h" "$PB/m3h" "$PB/m4h" "$PB/m5h"
echo "OK mesa: ICD + GL libs + m1..m5 staged in $PB"
