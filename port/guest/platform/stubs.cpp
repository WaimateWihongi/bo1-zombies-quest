/* Subsystems the Quest port doesn't have (yet), answered the way the engine already handles their
 * absence on a PC that lacks them:
 *   Steam       - not running (g_steamInitialized false; every interface pointer NULL)
 *   voice chat  - no capture or playback device
 *   sound       - (was: off; now snd_driver_quest.cpp)
 *                 milestone 7 brings an Android audio driver
 *   cutscenes   - none playing (Bink); the cinematic code textures show the engine's own
 *                 "no video" images (black luma, grey chroma), so video materials draw black
 *   video recording, minidumps, NVIDIA stereo, DirectDraw - absent
 *   D3DX        - HLSL compiling fails (fastfile shaders are already compiled); buffers work */
#include <windows.h>
#include <d3dx9.h>
#include <dxerr.h>
#include <ddraw.h>
#include <XInput.h>
#include <nvapi/nvapi.h>

#include <stdlib.h>
#include <string.h>

#include "win32_internal.h"
#include "win32/win_steam.h"
#include "win32/win_voice.h"
#include "win32/win_mini_dumper.h"
#include "groupvoice/play_dsound.h"
#include "groupvoice/record_dsound.h"
#include "gfx_d3d/r_cinematic.h"
#include "gfx_d3d/r_init.h"
#include "gfx_d3d/rb_state.h"
#include "sound/snd_driver_xaudio2.h"
#include "mjpeg/avi.h"
#include "CubeMapGenLib/CCubeMapProcessor.h"

/* ---- Steam ----------------------------------------------------------------------------------- */

bool g_steamInitialized;
bool g_steamAPIInitialized;

void Steam_Init() { bo1_log("Steam: not on the Quest (offline)"); }
void Steam_PrintInitStatus() {}
void Steam_CheckClients() {}
void Steam_CancelClientTicket() {}
void Steam_OnClientDropped(uint64_t) {}
uint64_t Steam_GetClientSteamID64() { return 0; }
bool Steam_CheckClientTicket(const void *, uint32, uint64_t) { return true; } /* LAN/loopback: no tickets */

bool Steam_GetRawClientTicket(unsigned char **buffer, uint32 *size)
{
    *buffer = NULL;
    *size = 0;
    return false;
}

/* the steam_api C entry points live_steam.cpp's inline accessors call */
class CCallbackBase;
static void *g_steam_context[128]; /* every interface slot NULL */
extern "C" {
int SteamAPI_GetHSteamUser() { return 0; }
int SteamGameServer_GetHSteamUser() { return 0; }
void *SteamInternal_ContextInit(void *) { return g_steam_context; }
void *SteamInternal_FindOrCreateUserInterface(int, const char *) { return NULL; }
void *SteamInternal_FindOrCreateGameServerInterface(int, const char *) { return NULL; }
void SteamAPI_RegisterCallResult(CCallbackBase *, uint64_t) {}
void SteamAPI_UnregisterCallResult(CCallbackBase *, uint64_t) {}
void SteamAPI_RegisterCallback(CCallbackBase *, int) {}
void SteamAPI_UnregisterCallback(CCallbackBase *) {}
void SteamAPI_RunCallbacks() {}
void SteamGameServer_RunCallbacks() {}
}

/* ---- voice chat ------------------------------------------------------------------------------ */

float voice_current_scaler = 1.0f;
bool Voice_Init() { return false; }
void Voice_Shutdown() {}
void Voice_Playback() {}
void Voice_StopClientSamples() {}
int Voice_GetLocalVoiceData() { return 0; }
double Voice_GetVoiceLevel() { return 0.0; }
bool Voice_IsClientTalking(unsigned int) { return false; }
void Voice_IncomingVoiceData(unsigned __int8, unsigned __int8 *, int) {}
int Live_GetClientNumForXuid(const SessionData_s *, unsigned __int64) { return -1; }

int DSound_Init(bool, HWND__ *) { return 0; }
void DSound_Shutdown() {}
dsound_sample_t *DSound_NewSample() { return NULL; }
void DSound_SampleFrame(dsound_sample_t *) {}
char DSound_StopSample(dsound_sample_t *) { return 0; }
unsigned int DSound_UpdateSample(dsound_sample_t *, char *, signed int) { return 0; }

