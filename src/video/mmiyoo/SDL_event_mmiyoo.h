/*
  Customized version for Miyoo-Mini handheld.
  Only tested under Miyoo-Mini stock OS (original firmware) with Parasyte compatible layer.

  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>
  Copyright (C) 2022-2022 Steward Fu <steward.fu@gmail.com>
  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#ifndef __SDL_EVENT_MMIYOO_H__
#define __SDL_EVENT_MMIYOO_H__

#include "../../SDL_internal.h"
#include "SDL_event_mmiyoo.h"

#define MYKEY_UP            0
#define MYKEY_DOWN          1
#define MYKEY_LEFT          2
#define MYKEY_RIGHT         3
#define MYKEY_A             4
#define MYKEY_B             5
#define MYKEY_X             6
#define MYKEY_Y             7
#define MYKEY_L1            8
#define MYKEY_R1            9
#define MYKEY_L2            10
#define MYKEY_R2            11
#define MYKEY_SELECT        12
#define MYKEY_START         13
#define MYKEY_MENU          14
#define MYKEY_QSAVE         15
#define MYKEY_QLOAD         16
#define MYKEY_FF            17
#define MYKEY_EXIT          18
#define MYKEY_POWER         19
#define MYKEY_VOLUP         20
#define MYKEY_VOLDOWN       21

#define MYKEY_LAST_BITS     18 // ignore POWER, VOL-, VOL+ keys

#define MMIYOO_KEYPAD_MODE 0
#define MMIYOO_MOUSE_MODE  1

typedef struct _MMIYOO_EventInfo {
    struct _keypad{
        uint32_t bitmaps;
    } keypad;

    struct _mouse{
        int x;
        int y;
        int minx;
        int miny;
        int maxx;
        int maxy;
    } mouse;

    int mode;
} MMIYOO_EventInfo;

extern void MMIYOO_EventInit(void);
extern void MMIYOO_EventDeinit(void);
extern void MMIYOO_PumpEvents(_THIS);

#endif

