/* DirectSound voice chat is not built for the guest (play_dsound.cpp / record_dsound.cpp);
   groupvoice's shared headers only need the interface names. */
#pragma once
#include <windows.h>

#ifdef __cplusplus
#include <d3d9.h>
/* only the cinematic player's device; DirectSoundCreate8 fails on the guest */
struct IDirectSound8 : IUnknown {
    virtual HRESULT SetCooperativeLevel(HWND window, DWORD level) = 0;
};
extern "C" HRESULT DirectSoundCreate8(const GUID *device, IDirectSound8 **ds, IUnknown *outer);
struct IDirectSoundBuffer;
struct IDirectSoundCaptureBuffer;
typedef IDirectSound8 *LPDIRECTSOUND8;
#else
typedef struct IDirectSound8 IDirectSound8;
typedef struct IDirectSoundBuffer IDirectSoundBuffer;
typedef struct IDirectSoundCaptureBuffer IDirectSoundCaptureBuffer;
#endif
