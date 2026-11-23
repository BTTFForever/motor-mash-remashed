/* padtest: list SDL3 gamepads and print their state for a few seconds (x86 build).
   cl /nologo /I third_party\SDL3\SDL3-3.4.20\include padtest.c third_party\SDL3\SDL3-3.4.20\lib\x86\SDL3.lib */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 8, i, n = 0;
    SDL_JoystickID *ids;
    Uint64 end;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    printf("SDL %d\n", SDL_GetVersion());
    SDL_Delay(1500);
    SDL_UpdateGamepads();
    ids = SDL_GetGamepads(&n);
    printf("%d gamepad(s)\n", n);
    for (i = 0; i < n; i++)
        printf("  %u: %s (vid %04x pid %04x) type %d\n", (unsigned)ids[i], SDL_GetGamepadNameForID(ids[i]),
               SDL_GetGamepadVendorForID(ids[i]), SDL_GetGamepadProductForID(ids[i]), (int)SDL_GetGamepadTypeForID(ids[i]));
    if (n > 0) {
        SDL_Gamepad *g = SDL_OpenGamepad(ids[0]);
        end = SDL_GetTicks() + secs * 1000;
        while (SDL_GetTicks() < end) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                    printf("button %s down\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)e.gbutton.button));
            }
            printf("LX %6d LY %6d RX %6d RY %6d LT %6d RT %6d\r",
                   SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY),
                   SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY),
                   SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            fflush(stdout);
            SDL_Delay(100);
        }
        printf("\n");
    }
    SDL_free(ids);
    SDL_Quit();
    return 0;
}
