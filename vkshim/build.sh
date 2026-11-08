#!/bin/bash
# Builds libvulkan.so (the onn Vulkan loader shim) and vktest for arm64 bionic, same NDK shim clang as
# build-m2h.sh. Regenerates onn_vkshim_gen.c from Mesa's vk.xml, links onnm4_wsi.c too if it is here,
# checks the exports, runs a host-side dry run of the extension rules (hostcheck.c + fake ICD), and stages
# libvulkan.so, vktest, wsitest (and apkrun/apkrun.sdl3android) with their sha256 back into this folder.
# Run (Linux or WSL, as the user that owns the Mesa tree): bash vkshim/build.sh. NDK_LIBVULKAN must point at an NDK sysroot libvulkan.so (API 29+).
set -eu
T=/opt/ndk-shim/toolchains/llvm/prebuilt/linux-aarch64/bin
CC="$T/aarch64-linux-android29-clang"; [ -x "$CC" ] || CC="$T/clang"
M=/root/panvk-kbase-android/work/mesa
VKXML=${VKXML:-$M/src/vulkan/registry/vk.xml}
D=$(cd "$(dirname "$0")" && pwd)
W=/root/vkshim-build
rm -rf "$W"; mkdir -p "$W"; cd "$W"
cp "$D"/gen_vkshim.py "$D"/onn_vkshim.c "$D"/onn_vkshim.h "$D"/vktest.c "$D"/wsitest.c "$D"/hostcheck*.c .
WSI=""; [ -f "$D/onnm4_wsi.c" ] && { cp "$D/onnm4_wsi.c" .; WSI=onnm4_wsi.c; echo "linking WSI module onnm4_wsi.c"; }

python3 gen_vkshim.py "$VKXML" .
"$CC" -O2 -Wall -Werror -fPIC -shared -fvisibility=hidden -I"$M/include" -I. \
    -Wl,-soname,libvulkan.so -Wl,-Bsymbolic -Wl,--no-undefined -o libvulkan.so onn_vkshim.c $WSI -ldl
"$CC" -O2 -Wall -Werror -I"$M/include" -o vktest vktest.c ./libvulkan.so
"$CC" -O2 -Wall -DVK_NO_PROTOTYPES -I"$M/include" -o wsitest wsitest.c -ldl
A=$D/../apkrun
mkdir -p apkrun && cp "$A"/apkrun.c "$A"/jnishim.c "$A"/jnishim.h "$A"/jni.h apkrun/
"$CC" -O1 -rdynamic -Iapkrun apkrun/apkrun.c apkrun/jnishim.c -o apkrun.sdl3android -ldl

echo "--- host dry run (host cc, fake ICD) ---"
mkdir -p host
cc -O2 -Wall -fPIC -shared -I"$M/include" -o host/libhostcheck_icd.so hostcheck_icd.c
cc -O2 -Wall -fPIC -shared -fvisibility=hidden -I"$M/include" -I. -Wl,-soname,libvulkan.so -Wl,-Bsymbolic     -o host/libvulkan.so onn_vkshim.c $WSI -ldl
cc -O2 -Wall -I"$M/include" -o host/hostcheck hostcheck.c host/libvulkan.so -ldl
LD_LIBRARY_PATH=host ONN_VK_ICD="$W/host/libhostcheck_icd.so" HOSTCHECK_SURFACE=${HOSTCHECK_SURFACE:-1} host/hostcheck

echo "--- dynamic section ---"
"$T/llvm-readelf" -d libvulkan.so | grep -E 'NEEDED|SONAME'
"$T/llvm-readelf" -d vktest | grep NEEDED
echo "--- exports ---"
"$T/llvm-nm" -D --defined-only libvulkan.so | awk '$2=="T"{print $3}' | sort > exported.txt
echo "exported functions: $(wc -l < exported.txt) (vk*: $(grep -c '^vk' exported.txt))"
grep -v '^vk' exported.txt | sed 's/^/non-vk export: /'
MISS=$(comm -23 core13.txt exported.txt)
[ -z "$MISS" ] && echo "core 1.0-1.3: all $(wc -l < core13.txt) exported" || { echo "MISSING core commands:"; echo "$MISS"; exit 1; }
MISS=$(comm -23 exports.txt exported.txt); [ -z "$MISS" ] || { echo "MISSING generated exports:"; echo "$MISS"; exit 1; }
# NDK's libvulkan.so stub = the symbols an NDK-built port may import from the real Android loader
NDK=${NDK_LIBVULKAN:?set NDK_LIBVULKAN to an NDK sysroot aarch64 libvulkan.so}  # the shim toolchain has no stub
if [ -f "$NDK" ]; then
  "$T/llvm-nm" -D --defined-only "$NDK" | awk '{print $3}' | sed 's/@.*//' | grep '^vk' | sort -u > ndk.txt
  echo "NDK API $(basename "$(dirname "$NDK")") libvulkan.so exports $(wc -l < ndk.txt); missing from ours: $(comm -23 ndk.txt exported.txt | tr '\n' ' ')"
else echo "NDK libvulkan.so stub not readable here, export comparison skipped (set NDK_LIBVULKAN)"
fi

cp libvulkan.so vktest wsitest onn_vkshim_gen.c exports.txt core13.txt "$D/"
cp apkrun.sdl3android "$A/"
cd "$D" && sha256sum libvulkan.so vktest wsitest ../apkrun/apkrun.sdl3android | tee libvulkan.so.sha256
