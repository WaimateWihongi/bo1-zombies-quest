/* Android input for the guest.
 *
 * NativeActivity delivers input on the main thread (on_input below); the guest takes it on its
 * own thread each frame (host_input_poll), so events wait in a small ring. What arrives on a flat
 * panel: keyboard keys, a pointer (the Quest controllers' laser, a mouse, a touch) with its
 * buttons and wheel, and gamepads (Bluetooth), whose newest state is kept XInput-style for the
 * engine's own gamepad code. The VR layer (milestone 6) will fill the same gamepad state from the
 * Touch controllers. */
#include "host.h"
#include <stddef.h>
#include "android_native_app_glue.h"

#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RING 512

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct bo1_input_event g_ring[RING];
static unsigned g_head, g_tail; /* g_head - g_tail events waiting */
static struct bo1_gamepad g_pad;
static int g_buttons;

static void push(uint32_t type, int32_t a, int32_t b, int32_t c)
{
    if (g_head - g_tail >= RING) return; /* the guest stopped taking input: drop */
    struct bo1_input_event *e = &g_ring[g_head++ % RING];
    e->type = type;
    e->a = a;
    e->b = b;
    e->c = c;
}

/* ---- gamepads -------------------------------------------------------------------------------- */

/* XINPUT_GAMEPAD_* bits for Android's gamepad keys */
static uint16_t pad_bit(int32_t key)
{
    switch (key) {
    case AKEYCODE_DPAD_UP: return 0x0001;
    case AKEYCODE_DPAD_DOWN: return 0x0002;
    case AKEYCODE_DPAD_LEFT: return 0x0004;
    case AKEYCODE_DPAD_RIGHT: return 0x0008;
    case AKEYCODE_BUTTON_START: return 0x0010;
    case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return 0x0020;
    case AKEYCODE_BUTTON_THUMBL: return 0x0040;
    case AKEYCODE_BUTTON_THUMBR: return 0x0080;
    case AKEYCODE_BUTTON_L1: return 0x0100;
    case AKEYCODE_BUTTON_R1: return 0x0200;
    case AKEYCODE_BUTTON_A: return 0x1000;
    case AKEYCODE_BUTTON_B: return 0x2000;
    case AKEYCODE_BUTTON_X: return 0x4000;
    case AKEYCODE_BUTTON_Y: return 0x8000;
    default: return 0;
    }
}

static int16_t stick(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

static uint8_t trigger(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (uint8_t)(v * 255.0f);
}

static int from_gamepad(const AInputEvent *e)
{
    int32_t source = AInputEvent_getSource(e);
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
           (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
}

static void gamepad_motion(const AInputEvent *e)
{
    g_pad.left_x = stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0));
    g_pad.left_y = stick(-AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0));
    g_pad.right_x = stick(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0));
    g_pad.right_y = stick(-AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0));
    float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0);
    float rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
    if (lt == 0.0f) lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
    if (rt == 0.0f) rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
    g_pad.left_trigger = trigger(lt);
    g_pad.right_trigger = trigger(rt);
    /* pads whose d-pad is a hat axis rather than keys */
    float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
    float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
    g_pad.buttons &= (uint16_t)~0x000F;
    if (hy < -0.5f) g_pad.buttons |= 0x0001;
    if (hy > 0.5f) g_pad.buttons |= 0x0002;
    if (hx < -0.5f) g_pad.buttons |= 0x0004;
    if (hx > 0.5f) g_pad.buttons |= 0x0008;
    g_pad.connected = 1;
    g_pad.packet++;
}

/* ---- the activity's input callback ----------------------------------------------------------- */

