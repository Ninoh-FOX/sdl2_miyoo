// LGPL-2.1 License
// (C) 2025 Steward Fu <steward.fu@gmail.com>

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MINI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <dirent.h>
#include <linux/input.h>

#include "SDL_hints.h"
#include "SDL_timer.h"
#include "SDL_mutex.h"
#include "../../events/SDL_events_c.h"
#include "../../core/linux/SDL_evdev.h"
#include "../../thread/SDL_systhread.h"

#include "SDL_video_mini.h"
#include "SDL_event_mini.h"

/*
 * Input for the Miyoo Mini buttons (/dev/input/event0).
 *
 * A thread reads the device and queues every key change, in order, with the
 * time it happened. Mini_PumpEvents hands them to SDL:
 *
 *  - Combo window (SDL_MINI_COMBO_MS, 33 ms by default, 0 = off): presses of
 *    the action buttons (everything except the d-pad) that start within the
 *    same window reach the application in the same pump, so buttons pressed
 *    "at the same time" land in the same frame even if the fingers are a
 *    few ms apart. The d-pad is not delayed. Off by default inside
 *    RetroArch, which already does this in its own joypad driver.
 *  - Minimum hold (SDL_MINI_MIN_HOLD_MS, 30 ms by default, 0 = off): a
 *    release is not delivered until the press has been visible for that
 *    long, so applications that read SDL_GetKeyboardState() once per frame
 *    do not miss quick taps.
 *  - Autorepeat events (value 2) are ignored.
 */

#define MINI_QUEUE_SIZE     512
#define MINI_DEF_COMBO_MS   33
#define MINI_DEF_HOLD_MS    30

typedef struct {
    Uint32 time;
    Uint16 code;
    Uint8 pressed;
} mini_keyev_t;

static volatile int running = 0;
static SDL_Thread *thread = NULL;
static SDL_mutex *qlock = NULL;

static mini_keyev_t queue[MINI_QUEUE_SIZE];
static int qcount = 0;
static int qoverflow = 0;

static Uint8 physkey[KEY_MAX] = { 0 };      // state read from the device (thread + qlock)
static Uint8 sentkey[KEY_MAX] = { 0 };      // state sent to SDL (main thread)
static Uint32 presstime[KEY_MAX] = { 0 };   // when the press was sent to SDL
static SDL_Scancode mymap[KEY_MAX] = { 0 };

static Uint32 combo_ms = MINI_DEF_COMBO_MS;
static Uint32 hold_ms = MINI_DEF_HOLD_MS;
static int combo_active = 0;
static Uint32 combo_start = 0;

static Uint32 Mini_EnvMs(const char *name, Uint32 def)
{
    const char *v = SDL_getenv(name);

    if (v && *v) {
        int n = SDL_atoi(v);
        return (n < 0) ? 0 : (n > 1000 ? 1000 : (Uint32)n);
    }
    return def;
}

// RetroArch has its own combo buffer in its Miyoo Mini joypad driver, so the
// SDL one would only add latency on top of it. Look at our own process name
// (comm, or the command line if comm was changed) instead of pgrep, so other
// programs are not affected while RetroArch runs in the background.
static int Mini_IsRetroArch(void)
{
    char buf[256] = { 0 };
    const char *files[] = { "/proc/self/comm", "/proc/self/cmdline" };
    int c0 = 0;

    for (c0 = 0; c0 < 2; c0++) {
        int fd = open(files[c0], O_RDONLY);
        ssize_t len = 0;

        if (fd < 0) {
            continue;
        }
        len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len <= 0) {
            continue;
        }
        buf[len] = 0;   // cmdline: only argv[0] (ends at the first NUL)
        SDL_strlwr(buf);
        if (SDL_strstr(buf, "retroarch")) {
            return 1;
        }
    }
    return 0;
}

static int Mini_IsDpad(int code)
{
    return (code == KEY_UP) || (code == KEY_DOWN) || (code == KEY_LEFT) || (code == KEY_RIGHT);
}

static void Mini_QueueKey(int code, int pressed)
{
    if (physkey[code] == pressed) {
        return;
    }
    physkey[code] = pressed;

    if (qcount < MINI_QUEUE_SIZE) {
        queue[qcount].time = SDL_GetTicks();
        queue[qcount].code = code;
        queue[qcount].pressed = pressed;
        qcount++;
    } else {
        // Nobody is pumping events: resync from physkey on the next pump
        qoverflow = 1;
    }
}

