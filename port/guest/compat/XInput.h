/* The XInput types the engine's gamepad code reads. On the Quest the VR input layer
   (milestone 6) implements the functions and fills these from the Touch controllers. */
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
typedef struct _XINPUT_STATE {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;
typedef struct _XINPUT_VIBRATION {
    WORD wLeftMotorSpeed, wRightMotorSpeed;
} XINPUT_VIBRATION;
typedef struct _XINPUT_CAPABILITIES {
    BYTE Type, SubType;
    WORD Flags;
    XINPUT_GAMEPAD Gamepad;
    XINPUT_VIBRATION Vibration;
} XINPUT_CAPABILITIES;

DWORD XInputGetState(DWORD user, XINPUT_STATE *state);
DWORD XInputSetState(DWORD user, XINPUT_VIBRATION *vibration);
DWORD XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES *capabilities);

#ifdef __cplusplus
}
#endif
