/* Input from Android into the engine, taken once a frame on the main thread.
 *
 * On Windows, win_wndproc.cpp turned window messages into engine events. Here Win_GetEvent's
 * per-frame call to Sys_ConsoleInput (win32_user.cpp) runs quest_input_pump, which takes the
 * host's queued events (host_input.c) and feeds the same engine entry points:
 *   keys            Sys_QueEvent(SE_KEY) with engine key numbers, plus SE_CHAR for typed text
 *   pointer         CL_MouseEvent in client pixels (the menus' cursor), as bo1_uimouse does
 *   pointer buttons SE_KEY K_MOUSE1.. (what IN_MouseEvent queues), the wheel K_MWHEELUP/DOWN
 * Gamepads go through the engine's own XInput code (win_gamepad.cpp): XInputGetState here
 * returns the host's latest state. */
#include <windows.h>
#include <XInput.h>

#include "win32_internal.h"
#include "bo1_abi.h"
#include "client_mp/cl_input_mp.h"
#include "ui/keycodes.h"
#include "win32/win_local.h"
#include "win32/win_main.h"
#include "win32/win_shared.h"

#include "gfx_d3d/rb_backend.h"
#include "ui/ui_shared.h"

extern "C" unsigned int host_input_poll(struct bo1_input_event *events, unsigned int max);
extern "C" void host_input_gamepad(struct bo1_gamepad *state);
extern "C" void bo1_d3d9_pick_request(int x, int y, void *texture);
extern "C" int bo1_d3d9_pick_result(float *u, float *v, unsigned *serial);
extern "C" void host_xr_pointer_mode(int mode);
extern bool g_bo1_vr_controls; /* quest_vr.cpp: the controllers play this frame */
extern bool g_bo1_mask_lb;     /* quest_vr.cpp: the left grip is the support hand's */
bool UI_IsFocusedMenu3D(int localClientNum);
int Menu_PointAt3D(int localClientNum, UiContext *dc, menuDef_t *menu, float tx, float ty);