int Mini_InputHandler(void *data)
{
    int fd = -1;
    static struct input_event evs[64];

    debug("%s++\n", __func__);
    fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (fd < 0){
        debug("%s, failed to open input device\n", __func__);
        return 0;
    }

    while (running) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, 50);

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0) {
            continue;
        }

        // Drain everything the driver has, so buttons pressed together are
        // queued together and not one event per sleep
        for (;;) {
            ssize_t len = read(fd, evs, sizeof(evs));
            int c0 = 0, cnt = 0;

            if (len < (ssize_t)sizeof(struct input_event)) {
                break;
            }

            cnt = len / sizeof(struct input_event);
            SDL_LockMutex(qlock);
            for (c0 = 0; c0 < cnt; c0++) {
                const struct input_event *ev = &evs[c0];

                if ((ev->type != EV_KEY) || (ev->code >= KEY_MAX) || (ev->value > 1)) {
                    continue;   // value 2 = autorepeat
                }
                debug("%s, code:%d, value:%d\n", __func__, ev->code, ev->value);
                if (mymap[ev->code]) {
                    Mini_QueueKey(ev->code, ev->value);
                }
            }
            SDL_UnlockMutex(qlock);
        }
    }

    close(fd);
    debug("%s--\n", __func__);
    return 0;
}

static void Mini_SendKey(int code, int pressed, Uint32 now)
{
    if (sentkey[code] == pressed) {
        return;
    }
    sentkey[code] = pressed;
    if (pressed) {
        presstime[code] = now;
    }
    debug("%s, key %s: %d\n", __func__, pressed ? "pressed" : "released", mymap[code]);
    SDL_SendKeyboardKey(pressed ? SDL_PRESSED : SDL_RELEASED, SDL_GetScancodeFromKey(mymap[code]));
}

void Mini_EventInit(void)
{
    debug("%s\n", __func__);

     mymap[KEY_0] = SDLK_0;
     mymap[KEY_1] = SDLK_1;
     mymap[KEY_2] = SDLK_2;
     mymap[KEY_3] = SDLK_3;
     mymap[KEY_4] = SDLK_4;
     mymap[KEY_5] = SDLK_5;
     mymap[KEY_6] = SDLK_6;
     mymap[KEY_7] = SDLK_7;
     mymap[KEY_8] = SDLK_8;
     mymap[KEY_9] = SDLK_9;

     mymap[KEY_A] = SDLK_a;
     mymap[KEY_B] = SDLK_b;
     mymap[KEY_C] = SDLK_c;
     mymap[KEY_D] = SDLK_d;
     mymap[KEY_E] = SDLK_e;
     mymap[KEY_F] = SDLK_f;
     mymap[KEY_G] = SDLK_g;
     mymap[KEY_H] = SDLK_h;
     mymap[KEY_I] = SDLK_i;
     mymap[KEY_J] = SDLK_j;
     mymap[KEY_K] = SDLK_k;
     mymap[KEY_L] = SDLK_l;
     mymap[KEY_M] = SDLK_m;
     mymap[KEY_N] = SDLK_n;
     mymap[KEY_O] = SDLK_o;
     mymap[KEY_P] = SDLK_p;
     mymap[KEY_Q] = SDLK_q;
     mymap[KEY_R] = SDLK_r;
     mymap[KEY_S] = SDLK_s;
     mymap[KEY_T] = SDLK_t;
     mymap[KEY_U] = SDLK_u;
     mymap[KEY_V] = SDLK_v;
     mymap[KEY_W] = SDLK_w;
     mymap[KEY_X] = SDLK_x;
     mymap[KEY_Y] = SDLK_y;
     mymap[KEY_Z] = SDLK_z;

     mymap[KEY_UP] = SDLK_UP;
     mymap[KEY_DOWN] = SDLK_DOWN;
     mymap[KEY_LEFT] = SDLK_LEFT;
     mymap[KEY_RIGHT] = SDLK_RIGHT;
     
     mymap[KEY_ESC] = SDLK_ESCAPE;
     mymap[KEY_SPACE] = SDLK_SPACE;
     mymap[KEY_CAPSLOCK] = SDLK_CAPSLOCK;
     mymap[KEY_BACKSPACE] = SDLK_BACKSPACE;
     mymap[KEY_TAB] = SDLK_TAB;
     mymap[KEY_GRAVE] = SDLK_BACKQUOTE;
     mymap[KEY_COMMA] = SDLK_COMMA;
     mymap[KEY_DOT] = SDLK_PERIOD;
     mymap[KEY_APOSTROPHE] = SDLK_QUOTEDBL;
     mymap[KEY_LEFTBRACE] = SDLK_LEFTBRACKET;
     mymap[KEY_RIGHTBRACE] = SDLK_RIGHTBRACKET;
     mymap[KEY_ENTER] = SDLK_RETURN;
     mymap[KEY_MINUS] = SDLK_MINUS;
     mymap[KEY_EQUAL] = SDLK_EQUALS;
     mymap[KEY_SLASH] = SDLK_SLASH;
     mymap[KEY_BACKSLASH] = SDLK_BACKSLASH;

     mymap[KEY_F1] = SDLK_F1;
     mymap[KEY_F2] = SDLK_F2;
     mymap[KEY_F3] = SDLK_F3;
     mymap[KEY_F4] = SDLK_F4;
     mymap[KEY_F5] = SDLK_F5;

    mymap[KEY_RIGHTSHIFT] = SDLK_RSHIFT;
    mymap[KEY_LEFTSHIFT] = SDLK_LSHIFT;
    mymap[KEY_RIGHTCTRL] = SDLK_RCTRL;
    mymap[KEY_LEFTCTRL] = SDLK_LCTRL;
    mymap[KEY_RIGHTALT] = SDLK_RALT;
    mymap[KEY_LEFTALT] = SDLK_LALT;

    // An explicit SDL_MINI_COMBO_MS always wins; otherwise no window for RetroArch
    combo_ms = Mini_EnvMs("SDL_MINI_COMBO_MS", Mini_IsRetroArch() ? 0 : MINI_DEF_COMBO_MS);
    hold_ms = Mini_EnvMs("SDL_MINI_MIN_HOLD_MS", MINI_DEF_HOLD_MS);
    debug("%s, combo window %u ms, minimum hold %u ms\n", __func__, combo_ms, hold_ms);

    SDL_memset(physkey, 0, sizeof(physkey));
    SDL_memset(sentkey, 0, sizeof(sentkey));
    qcount = 0;
    qoverflow = 0;
    combo_active = 0;

    if ((qlock = SDL_CreateMutex()) == NULL) {
        debug("%s, failed to create input mutex\n", __func__);
        return;
    }

    running = 1;
    if ((thread = SDL_CreateThreadInternal(Mini_InputHandler, "Mini_InputHandler", 16384, NULL)) == NULL) {
        debug("%s, failed to create input thread\n", __func__);
        running = 0;
    }
}

