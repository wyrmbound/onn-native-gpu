#!/bin/sh
# 20-bionic-bundle.sh: the arm64 bionic runtime the box lacks, taken from an Android 14 arm64 GSI.
# Output $WORK/bundle: linker64, libc/libm/libdl from the runtime APEX, plus liblog, libz, libnativewindow,
# libsync (what the ICD, EGL and gallium need) and their whole DT_NEEDED closure (the graphics HAL client
# libs: libui, mapper/allocator, libbinder, ...). Fails if any NEEDED name does not resolve.
# Reads the images with debugfs (ext4) or fsck.erofs (erofs): no mounting, no root.
set -eu
. "$(dirname "$0")/common.sh"
need_shim
: "${GSI_ZIP:?set GSI_ZIP in kit.env}"
[ -f "$GSI_ZIP" ] || die "GSI_ZIP=$GSI_ZIP not found"

G=$WORK/gsi
mkdir -p "$G"

# fs_get IMG PATH OUT: copy file or folder PATH out of an ext4/erofs image to OUT
fs_get() {
  if file -b "$1" | grep -qi erofs; then
    [ -d "$1.x" ] || fsck.erofs --extract="$1.x" "$1" >/dev/null
    rm -rf "$3"; cp -a "$1.x$2" "$3"
  elif debugfs -R "stat $2" "$1" 2>/dev/null | grep -q 'Type: directory'; then
    rm -rf "$3"; mkdir -p "$3.tmp"
    debugfs -R "rdump $2 $3.tmp" "$1" >/dev/null 2>&1
    mv "$3.tmp/$(basename "$2")" "$3"; rmdir "$3.tmp"
  else
    rm -f "$3"; debugfs -R "dump $2 $3" "$1" >/dev/null 2>&1
  fi
  [ -e "$3" ] || die "could not read $2 from $1"
}

say "system image from $(basename "$GSI_ZIP")"
if [ ! -f "$G/system.raw.img" ]; then
  unzip -o "$GSI_ZIP" system.img -d "$G" >/dev/null
  if file -b "$G/system.img" | grep -qi sparse; then simg2img "$G/system.img" "$G/system.raw.img"; rm -f "$G/system.img"
  else mv "$G/system.img" "$G/system.raw.img"; fi
fi
file -b "$G/system.raw.img"

say "system/lib64 and the runtime APEX"
fs_get "$G/system.raw.img" /system/lib64 "$G/sys-lib64"
AP=""
for a in com.android.runtime.apex com.android.runtime.capex; do
  if debugfs -R "stat /system/apex/$a" "$G/system.raw.img" 2>/dev/null | grep -q Type: || [ -e "$G/system.raw.img.x/system/apex/$a" ]; then AP=$a; fi
done
[ -n "$AP" ] || die "no com.android.runtime APEX in the GSI"
fs_get "$G/system.raw.img" "/system/apex/$AP" "$G/$AP"
if [ "$AP" = com.android.runtime.capex ]; then
  unzip -p "$G/$AP" original_apex > "$G/runtime.apex"
else
  mv "$G/$AP" "$G/runtime.apex"
fi
unzip -o "$G/runtime.apex" apex_payload.img -d "$G" >/dev/null
rm -rf "$G/apex_payload.img.x"
fs_get "$G/apex_payload.img" /bin/linker64 "$G/rt-linker64"
fs_get "$G/apex_payload.img" /lib64 "$G/rt-lib64"
echo "system lib64: $(ls "$G/sys-lib64" | wc -l) entries, APEX lib64: $(ls "$G/rt-lib64" "$G/rt-lib64/bionic" | wc -l) entries"

say "bundle seeds"
rm -rf "$BUNDLE"; mkdir -p "$BUNDLE"
SRC="$G/rt-lib64/bionic $G/rt-lib64 $G/sys-lib64"   # APEX first: system/lib64's libc.so is a symlink into /apex
find_lib() { for d in $SRC; do [ -f "$d/$1" ] && { echo "$d/$1"; return; }; done; return 0; }
cp "$G/rt-linker64" "$BUNDLE/linker64"
for l in libc.so libm.so libdl.so liblog.so libz.so libnativewindow.so libsync.so; do
  s=$(find_lib "$l"); [ -n "$s" ] || die "seed $l not in the GSI"
  cp -L "$s" "$BUNDLE/"; echo "+ $l"
done

say "NEEDED closure"
missing=""
changed=1
while [ "$changed" = 1 ]; do
  changed=0
  for f in "$BUNDLE"/*; do
    for n in $(needed "$f"); do
      [ -e "$BUNDLE/$n" ] && continue
      s=$(find_lib "$n")
      if [ -n "$s" ]; then cp -L "$s" "$BUNDLE/"; echo "+ $n  <- ${s#$G/}"; changed=1
      else case " $missing " in *" $n "*) ;; *) missing="$missing $n";; esac; fi
    done
  done
done
[ -z "$missing" ] || die "unresolved NEEDED:$missing"
for f in "$BUNDLE"/*; do
  "$BIN/llvm-readelf" -h "$f" | grep -q AArch64 || die "$(basename "$f") is not an arm64 ELF"
done
echo "OK bundle: $(ls "$BUNDLE" | wc -l) files in $BUNDLE, every NEEDED resolves, all arm64"