namespace {

/* Android keycodes (android/keycodes.h) */
enum {
    AK_BACK = 4, AK_0 = 7, AK_9 = 16, AK_DPAD_UP = 19, AK_DPAD_DOWN = 20, AK_DPAD_LEFT = 21, AK_DPAD_RIGHT = 22,
    AK_A = 29, AK_Z = 54, AK_COMMA = 55, AK_PERIOD = 56, AK_ALT_LEFT = 57, AK_ALT_RIGHT = 58, AK_SHIFT_LEFT = 59,
    AK_SHIFT_RIGHT = 60, AK_TAB = 61, AK_SPACE = 62, AK_ENTER = 66, AK_DEL = 67, AK_GRAVE = 68, AK_MINUS = 69,
    AK_EQUALS = 70, AK_LEFT_BRACKET = 71, AK_RIGHT_BRACKET = 72, AK_BACKSLASH = 73, AK_SEMICOLON = 74,
    AK_APOSTROPHE = 75, AK_SLASH = 76, AK_PAGE_UP = 92, AK_PAGE_DOWN = 93, AK_ESCAPE = 111, AK_FORWARD_DEL = 112,
    AK_CTRL_LEFT = 113, AK_CTRL_RIGHT = 114, AK_CAPS_LOCK = 115, AK_MOVE_HOME = 122, AK_MOVE_END = 123,
    AK_INSERT = 124, AK_F1 = 131, AK_F12 = 142, AK_NUMPAD_ENTER = 160,
};
const int META_SHIFT = 0x1, META_CAPS = 0x100000;

/* the engine's key number for an Android key; 0 for none */
int engine_key(int k)
{
    if (k >= AK_A && k <= AK_Z) return 'a' + (k - AK_A);
    if (k >= AK_0 && k <= AK_9) return '0' + (k - AK_0);
    if (k >= AK_F1 && k <= AK_F12) return K_F1 + (k - AK_F1);
    switch (k) {
    case AK_ENTER: return K_ENTER;
    case AK_NUMPAD_ENTER: return K_KP_ENTER;
    case AK_ESCAPE: case AK_BACK: return K_ESCAPE;
    case AK_DEL: return K_BACKSPACE;
    case AK_FORWARD_DEL: return K_DEL;
    case AK_TAB: return K_TAB;
    case AK_SPACE: return K_SPACE;
    case AK_DPAD_UP: return K_UPARROW;
    case AK_DPAD_DOWN: return K_DOWNARROW;
    case AK_DPAD_LEFT: return K_LEFTARROW;
    case AK_DPAD_RIGHT: return K_RIGHTARROW;
    case AK_SHIFT_LEFT: case AK_SHIFT_RIGHT: return K_SHIFT;
    case AK_CTRL_LEFT: case AK_CTRL_RIGHT: return K_CTRL;
    case AK_ALT_LEFT: case AK_ALT_RIGHT: return K_ALT;
    case AK_CAPS_LOCK: return K_CAPSLOCK;
    case AK_PAGE_UP: return K_PGUP;
    case AK_PAGE_DOWN: return K_PGDN;
    case AK_MOVE_HOME: return K_HOME;
    case AK_MOVE_END: return K_END;
    case AK_INSERT: return K_INS;
    case AK_GRAVE: return '`'; /* the console */
    case AK_MINUS: return '-';
    case AK_EQUALS: return '=';
    case AK_LEFT_BRACKET: return '[';
    case AK_RIGHT_BRACKET: return ']';
    case AK_BACKSLASH: return '\\';
    case AK_SEMICOLON: return ';';
    case AK_APOSTROPHE: return '\'';
    case AK_SLASH: return '/';
    case AK_COMMA: return ',';
    case AK_PERIOD: return '.';
    default: return 0;
    }
}

/* the character a key types on a US layout (what WM_CHAR would carry); 0 for none */
int typed_char(int k, int meta)
{
    bool shift = meta & META_SHIFT;
    if (k >= AK_A && k <= AK_Z) return (shift != !!(meta & META_CAPS) ? 'A' : 'a') + (k - AK_A);
    if (k >= AK_0 && k <= AK_9) return shift ? ")!@#$%^&*("[k - AK_0] : '0' + (k - AK_0);
    switch (k) {
    case AK_SPACE: return ' ';
    case AK_ENTER: case AK_NUMPAD_ENTER: return '\r';
    case AK_DEL: return '\b';
    case AK_TAB: return '\t';
    case AK_GRAVE: return shift ? '~' : '`';
    case AK_MINUS: return shift ? '_' : '-';
    case AK_EQUALS: return shift ? '+' : '=';
    case AK_LEFT_BRACKET: return shift ? '{' : '[';
    case AK_RIGHT_BRACKET: return shift ? '}' : ']';
    case AK_BACKSLASH: return shift ? '|' : '\\';
    case AK_SEMICOLON: return shift ? ':' : ';';
    case AK_APOSTROPHE: return shift ? '"' : '\'';
    case AK_SLASH: return shift ? '?' : '/';
    case AK_COMMA: return shift ? '<' : ',';
    case AK_PERIOD: return shift ? '>' : '.';
    default: return 0;
    }
}

int g_last_x = -1, g_last_y = -1, g_buttons;
int g_point_x = -1, g_point_y = -1, g_point_time = -100000;

void queue_key(int key, bool down) { Sys_QueEvent(Sys_Milliseconds(), SE_KEY, key, down, 0, NULL); }

/* While a TV menu has focus and the pointer moved in the last few seconds: ask the renderer where
   on the TV's texture (the UI3D render target) the pointed-at pixel is, and focus the item there.
   The answer arrives a frame later. */
void point_at_3d_menu()
{
    static unsigned last_serial;
    {
        /* debug: <data>/debug_point holding "x y" (back-buffer pixels) points there, as if the
           pointer had just moved; checked twice a second */
        static int checked;
        if (Sys_Milliseconds() - checked > 500) {
            checked = Sys_Milliseconds();
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s/debug_point", bo1_data_root());
            if (FILE *f = fopen(path, "r")) {
                int x, y;
                if (fscanf(f, "%d %d", &x, &y) == 2) {
                    g_point_x = x;
                    g_point_y = y;
                    g_point_time = Sys_Milliseconds();
                }
                fclose(f);
            }
        }
    }
    /* the headset's pointer: into the world while a menu on an in-world screen (the TV) has focus,
       on the 2D panel otherwise (host_xr.c) */
    host_xr_pointer_mode(g_bo1_vr_controls ? 2 : UI_IsFocusedMenu3D(0) ? 1 : 0);
    const GfxRenderTarget &ui3d = gfxRenderTargets[R_RENDERTARGET_UI3D];
    bool wanted = UI_IsFocusedMenu3D(0) && g_point_x >= 0 && Sys_Milliseconds() - g_point_time < 3000 && ui3d.image;
    float u, v;
    unsigned serial;
    int valid = bo1_d3d9_pick_result(&u, &v, &serial);
    if (wanted && serial != last_serial) {
        static int logged;
        if (logged < 60) {
            logged++;
            bo1_log("point: pixel %d,%d -> %s %.3f %.3f (texture %ux%u)", g_point_x, g_point_y, valid ? "TV" : "nothing", u, v,
                    ui3d.width, ui3d.height);
        }
        if (valid) {
            UiContext *dc = &UI_GetInfo(0)->uiDC;
            if (Menu_PointAt3D(0, dc, Menu_GetFocused(dc), u * ui3d.width, v * ui3d.height))
                bo1_log("point: focused an item at %.0f,%.0f", u * ui3d.width, v * ui3d.height);
        }
    }
    last_serial = serial;
    if (wanted) bo1_d3d9_pick_request(g_point_x, g_point_y, ui3d.image->texture.basemap);
}

} // namespace