void Mini_EventQuit(void)
{
    debug("%s\n", __func__);
    running = 0;
    if (thread) {
        SDL_WaitThread(thread, NULL);
        thread = NULL;
    }
    if (qlock) {
        SDL_DestroyMutex(qlock);
        qlock = NULL;
    }
}

void Mini_PumpEvents(_THIS)
{
    static Uint8 blocked[KEY_MAX];
    Uint32 now = 0;
    int c0 = 0, kept = 0;

    if (!qlock) {
        return;
    }

    SDL_LockMutex(qlock);
    now = SDL_GetTicks();

    if (qoverflow) {
        // The queue filled up (no pumping for a long time): take the
        // current state as it is
        qoverflow = 0;
        qcount = 0;
        combo_active = 0;
        for (c0 = 0; c0 < KEY_MAX; c0++) {
            if (mymap[c0]) {
                Mini_SendKey(c0, physkey[c0], now);
            }
        }
        SDL_UnlockMutex(qlock);
        return;
    }

    if (qcount == 0) {
        SDL_UnlockMutex(qlock);
        return;
    }

    // Events of a key that has an earlier event still waiting must wait too,
    // so every key keeps its own order; other keys are not held up by it
    SDL_memset(blocked, 0, sizeof(blocked));
    for (c0 = 0; c0 < qcount; c0++) {
        const mini_keyev_t ev = queue[c0];
        int wait = 0;

        if (blocked[ev.code]) {
            wait = 1;
        } else if (ev.pressed) {
            if (combo_ms && !Mini_IsDpad(ev.code)) {
                if (!combo_active || ((Uint32)(ev.time - combo_start) >= combo_ms)) {
                    combo_active = 1;
                    combo_start = ev.time;
                }
                if ((Uint32)(now - combo_start) < combo_ms) {
                    wait = 1;
                }
            }
        } else if (hold_ms && sentkey[ev.code] && ((Uint32)(now - presstime[ev.code]) < hold_ms)) {
            wait = 1;
        }

        if (wait) {
            blocked[ev.code] = 1;
            queue[kept++] = ev;
        } else {
            Mini_SendKey(ev.code, ev.pressed, now);
        }
    }
    qcount = kept;
    SDL_UnlockMutex(qlock);
}

#endif
