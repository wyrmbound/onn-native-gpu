# sm64ex for the onn 4K Pro

**Use `scripts/50-sm64ex.sh`**: it clones sm64ex, applies `0001-onn-arm64-bionic-port.patch` and builds with the
kit's toolchain from `scripts/kit.env`. Bring your own US ROM; the script refuses to run without it and never copies it.

What the patch changes, and why: `USE_GLES=1` defines `USE_GLES` and links `-lGLESv2` outside the Raspberry Pi target;
the build tools are built with the host compiler; `-lpthread` is dropped (bionic folds it into libc); `audio_sdl2.c`
gets `<stdio.h>`; window and context creation fail loudly; under `USE_GLES` the window is created at 0,0 without
SHOWN/RESIZABLE (the offscreen driver rejects them), the EGL config asks for no depth (the present module's FBOs carry
the depth buffer), and the GL entry points are resolved through `SDL_GL_GetProcAddress` (`gfx_gles_loader.c`).
Link it as PIE (`NO_PIE=0`): Android's linker refuses `ET_EXEC`.

`run_sm64.sh` is the launcher pushed to the box: it seeds a 1280x720 config, sets the DualShock 4 mapping, sends audio
through a pipe to OnnShell and presents into OnnShell's buffers.