void quest_input_pump()
{
    bo1_input_event events[64];
    unsigned n;
    while ((n = host_input_poll(events, 64)) > 0) {
        for (unsigned i = 0; i < n; i++) {
            const bo1_input_event &e = events[i];
            static int logged;
            if (logged < 40 && e.type != BO1_INPUT_POINTER) {
                logged++;
                bo1_log("input: type %u %d %d %d", e.type, e.a, e.b, e.c);
            }
            switch (e.type) {
            case BO1_INPUT_KEY: {
                int key = engine_key(e.a);
                if (key) queue_key(key, e.b != 0);
                int c = e.b ? typed_char(e.a, e.c) : 0;
                if (c) Sys_QueEvent(Sys_Milliseconds(), SE_CHAR, c, 0, 0, NULL);
                break;
            }
            case BO1_INPUT_POINTER: {
                /* the client area is the back buffer's size (win32_user.cpp's window) */
                RECT client;
                GetClientRect(NULL, &client);
                int x = (int)((long long)e.a * client.right / 65535), y = (int)((long long)e.b * client.bottom / 65535);
                if (UI_IsFocusedMenu3D(0)) {
                    /* a TV menu: on Windows the mouse turns the view here; the Quest's pointer points at
                       the TV instead (point_at_3d_menu) */
                    g_point_x = x;
                    g_point_y = y;
                    g_point_time = Sys_Milliseconds();
                } else {
                    CL_MouseEvent(x, y, g_last_x < 0 ? 0 : x - g_last_x, g_last_y < 0 ? 0 : y - g_last_y);
                }
                g_last_x = x;
                g_last_y = y;
                break;
            }
            case BO1_INPUT_BUTTONS:
                for (int b = 0; b < 3; b++)
                    if ((e.a ^ g_buttons) & (1 << b)) queue_key(K_MOUSE1 + b, e.a & (1 << b));
                g_buttons = e.a;
                break;
            case BO1_INPUT_SCROLL: {
                /* A mouse wheel sends whole notches (120 each); the Quest controller's thumbstick sends
                   a steady stream of fractions of one while it's held. Add them up: one wheel step per
                   notch's worth, at most one every SCROLL_REPEAT_MS so a held stick steps through a menu
                   at a readable pace (the TV menus turn wheel steps into up / down). A new direction
                   starts afresh. */
                const int SCROLL_REPEAT_MS = 250;
                static int accumulated, last_step;
                if ((e.a > 0) != (accumulated > 0)) accumulated = 0;
                accumulated += e.a;
                int now = Sys_Milliseconds();
                if ((accumulated >= 120 || accumulated <= -120) && now - last_step >= SCROLL_REPEAT_MS) {
                    int key = accumulated > 0 ? K_MWHEELUP : K_MWHEELDOWN;
                    queue_key(key, true);
                    queue_key(key, false);
                    accumulated = 0;
                    last_step = now;
                } else if (accumulated >= 120 || accumulated <= -120) {
                    accumulated = accumulated > 0 ? 120 : -120; /* waiting out the repeat delay */
                }
                break;
            }
            }
        }
    }
    point_at_3d_menu();
}

