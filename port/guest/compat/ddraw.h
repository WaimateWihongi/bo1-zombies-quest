/* DirectDraw, which the engine loads only to ask for free video memory (r_texturemem.cpp);
   LoadLibraryA("ddraw.dll") fails on the guest, so the query is never made. */
#pragma once
#include <d3d9.h>

typedef struct _DDSCAPS2 {
    DWORD dwCaps, dwCaps2, dwCaps3, dwCaps4;
} DDSCAPS2, *LPDDSCAPS2;
#define DDSCAPS_VIDEOMEMORY 0x00004000
#define DDSCAPS_LOCALVIDMEM 0x10000000
#define DDSCAPS_TEXTURE 0x00001000

#ifdef __cplusplus
struct IDirectDraw7 : IUnknown {
    virtual HRESULT GetAvailableVidMem(LPDDSCAPS2 caps, LPDWORD total, LPDWORD free) = 0;
    virtual HRESULT SetCooperativeLevel(HWND window, DWORD flags) = 0;
};
extern "C" const GUID IID_IDirectDraw7;
#endif