int DSOUNDRecord_Init(bool) { return 0; }
void DSOUNDRecord_Shutdown() {}
void DSOUNDRecord_Frame() {}
dsound_sample_t *DSOUNDRecord_NewSample() { return NULL; }
int DSOUNDRecord_DestroySample(dsound_sample_t *) { return 0; }
HRESULT DSOUNDRecord_Start(dsound_sample_t *) { return E_FAIL; }
HRESULT DSOUNDRecord_Stop(dsound_sample_t *) { return E_FAIL; }

/* ---- sound driver: off until the Android one exists ------------------------------------------ */

/* sound: the Quest's own driver now (snd_driver_quest.cpp) */

/* ---- cutscenes: none play -------------------------------------------------------------------- */

/* what r_cinematic.cpp sets when no video is playing; the images exist once the renderer has started */
static void set_no_video_images()
{
    if (!rgp.blackImage || !rgp.grayImage) return;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y] = rgp.blackImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CR] = rgp.grayImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_CB] = rgp.grayImage;
    gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_A] = rgp.blackImage;
}

void R_Cinematic_Init() { set_no_video_images(); }
void R_Cinematic_Shutdown() {}
void R_Cinematic_BeginLostDevice() {}
void R_Cinematic_EndLostDevice() {}
void R_Cinematic_ForceRelinquishIO() {}
void R_Cinematic_StartPlayback_SP(const char *name, unsigned int, float)
{
    bo1_log("cinematic %s: skipped (no video decoder yet)", name ? name : "");
}
void R_Cinematic_StartNextPlayback() {}
void R_Cinematic_StopPlayback() {}
void R_Cinematic_Pause_SP(bool) {}
void R_Cinematic_Unpause_SP(bool) {}
void R_Cinematic_UpdateFrame(bool)
{
    if (!gfxCmdBufInput.codeImages[TEXTURE_SRC_CODE_CINEMATIC_Y]) set_no_video_images();
}
bool R_Cinematic_IsStarted() { return false; }
bool R_Cinematic_IsFinished() { return true; }
bool R_Cinematic_IsNextReady() { return false; }
char R_Cinematic_IsUnderrun() { return 0; }
float R_Cinematic_GetTimeRemaining() { return 0.0f; }

char R_Cinematic_GetFilenameAndTimeInMsec(char *name, int size, unsigned int *time)
{
    if (size > 0) name[0] = 0;
    *time = 0;
    return 0;
}

/* ---- video recording, minidumps -------------------------------------------------------------- */

void mjpeg_create(const char *, int, int, unsigned int) {}
void mjpeg_add_frame(unsigned __int8 *, unsigned __int8 *, unsigned __int8 *) {}
void mjpeg_close_int() {}

void Sys_StartMiniDump(bool) {}
bool Sys_IsMiniDumpStarted() { return false; }

/* ---- reflection-probe filtering (CubeMapGen): done at once, with nothing written ------------- */

CImageSurface::CImageSurface() {}
CImageSurface::~CImageSurface() {}
CCubeMapProcessor::CCubeMapProcessor() {}
CCubeMapProcessor::~CCubeMapProcessor() {}
void CCubeMapProcessor::Init(int32, int32, int32, int32) {}
void CCubeMapProcessor::SetInputFaceData(int32, int32, int32, int32, void *, float32, float32, float32) {}
void CCubeMapProcessor::InitiateFiltering(float32, float32, float32, int32, int32, int32, bool8)
{
    bo1_log("reflection probe filtering: not available on the Quest");
}
int32 CCubeMapProcessor::GetStatus() { return CP_STATUS_FILTER_COMPLETED; }
void CCubeMapProcessor::GetOutputFaceData(int32, int32, int32, int32, int32, void *, float32, float32) {}

/* ---- NVIDIA stereo, DirectDraw (XInput: quest_input.cpp) ------------------------------------------------------- */

