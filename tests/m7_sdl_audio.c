/* M7: SDL2 audio end-to-end check. Plays a 3 s 440 Hz tone (32000 Hz S16 stereo, the sm64ex format)
 * through whatever SDL audio driver is selected. On the box, the "disk" driver writes real-time PCM to a FIFO:
 *   mkfifo /data/local/tmp/pb/aud 2>/dev/null
 *   nc 127.0.0.1 7332 < /data/local/tmp/pb/aud &
 *   SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=/data/local/tmp/pb/aud LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 /data/local/tmp/pb/m7
 * ZeroCopyActivity's AudioSink turns port 7332 into an AudioTrack. Expect a tone on the TV; prints "M7 PASS".
 * Build: aarch64-linux-android29-clang -O1 -DSDL_MAIN_HANDLED m7_sdl_audio.c -I<sdl>/include -o m7 -L<sdl>/lib -lSDL2 -ldl -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "SDL.h"

static double phase;
static void fill(void *u, Uint8 *stream, int len)
{
    (void)u; Sint16 *s = (Sint16 *)stream;
    for (int i = 0; i < len / 4; i++) {
        Sint16 v = (Sint16)(sin(phase) * 8000); s[2 * i] = s[2 * i + 1] = v;
        phase += 2 * M_PI * 440 / 32000; if (phase > 2 * M_PI) phase -= 2 * M_PI;
    }
}

int main(void)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError()); return 1; }
    int queue = getenv("PB_QUEUE") != NULL; /* PB_QUEUE=1: SDL_QueueAudio push API, the way sm64ex feeds audio */
    SDL_AudioSpec want = { .freq = 32000, .format = AUDIO_S16, .channels = 2, .samples = 512, .callback = queue ? NULL : fill }, have;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "FAIL: SDL_OpenAudioDevice: %s\n", SDL_GetError()); return 1; }
    printf("audio driver: %s  %d Hz %d ch fmt 0x%x samples %d\n", SDL_GetCurrentAudioDriver(), have.freq, have.channels, have.format, have.samples);
    SDL_PauseAudioDevice(dev, 0);
    Uint32 t0 = SDL_GetTicks();
    if (!queue) SDL_Delay(3000);
    else { /* sm64ex audio_sdl2.c: top the queue up to ~2 periods each tick */
        Uint8 chunk[2048];
        while (SDL_GetTicks() - t0 < 3000) {
            while (SDL_GetQueuedAudioSize(dev) < 2 * sizeof chunk) { fill(NULL, chunk, sizeof chunk); SDL_QueueAudio(dev, chunk, sizeof chunk); }
            SDL_Delay(5);
        }
    }
    SDL_CloseAudioDevice(dev);
    printf("3 s tone took %u ms wall\nM7 PASS\n", SDL_GetTicks() - t0);
    SDL_Quit();
    return 0;
}