/* the engine's gamepad code, on the host's gamepad state; one pad, port 0 */
static bool g_pad_forced; /* debug: <data>/debug_pad */
extern "C" DWORD XInputGetState(DWORD user, XINPUT_STATE *state)
{
    bo1_gamepad pad;
    host_input_gamepad(&pad);
    {
        /* debug: <data>/debug_pad makes the pad connected with the right trigger held (checked each second);
           and how often the game asks */
        static int checked, calls;
        if (calls++ % 600 == 0) bo1_log("pad: polled %d times, connected %u", calls, pad.connected);
        if (Sys_Milliseconds() - checked > 1000) {
            checked = Sys_Milliseconds();
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s/debug_pad", bo1_data_root());
            FILE *f = fopen(path, "r");
            g_pad_forced = f != NULL;
            if (f) fclose(f);
        }
        if (g_pad_forced) {
            pad.connected = 1;
            pad.right_trigger = 255;
        }
    }
    if (user != 0 || !pad.connected) return 1167; /* ERROR_DEVICE_NOT_CONNECTED */
    state->dwPacketNumber = pad.packet;
    {
        /* debug: the first changes of buttons and triggers, as the game gets them */
        static uint16_t last_buttons;
        static uint8_t last_lt, last_rt;
        static int logged;
        bool lt = pad.left_trigger > 64, rt = pad.right_trigger > 64;
        if (logged < 80 && (pad.buttons != last_buttons || lt != (last_lt > 64) || rt != (last_rt > 64))) {
            logged++;
            bo1_log("pad: buttons 0x%04x, triggers %u %u, vr controls %d", pad.buttons, pad.left_trigger, pad.right_trigger,
                    g_bo1_vr_controls ? 1 : 0);
        }
        last_buttons = pad.buttons;
        last_lt = pad.left_trigger;
        last_rt = pad.right_trigger;
    }
    if (g_bo1_vr_controls) pad.right_x = pad.right_y = 0; /* turning and aiming are the headset's (quest_vr.cpp) */
    if (g_bo1_vr_controls && g_bo1_mask_lb) pad.buttons &= (uint16_t)~0x0100; /* the left grip holds the gun there */
    state->Gamepad.wButtons = pad.buttons;
    state->Gamepad.bLeftTrigger = pad.left_trigger;
    state->Gamepad.bRightTrigger = pad.right_trigger;
    state->Gamepad.sThumbLX = pad.left_x;
    state->Gamepad.sThumbLY = pad.left_y;
    state->Gamepad.sThumbRX = pad.right_x;
    state->Gamepad.sThumbRY = pad.right_y;
    return 0;
}

extern "C" DWORD XInputSetState(DWORD, XINPUT_VIBRATION *) { return 0; } /* rumble: with the VR layer */

extern "C" DWORD XInputGetCapabilities(DWORD user, DWORD, XINPUT_CAPABILITIES *capabilities)
{
    bo1_gamepad pad;
    host_input_gamepad(&pad);
    if (g_pad_forced) pad.connected = 1;
    if (user != 0 || !pad.connected) return 1167;
    memset(capabilities, 0, sizeof *capabilities);
    capabilities->Type = 1;    /* XINPUT_DEVTYPE_GAMEPAD */
    capabilities->SubType = 1; /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return 0;
}