extern "C" {
NvAPI_Status NvAPI_Initialize(void) { return NVAPI_ERROR; }
NvAPI_Status NvAPI_Stereo_CreateHandleFromIUnknown(IUnknown *, StereoHandle *handle)
{
    *handle = NULL;
    return NVAPI_ERROR;
}
NvAPI_Status NvAPI_Stereo_DestroyHandle(StereoHandle) { return NVAPI_OK; }
NvAPI_Status NvAPI_Stereo_IsActivated(StereoHandle, NvU8 *active)
{
    *active = 0;
    return NVAPI_OK;
}
NvAPI_Status NvAPI_Stereo_SetConvergence(StereoHandle, float) { return NVAPI_ERROR; }

const GUID IID_IDirectDraw7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};

const char *DXGetErrorDescriptionA(HRESULT hr)
{
    static char text[32];
    snprintf(text, sizeof text, "HRESULT 0x%08lx", (unsigned long)hr);
    return text;
}
}

/* ---- D3DX ------------------------------------------------------------------------------------ */

namespace {
struct Buffer final : ID3DXConstantTable {
    ULONG refs = 1;
    DWORD size;
    void *data;
    explicit Buffer(DWORD n) : size(n), data(calloc(1, n ? n : 1)) {}
    ~Buffer() { free(data); }
    HRESULT QueryInterface(REFIID, void **out) override
    {
        *out = NULL;
        return E_FAIL;
    }
    ULONG AddRef() override { return ++refs; }
    ULONG Release() override
    {
        ULONG left = --refs;
        if (!left) delete this;
        return left;
    }
    void *GetBufferPointer() override { return data; }
    DWORD GetBufferSize() override { return size; }
};

/* finds the 'CTAB' comment block in shader bytecode: comment tokens are 0xFFFE with the length
   in dwords in the high half */
const DWORD *find_ctab(const DWORD *code, DWORD *size)
{
    for (const DWORD *t = code + 1; *t != 0x0000FFFF; t++) {
        if ((*t & 0xFFFF) == 0xFFFE) {
            DWORD words = *t >> 16;
            if (words >= 2 && t[1] == MAKEFOURCC('C', 'T', 'A', 'B')) {
                *size = (words - 1) * 4;
                return t + 2;
            }
            t += words;
        }
        /* other instructions: the engine only asks for shaders whose comments come first */
        else break;
    }
    return NULL;
}
} // namespace

extern "C" HRESULT D3DXCreateBuffer(DWORD size, LPD3DXBUFFER *buffer)
{
    *buffer = new Buffer(size);
    return S_OK;
}

extern "C" HRESULT D3DXCompileShader(const char *, UINT, const void *, void *, const char *function,
                                     const char *profile, DWORD, LPD3DXBUFFER *shader, LPD3DXBUFFER *errors,
                                     LPD3DXCONSTANTTABLE *constants)
{
    bo1_log("D3DXCompileShader %s (%s): HLSL compiling isn't available on the Quest", function ? function : "",
            profile ? profile : "");
    if (shader) *shader = NULL;
    if (errors) *errors = NULL;
    if (constants) *constants = NULL;
    return E_FAIL;
}

extern "C" HRESULT D3DXGetShaderConstantTable(const void *function, LPD3DXCONSTANTTABLE *constants)
{
    DWORD size;
    const DWORD *ctab = find_ctab((const DWORD *)function, &size);
    if (!ctab) {
        *constants = NULL;
        return E_FAIL;
    }
    Buffer *b = new Buffer(size);
    memcpy(b->data, ctab, size);
    *constants = b;
    return S_OK;
}

/* only reached after a successful D3DXCompileShader, which never happens here */
extern "C" HRESULT D3DXGetShaderInputSemantics(const void *, D3DXSEMANTIC *, UINT *count)
{
    *count = 0;
    return E_FAIL;
}

extern "C" HRESULT D3DXGetShaderOutputSemantics(const void *, D3DXSEMANTIC *, UINT *count)
{
    *count = 0;
    return E_FAIL;
}

/* screenshots come back with the GLES renderer (milestone 4) */
extern "C" HRESULT D3DXSaveSurfaceToFileA(const char *path, D3DXIMAGE_FILEFORMAT, IDirect3DSurface9 *, const void *,
                                          const RECT *)
{
    bo1_log("screenshot %s: not available yet", path ? path : "");
    return E_FAIL;
}
