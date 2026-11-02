/* NVIDIA's driver API (stereo 3D, SLI) does not exist on the Quest: the guest's versions of
   these functions all fail, so the renderer takes its non-NVIDIA paths. Only what the engine names. */
#pragma once
#include <d3d9.h>

typedef int NvAPI_Status;
#define NVAPI_OK 0
#define NVAPI_ERROR (-1)
typedef unsigned int NvU32;
typedef unsigned char NvU8;
typedef void *StereoHandle;
struct NVDX_ObjectHandle__;
typedef struct NVDX_ObjectHandle__ *NVDX_ObjectHandle;

typedef struct {
    NvU32 version;
    NvU32 drvVersion;
} NV_DISPLAY_DRIVER_VERSION;
typedef struct {
    NvU32 version;
    NvU32 maxNumAFRGroups;
    NvU32 numAFRGroups;
} NV_GET_CURRENT_SLI_STATE;

#ifdef __cplusplus
extern "C" {
#endif
NvAPI_Status NvAPI_Initialize(void);
NvAPI_Status NvAPI_GetDisplayDriverVersion(NvU32 display, NV_DISPLAY_DRIVER_VERSION *version);
NvAPI_Status NvAPI_D3D_GetCurrentSLIState(IUnknown *device, NV_GET_CURRENT_SLI_STATE *state);
NvAPI_Status NvAPI_D3D9_GetTextureHandle(IDirect3DTexture9 *texture, NVDX_ObjectHandle *handle);
NvAPI_Status NvAPI_D3D9_GetCurrentZBufferHandle(IDirect3DDevice9 *device, NVDX_ObjectHandle *handle);
NvAPI_Status NvAPI_D3D9_StretchRect(IDirect3DDevice9 *device, NVDX_ObjectHandle source, const RECT *source_rect,
                                    IDirect3DSurface9 *dest, const RECT *dest_rect, D3DTEXTUREFILTERTYPE filter);
NvAPI_Status NvAPI_Stereo_IsEnabled(NvU8 *enabled);
NvAPI_Status NvAPI_Stereo_IsActivated(StereoHandle handle, NvU8 *activated);
NvAPI_Status NvAPI_Stereo_CreateHandleFromIUnknown(IUnknown *device, StereoHandle *handle);
NvAPI_Status NvAPI_Stereo_DestroyHandle(StereoHandle handle);
NvAPI_Status NvAPI_Stereo_SetConvergence(StereoHandle handle, float convergence);
#ifdef __cplusplus
}
#endif
