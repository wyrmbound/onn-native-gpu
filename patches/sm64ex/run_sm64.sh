#!/system/bin/sh
# M9: full sm64ex. Video via ONNM4 dma-buf present; audio via anon pipe (SELinux blocks a FIFO):
# the disk driver writes PCM to /proc/self/fd/3, which 3>&1 wires to the pipe into toybox nc -> AudioSink.
# GL env matches m5h's winning combo; SDL_VIDEO_*_DRIVER point SDL's dlopen at our libs.
PB=/data/local/tmp/pb
cd "$PB" || exit 1
mkdir -p "$PB/cfg/sm64ex"
# Seed the window to the 1280x720 present FBOs (default 640x480 would draw into the bottom-left quarter).
C="$PB/cfg/sm64ex/sm64config.txt"
if [ -f "$C" ]; then
  sed -i -e 's/^window_w .*/window_w 1280/' -e 's/^window_h .*/window_h 720/' \
         -e 's/^fullscreen .*/fullscreen false/' -e 's/^skip_intro .*/skip_intro true/' "$C"
else
  printf 'fullscreen false\nwindow_w 1280\nwindow_h 720\nskip_intro true\n' > "$C"
fi
echo "[run_sm64] config: $(grep -E '^(window_w|window_h|fullscreen|skip_intro) ' "$C" | tr '\n' ' ')" 1>&2
echo "[run_sm64] env: ONNM4=1 SDL_VIDEODRIVER=offscreen SDL_VIDEO_GL_DRIVER=$PB/libGLESv2.so SDL_VIDEO_EGL_DRIVER=$PB/libEGL.so SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/proc/self/fd/3 MESA_LOADER_DRIVER_OVERRIDE=zink ZINK_VK_ICD=$PB/libvulkan_panfrost.so HOME=$PB/cfg XDG_DATA_HOME=$PB/cfg LD_LIBRARY_PATH=$PB (NO LIBGL_ALWAYS_SOFTWARE, NO GALLIUM_DRIVER)" 1>&2
: > "$PB/sm64.log"
# DS4 on the box's hid-playstation driver (evdev version 0x8000) has no exact SDL 2.30 db entry, so SDL fell back to the
# old hid-sony layout (right stick read as triggers -> Mario crouches). Give it the hid-playstation layout.
export SDL_GAMECONTROLLERCONFIG='05009b514c050000cc09000000800000,PS4 Controller,a:b0,b:b1,x:b3,y:b2,back:b8,guide:b10,start:b9,leftshoulder:b4,rightshoulder:b5,leftstick:b11,rightstick:b12,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,platform:Linux,'
{ env ONNM4=1 SDL_VIDEODRIVER=offscreen SDL_VIDEO_GL_DRIVER="$PB/libGLESv2.so" SDL_VIDEO_EGL_DRIVER="$PB/libEGL.so" \
      SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/proc/self/fd/3 \
      MESA_LOADER_DRIVER_OVERRIDE=zink ZINK_VK_ICD="$PB/libvulkan_panfrost.so" \
      HOME="$PB/cfg" XDG_DATA_HOME="$PB/cfg" LD_LIBRARY_PATH="$PB" \
      LD_PRELOAD="$PB/libonngrab.so" \
      "$PB/linker64" "$PB/sm64" --skip-intro 3>&1 1>>"$PB/sm64.log" 2>&1; } | toybox nc 127.0.0.1 7332
