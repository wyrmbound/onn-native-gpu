/* M8: SDL2 input check. Lists joysticks/game controllers seen through evdev (shell is in group input) and prints
 * button/axis/key events for PB_SECS seconds (default 15), then "M8 PASS" if at least one device was found.
 * Run: LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 /data/local/tmp/pb/m8   (SDL_VIDEODRIVER unset: no video needed)
 * Build: aarch64-linux-android29-clang -O1 -DSDL_MAIN_HANDLED m8_sdl_input.c -I<sdl>/include -o m8 -L<sdl>/lib -lSDL2 -ldl -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include "SDL.h"

int main(void)
{
    SDL_SetMainReady();
    int secs = getenv("PB_SECS") ? atoi(getenv("PB_SECS")) : 15;
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError()); return 1; }
    int n = SDL_NumJoysticks();
    printf("joysticks: %d\n", n);
    for (int i = 0; i < n; i++) {
        SDL_Joystick *j = SDL_JoystickOpen(i);
        printf("  %d: %s  axes %d buttons %d hats %d  controller:%s\n", i, SDL_JoystickNameForIndex(i),
               j ? SDL_JoystickNumAxes(j) : -1, j ? SDL_JoystickNumButtons(j) : -1, j ? SDL_JoystickNumHats(j) : -1,
               SDL_IsGameController(i) ? "yes" : "no (needs SDL_GAMECONTROLLERCONFIG mapping)");
        char guid[33]; SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
        printf("  guid %s\n", guid);
        if (SDL_IsGameController(i)) {
            SDL_GameController *gc = SDL_GameControllerOpen(i);
            char *m = gc ? SDL_GameControllerMapping(gc) : NULL;
            printf("  mapping %s\n", m ? m : "(none)"); SDL_free(m);
            /* resting values: a trigger above sm64ex's AXIS_THRESHOLD (7680) at rest reads as held */
            SDL_Delay(300); SDL_GameControllerUpdate();
            if (gc) printf("  at rest: lefttrigger %d righttrigger %d  leftx %d lefty %d rightx %d righty %d\n",
                           SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT), SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT),
                           SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX), SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
                           SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX), SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));
        }
    }
    Uint32 end = SDL_GetTicks() + secs * 1000; int events = 0;
    SDL_Event e;
    while (SDL_GetTicks() < end) {
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: printf("joy %d button %d %s\n", e.jbutton.which, e.jbutton.button, e.type == SDL_JOYBUTTONDOWN ? "down" : "up"); events++; break;
            case SDL_JOYAXISMOTION: if (abs(e.jaxis.value) > 16000) { printf("joy %d axis %d %d\n", e.jaxis.which, e.jaxis.axis, e.jaxis.value); events++; } break;
            case SDL_JOYHATMOTION: printf("joy %d hat %d 0x%x\n", e.jhat.which, e.jhat.hat, e.jhat.value); events++; break;
            case SDL_CONTROLLERBUTTONDOWN: printf("pad %d %s down\n", e.cbutton.which, SDL_GameControllerGetStringForButton(e.cbutton.button)); events++; break;
            case SDL_JOYDEVICEADDED: printf("device added %d\n", e.jdevice.which); break;
            }
        }
        SDL_Delay(10);
    }
    printf("%d events in %d s\n%s\n", events, secs, n > 0 ? "M8 PASS" : "M8 FAIL: no joystick found (check /dev/input perms, group input)");
    SDL_Quit();
    return n > 0 ? 0 : 2;
}