int32_t host_on_input(struct android_app *app, AInputEvent *e)
{
    int32_t handled = 0;
    {
        /* debug: what Android delivers (the first 300 events that aren't plain pointer moves) */
        static int logged;
        int32_t type = AInputEvent_getType(e), source = AInputEvent_getSource(e);
        if (type == AINPUT_EVENT_TYPE_KEY && logged < 300) {
            logged++;
            host_info("input event: key source %#x code %d action %d", source, AKeyEvent_getKeyCode(e), AKeyEvent_getAction(e));
        } else if (type == AINPUT_EVENT_TYPE_MOTION && logged < 300) {
            int32_t action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
            if (action != AMOTION_EVENT_ACTION_HOVER_MOVE && action != AMOTION_EVENT_ACTION_MOVE) {
                logged++;
                host_info("input event: motion source %#x action %d buttons %#x x %.0f y %.0f vscroll %.2f hscroll %.2f "
                          "axes x %.2f y %.2f z %.2f rz %.2f",
                          source, action, AMotionEvent_getButtonState(e), AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0),
                          AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0),
                          AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HSCROLL, 0),
                          AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0),
                          AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0));
            } else if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
                logged++;
                host_info("input event: joystick move source %#x x %.2f y %.2f z %.2f rz %.2f", source,
                          AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0),
                          AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0));
            }
        }
    }
    pthread_mutex_lock(&g_lock);
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(e), action = AKeyEvent_getAction(e);
        if (action != AKEY_EVENT_ACTION_MULTIPLE) {
            int down = action == AKEY_EVENT_ACTION_DOWN;
            uint16_t bit = from_gamepad(e) ? pad_bit(key) : 0;
            if (bit) {
                if (down) g_pad.buttons |= bit;
                else g_pad.buttons &= (uint16_t)~bit;
                g_pad.connected = 1;
                g_pad.packet++;
            } else if (!(down && AKeyEvent_getRepeatCount(e) > 0 && key != AKEYCODE_DEL)) {
                push(BO1_INPUT_KEY, key, down, AKeyEvent_getMetaState(e));
            }
            /* keep Back from closing the activity: the game has its own menus */
            handled = key != AKEYCODE_VOLUME_UP && key != AKEYCODE_VOLUME_DOWN;
        }
    } else if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) {
        if (from_gamepad(e)) {
            gamepad_motion(e);
            handled = 1;
        } else {
            int32_t action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
            ANativeWindow *window = app->window;
            float w = window ? (float)ANativeWindow_getWidth(window) : 1.0f;
            float h = window ? (float)ANativeWindow_getHeight(window) : 1.0f;
            float x = AMotionEvent_getX(e, 0) / w, y = AMotionEvent_getY(e, 0) / h;
            push(BO1_INPUT_POINTER, (int32_t)(x * 65535.0f), (int32_t)(y * 65535.0f), 0);
            int buttons = g_buttons;
            if ((AInputEvent_getSource(e) & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) {
                /* a mouse (or the Quest's laser as one): its own button state */
                int32_t state = AMotionEvent_getButtonState(e);
                buttons = (state & AMOTION_EVENT_BUTTON_PRIMARY ? 1 : 0) | (state & AMOTION_EVENT_BUTTON_SECONDARY ? 2 : 0) |
                          (state & AMOTION_EVENT_BUTTON_TERTIARY ? 4 : 0);
                if (action == AMOTION_EVENT_ACTION_DOWN && !buttons) buttons = 1;
                if (action == AMOTION_EVENT_ACTION_UP) buttons = 0;
            } else if (action == AMOTION_EVENT_ACTION_DOWN) {
                buttons = 1; /* a touch, or the laser as a touchscreen: the trigger */
            } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
                buttons = 0;
            }
            if (buttons != g_buttons) {
                g_buttons = buttons;
                push(BO1_INPUT_BUTTONS, buttons, 0, 0);
            }
            if (action == AMOTION_EVENT_ACTION_SCROLL) {
                float v = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0);
                if (v != 0.0f) push(BO1_INPUT_SCROLL, (int32_t)(v * 120.0f), 0, 0);
            }
            handled = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return handled;
}

/* ---- the VR controllers (host_xr.c, render thread): the same events as the flat panel's ------- */

void host_input_pointer(float u, float v)
{
    pthread_mutex_lock(&g_lock);
    push(BO1_INPUT_POINTER, (int32_t)(u * 65535.0f), (int32_t)(v * 65535.0f), 0);
    pthread_mutex_unlock(&g_lock);
}

void host_input_buttons(int buttons)
{
    pthread_mutex_lock(&g_lock);
    if (buttons != g_buttons) {
        g_buttons = buttons;
        push(BO1_INPUT_BUTTONS, buttons, 0, 0);
    }
    pthread_mutex_unlock(&g_lock);
}

void host_input_key(int android_key, int down)
{
    pthread_mutex_lock(&g_lock);
    push(BO1_INPUT_KEY, android_key, down, 0);
    pthread_mutex_unlock(&g_lock);
}

/* wheel notches (fractions of one, as a thumbstick sends them) */
void host_input_scroll(float notches)
{
    pthread_mutex_lock(&g_lock);
    push(BO1_INPUT_SCROLL, (int32_t)(notches * 120.0f), 0, 0);
    pthread_mutex_unlock(&g_lock);
}

/* ---- the guest's side ------------------------------------------------------------------------ */

uint32_t host_input_poll(uint32_t guest_events, uint32_t max)
{
    struct bo1_input_event *out = GUEST_PTR(struct bo1_input_event *, guest_events);
    uint32_t n = 0;
    pthread_mutex_lock(&g_lock);
    while (n < max && g_tail != g_head) out[n++] = g_ring[g_tail++ % RING];
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* the headset's controllers in game (host_xr.c): the gamepad the game reads */
void host_input_xr_pad(const struct bo1_gamepad *pad)
{
    pthread_mutex_lock(&g_lock);
    if (memcmp(&pad->buttons, &g_pad.buttons, sizeof *pad - offsetof(struct bo1_gamepad, buttons)) || !g_pad.connected) {
        uint32_t packet = g_pad.packet + 1;
        g_pad = *pad;
        g_pad.connected = 1;
        g_pad.packet = packet;
    }
    pthread_mutex_unlock(&g_lock);
}

void host_input_gamepad(uint32_t guest_state)
{
    pthread_mutex_lock(&g_lock);
    *GUEST_PTR(struct bo1_gamepad *, guest_state) = g_pad;
    pthread_mutex_unlock(&g_lock);
}
