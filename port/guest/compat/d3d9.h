/* Direct3D 9 for the guest: the types and constants the engine uses, with their documented API
   values (image assets in fastfiles store D3DFORMAT values, so those must match exactly).
   The interfaces are declared here and implemented on OpenGL ES by the renderer port
   (milestone 4). */
#pragma once

#include <windows.h>

#define D3D_SDK_VERSION 32
#define D3DADAPTER_DEFAULT 0
#define D3D_OK S_OK
#define MAKE_D3DHRESULT(code) ((HRESULT)(0x88760000u | (code)))
#define D3DERR_NOTAVAILABLE MAKE_D3DHRESULT(2154)
#define D3DERR_INVALIDCALL MAKE_D3DHRESULT(2156)
#define D3DERR_DEVICELOST MAKE_D3DHRESULT(2152)
#define S_PRESENT_OCCLUDED ((HRESULT)0x08760868)
#define D3DERR_DEVICENOTRESET MAKE_D3DHRESULT(2153)
#define D3DDDIERR_DEVICEREMOVED ((HRESULT)0x88760870)

typedef DWORD D3DCOLOR;
#define D3DCOLOR_ARGB(a, r, g, b) ((D3DCOLOR)((((a) & 0xff) << 24) | (((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))
#define D3DCOLOR_XRGB(r, g, b) D3DCOLOR_ARGB(0xff, r, g, b)

#define BO1_FOURCC(a, b, c, d) ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
typedef enum _D3DFORMAT {
    D3DFMT_UNKNOWN = 0,
    D3DFMT_R8G8B8 = 20,
    D3DFMT_A8R8G8B8 = 21,
    D3DFMT_X8R8G8B8 = 22,
    D3DFMT_R5G6B5 = 23,
    D3DFMT_X1R5G5B5 = 24,
    D3DFMT_A1R5G5B5 = 25,
    D3DFMT_A4R4G4B4 = 26,
    D3DFMT_A8 = 28,
    D3DFMT_A8B8G8R8 = 32,
    D3DFMT_G16R16 = 34,
    D3DFMT_A16B16G16R16 = 36,
    D3DFMT_L8 = 50,
    D3DFMT_A8L8 = 51,
    D3DFMT_D16_LOCKABLE = 70,
    D3DFMT_D15S1 = 73,
    D3DFMT_D24S8 = 75,
    D3DFMT_D24X8 = 77,
    D3DFMT_D16 = 80,
    D3DFMT_D24FS8 = 83,
    D3DFMT_INDEX16 = 101,
    D3DFMT_INDEX32 = 102,
    D3DFMT_G16R16F = 112,
    D3DFMT_A16B16G16R16F = 113,
    D3DFMT_R32F = 114,
    D3DFMT_G32R32F = 115,
    D3DFMT_A32B32G32R32F = 116,
    D3DFMT_DXT1 = BO1_FOURCC('D', 'X', 'T', '1'),
    D3DFMT_DXT2 = BO1_FOURCC('D', 'X', 'T', '2'),
    D3DFMT_DXT3 = BO1_FOURCC('D', 'X', 'T', '3'),
    D3DFMT_DXT4 = BO1_FOURCC('D', 'X', 'T', '4'),
    D3DFMT_DXT5 = BO1_FOURCC('D', 'X', 'T', '5'),
    D3DFMT_FORCE_DWORD = 0x7fffffff
} D3DFORMAT;

typedef enum _D3DPOOL {
    D3DPOOL_DEFAULT = 0,
    D3DPOOL_MANAGED = 1,
    D3DPOOL_SYSTEMMEM = 2,
    D3DPOOL_SCRATCH = 3,
    D3DPOOL_FORCE_DWORD = 0x7fffffff
} D3DPOOL;

typedef enum _D3DDEVTYPE {
    D3DDEVTYPE_HAL = 1,
    D3DDEVTYPE_REF = 2,
    D3DDEVTYPE_SW = 3,
    D3DDEVTYPE_NULLREF = 4,
    D3DDEVTYPE_FORCE_DWORD = 0x7fffffff
} D3DDEVTYPE;

typedef enum _D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE = 0,
    D3DMULTISAMPLE_NONMASKABLE = 1,
    D3DMULTISAMPLE_2_SAMPLES = 2,
    D3DMULTISAMPLE_4_SAMPLES = 4,
    D3DMULTISAMPLE_8_SAMPLES = 8,
    D3DMULTISAMPLE_16_SAMPLES = 16,
    D3DMULTISAMPLE_FORCE_DWORD = 0x7fffffff
} D3DMULTISAMPLE_TYPE;

typedef enum _D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE = 0,
    D3DTEXF_POINT = 1,
    D3DTEXF_LINEAR = 2,
    D3DTEXF_ANISOTROPIC = 3,
    D3DTEXF_PYRAMIDALQUAD = 6,
    D3DTEXF_GAUSSIANQUAD = 7,
    D3DTEXF_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREFILTERTYPE;

typedef enum _D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP = 1,
    D3DTADDRESS_MIRROR = 2,
    D3DTADDRESS_CLAMP = 3,
    D3DTADDRESS_BORDER = 4,
    D3DTADDRESS_MIRRORONCE = 5,
    D3DTADDRESS_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREADDRESS;

typedef enum _D3DSAMPLERSTATETYPE {
    D3DSAMP_ADDRESSU = 1,
    D3DSAMP_ADDRESSV = 2,
    D3DSAMP_ADDRESSW = 3,
    D3DSAMP_BORDERCOLOR = 4,
    D3DSAMP_MAGFILTER = 5,
    D3DSAMP_MINFILTER = 6,
    D3DSAMP_MIPFILTER = 7,
    D3DSAMP_MIPMAPLODBIAS = 8,
    D3DSAMP_MAXMIPLEVEL = 9,
    D3DSAMP_MAXANISOTROPY = 10,
    D3DSAMP_SRGBTEXTURE = 11,
    D3DSAMP_ELEMENTINDEX = 12,
    D3DSAMP_DMAPOFFSET = 13,
    D3DSAMP_FORCE_DWORD = 0x7fffffff
} D3DSAMPLERSTATETYPE;

typedef enum _D3DRENDERSTATETYPE {
    D3DRS_ZENABLE = 7,
    D3DRS_FILLMODE = 8,
    D3DRS_SHADEMODE = 9,
    D3DRS_ZWRITEENABLE = 14,
    D3DRS_ALPHATESTENABLE = 15,
    D3DRS_LASTPIXEL = 16,
    D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20,
    D3DRS_CULLMODE = 22,
    D3DRS_ZFUNC = 23,
    D3DRS_ALPHAREF = 24,
    D3DRS_ALPHAFUNC = 25,
    D3DRS_DITHERENABLE = 26,
    D3DRS_ALPHABLENDENABLE = 27,
    D3DRS_FOGENABLE = 28,
    D3DRS_SPECULARENABLE = 29,
    D3DRS_STENCILENABLE = 52,
    D3DRS_STENCILFAIL = 53,
    D3DRS_STENCILZFAIL = 54,
    D3DRS_STENCILPASS = 55,
    D3DRS_STENCILFUNC = 56,
    D3DRS_STENCILREF = 57,
    D3DRS_STENCILMASK = 58,
    D3DRS_STENCILWRITEMASK = 59,
    D3DRS_TEXTUREFACTOR = 60,
    D3DRS_WRAP0 = 128,
    D3DRS_CLIPPING = 136,
    D3DRS_LIGHTING = 137,
    D3DRS_POINTSIZE = 154,
    D3DRS_COLORWRITEENABLE = 168,
    D3DRS_BLENDOP = 171,
    D3DRS_SCISSORTESTENABLE = 174,
    D3DRS_SLOPESCALEDEPTHBIAS = 175,
    D3DRS_ADAPTIVETESS_Y = 181,
    D3DRS_TWOSIDEDSTENCILMODE = 185,
    D3DRS_CCW_STENCILFAIL = 186,
    D3DRS_CCW_STENCILZFAIL = 187,
    D3DRS_CCW_STENCILPASS = 188,
    D3DRS_CCW_STENCILFUNC = 189,
    D3DRS_COLORWRITEENABLE1 = 190,
    D3DRS_COLORWRITEENABLE2 = 191,
    D3DRS_COLORWRITEENABLE3 = 192,
    D3DRS_BLENDFACTOR = 193,
    D3DRS_SRGBWRITEENABLE = 194,
    D3DRS_DEPTHBIAS = 195,
    D3DRS_SEPARATEALPHABLENDENABLE = 206,
    D3DRS_SRCBLENDALPHA = 207,
    D3DRS_DESTBLENDALPHA = 208,
    D3DRS_BLENDOPALPHA = 209,
    D3DRS_FORCE_DWORD = 0x7fffffff
} D3DRENDERSTATETYPE;

typedef enum _D3DZBUFFERTYPE { D3DZB_FALSE = 0, D3DZB_TRUE = 1, D3DZB_USEW = 2, D3DZB_FORCE_DWORD = 0x7fffffff } D3DZBUFFERTYPE;
typedef enum _D3DFILLMODE { D3DFILL_POINT = 1, D3DFILL_WIREFRAME = 2, D3DFILL_SOLID = 3, D3DFILL_FORCE_DWORD = 0x7fffffff } D3DFILLMODE;
typedef enum _D3DCULL { D3DCULL_NONE = 1, D3DCULL_CW = 2, D3DCULL_CCW = 3, D3DCULL_FORCE_DWORD = 0x7fffffff } D3DCULL;

typedef enum _D3DBLEND {
    D3DBLEND_ZERO = 1,
    D3DBLEND_ONE = 2,
    D3DBLEND_SRCCOLOR = 3,
    D3DBLEND_INVSRCCOLOR = 4,
    D3DBLEND_SRCALPHA = 5,
    D3DBLEND_INVSRCALPHA = 6,
    D3DBLEND_DESTALPHA = 7,
    D3DBLEND_INVDESTALPHA = 8,
    D3DBLEND_DESTCOLOR = 9,
    D3DBLEND_INVDESTCOLOR = 10,
    D3DBLEND_SRCALPHASAT = 11,
    D3DBLEND_FORCE_DWORD = 0x7fffffff
} D3DBLEND;

typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1,
    D3DPT_LINELIST = 2,
    D3DPT_LINESTRIP = 3,
    D3DPT_TRIANGLELIST = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN = 6,
    D3DPT_FORCE_DWORD = 0x7fffffff
} D3DPRIMITIVETYPE;

typedef enum _D3DRESOURCETYPE {
    D3DRTYPE_SURFACE = 1,
    D3DRTYPE_VOLUME = 2,
    D3DRTYPE_TEXTURE = 3,
    D3DRTYPE_VOLUMETEXTURE = 4,
    D3DRTYPE_CUBETEXTURE = 5,
    D3DRTYPE_VERTEXBUFFER = 6,
    D3DRTYPE_INDEXBUFFER = 7,
    D3DRTYPE_FORCE_DWORD = 0x7fffffff
} D3DRESOURCETYPE;

typedef enum _D3DCUBEMAP_FACES {
    D3DCUBEMAP_FACE_POSITIVE_X = 0,
    D3DCUBEMAP_FACE_NEGATIVE_X = 1,
    D3DCUBEMAP_FACE_POSITIVE_Y = 2,
    D3DCUBEMAP_FACE_NEGATIVE_Y = 3,
    D3DCUBEMAP_FACE_POSITIVE_Z = 4,
    D3DCUBEMAP_FACE_NEGATIVE_Z = 5,
    D3DCUBEMAP_FACE_FORCE_DWORD = 0x7fffffff
} D3DCUBEMAP_FACES;

typedef enum _D3DQUERYTYPE {
    D3DQUERYTYPE_EVENT = 8,
    D3DQUERYTYPE_OCCLUSION = 9,
    D3DQUERYTYPE_TIMESTAMP = 10,
    D3DQUERYTYPE_TIMESTAMPDISJOINT = 11,
    D3DQUERYTYPE_TIMESTAMPFREQ = 12
} D3DQUERYTYPE;
#define D3DISSUE_END (1 << 0)
#define D3DISSUE_BEGIN (1 << 1)
#define D3DGETDATA_FLUSH (1 << 0)

typedef enum _D3DSWAPEFFECT {
    D3DSWAPEFFECT_DISCARD = 1,
    D3DSWAPEFFECT_FLIP = 2,
    D3DSWAPEFFECT_COPY = 3,
    D3DSWAPEFFECT_FORCE_DWORD = 0x7fffffff
} D3DSWAPEFFECT;

typedef enum _D3DBACKBUFFER_TYPE {
    D3DBACKBUFFER_TYPE_MONO = 0,
    D3DBACKBUFFER_TYPE_LEFT = 1,
    D3DBACKBUFFER_TYPE_RIGHT = 2,
    D3DBACKBUFFER_TYPE_FORCE_DWORD = 0x7fffffff
} D3DBACKBUFFER_TYPE;

/* fixed-function texture stages: only the engine's few 2D paths use them */
typedef enum _D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP = 1,
    D3DTSS_COLORARG1 = 2,
    D3DTSS_COLORARG2 = 3,
    D3DTSS_ALPHAOP = 4,
    D3DTSS_ALPHAARG1 = 5,
    D3DTSS_ALPHAARG2 = 6,
    D3DTSS_FORCE_DWORD = 0x7fffffff
} D3DTEXTURESTAGESTATETYPE;
typedef enum _D3DTEXTUREOP { D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3, D3DTOP_MODULATE = 4, D3DTOP_FORCE_DWORD = 0x7fffffff } D3DTEXTUREOP;
#define D3DTA_DIFFUSE 0x00000000
#define D3DTA_CURRENT 0x00000001
#define D3DTA_TEXTURE 0x00000002

#define D3DUSAGE_RENDERTARGET 0x00000001
#define D3DUSAGE_DEPTHSTENCIL 0x00000002
#define D3DUSAGE_WRITEONLY 0x00000008
#define D3DUSAGE_DYNAMIC 0x00000200
#define D3DLOCK_READONLY 0x00000010
#define D3DLOCK_DISCARD 0x00002000
#define D3DLOCK_NOOVERWRITE 0x00001000
#define D3DCLEAR_TARGET 0x00000001
#define D3DCLEAR_ZBUFFER 0x00000002
#define D3DCLEAR_STENCIL 0x00000004
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040
#define D3DCREATE_MULTITHREADED 0x00000004
#define D3DPTEXTURECAPS_NONPOW2CONDITIONAL 0x00000100
#define D3DFVF_XYZRHW 0x004
#define D3DFVF_DIFFUSE 0x040
#define D3DFVF_SPECULAR 0x080
#define D3DFVF_TEX1 0x100

typedef enum _D3DDECLTYPE {
    D3DDECLTYPE_FLOAT1 = 0,
    D3DDECLTYPE_FLOAT2 = 1,
    D3DDECLTYPE_FLOAT3 = 2,
    D3DDECLTYPE_FLOAT4 = 3,
    D3DDECLTYPE_D3DCOLOR = 4,
    D3DDECLTYPE_UBYTE4 = 5,
    D3DDECLTYPE_SHORT2 = 6,
    D3DDECLTYPE_SHORT4 = 7,
    D3DDECLTYPE_UBYTE4N = 8,
    D3DDECLTYPE_SHORT2N = 9,
    D3DDECLTYPE_SHORT4N = 10,
    D3DDECLTYPE_USHORT2N = 11,
    D3DDECLTYPE_USHORT4N = 12,
    D3DDECLTYPE_UDEC3 = 13,
    D3DDECLTYPE_DEC3N = 14,
    D3DDECLTYPE_FLOAT16_2 = 15,
    D3DDECLTYPE_FLOAT16_4 = 16,
    D3DDECLTYPE_UNUSED = 17
} D3DDECLTYPE;
typedef enum _D3DDECLMETHOD { D3DDECLMETHOD_DEFAULT = 0 } D3DDECLMETHOD;
typedef enum _D3DDECLUSAGE {
    D3DDECLUSAGE_POSITION = 0,
    D3DDECLUSAGE_BLENDWEIGHT = 1,
    D3DDECLUSAGE_BLENDINDICES = 2,
    D3DDECLUSAGE_NORMAL = 3,
    D3DDECLUSAGE_PSIZE = 4,
    D3DDECLUSAGE_TEXCOORD = 5,
    D3DDECLUSAGE_TANGENT = 6,
    D3DDECLUSAGE_BINORMAL = 7,
    D3DDECLUSAGE_TESSFACTOR = 8,
    D3DDECLUSAGE_POSITIONT = 9,
    D3DDECLUSAGE_COLOR = 10,
    D3DDECLUSAGE_FOG = 11,
    D3DDECLUSAGE_DEPTH = 12,
    D3DDECLUSAGE_SAMPLE = 13
} D3DDECLUSAGE;

typedef struct _D3DVERTEXELEMENT9 {
    WORD Stream;
    WORD Offset;
    BYTE Type;
    BYTE Method;
    BYTE Usage;
    BYTE UsageIndex;
} D3DVERTEXELEMENT9, *LPD3DVERTEXELEMENT9;
#define D3DDECL_END() {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0}

typedef struct _D3DRECT {
    LONG x1, y1, x2, y2;
} D3DRECT;

typedef struct _D3DVIEWPORT9 {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT9;

typedef struct _D3DLOCKED_RECT {
    INT Pitch;
    void *pBits;
} D3DLOCKED_RECT;

typedef struct _D3DBOX {
    UINT Left, Top, Right, Bottom, Front, Back;
} D3DBOX;

typedef struct _D3DLOCKED_BOX {
    INT RowPitch;
    INT SlicePitch;
    void *pBits;
} D3DLOCKED_BOX;

typedef struct _D3DSURFACE_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD MultiSampleQuality;
    UINT Width;
    UINT Height;
} D3DSURFACE_DESC;

typedef struct _D3DVOLUME_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Width;
    UINT Height;
    UINT Depth;
} D3DVOLUME_DESC;

typedef struct _D3DDISPLAYMODE {
    UINT Width;
    UINT Height;
    UINT RefreshRate;
    D3DFORMAT Format;
} D3DDISPLAYMODE;

typedef struct _D3DGAMMARAMP {
    WORD red[256];
    WORD green[256];
    WORD blue[256];
} D3DGAMMARAMP;

typedef struct _D3DPRESENT_PARAMETERS_ {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD MultiSampleQuality;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct _D3DADAPTER_IDENTIFIER9 {
    char Driver[512];
    char Description[512];
    char DeviceName[32];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId;
    DWORD DeviceId;
    DWORD SubSysId;
    DWORD Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
} D3DADAPTER_IDENTIFIER9;

/* Capabilities: the renderer port fills this with what GLES offers. Laid out as the API's
   structure, with the members the engine reads named. */
typedef struct _D3DVSHADERCAPS2_0 {
    DWORD Caps;
    INT DynamicFlowControlDepth;
    INT NumTemps;
    INT StaticFlowControlDepth;
} D3DVSHADERCAPS2_0;
typedef struct _D3DPSHADERCAPS2_0 {
    DWORD Caps;
    INT DynamicFlowControlDepth;
    INT NumTemps;
    INT StaticFlowControlDepth;
    INT NumInstructionSlots;
} D3DPSHADERCAPS2_0;
typedef struct _D3DCAPS9 {
    D3DDEVTYPE DeviceType;
    UINT AdapterOrdinal;
    DWORD Caps, Caps2, Caps3;
    DWORD PresentationIntervals;
    DWORD CursorCaps;
    DWORD DevCaps;
    DWORD PrimitiveMiscCaps;
    DWORD RasterCaps;
    DWORD ZCmpCaps;
    DWORD SrcBlendCaps;
    DWORD DestBlendCaps;
    DWORD AlphaCmpCaps;
    DWORD ShadeCaps;
    DWORD TextureCaps;
    DWORD TextureFilterCaps;
    DWORD CubeTextureFilterCaps;
    DWORD VolumeTextureFilterCaps;
    DWORD TextureAddressCaps;
    DWORD VolumeTextureAddressCaps;
    DWORD LineCaps;
    DWORD MaxTextureWidth, MaxTextureHeight;
    DWORD MaxVolumeExtent;
    DWORD MaxTextureRepeat;
    DWORD MaxTextureAspectRatio;
    DWORD MaxAnisotropy;
    float MaxVertexW;
    float GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom;
    float ExtentsAdjust;
    DWORD StencilCaps;
    DWORD FVFCaps;
    DWORD TextureOpCaps;
    DWORD MaxTextureBlendStages;
    DWORD MaxSimultaneousTextures;
    DWORD VertexProcessingCaps;
    DWORD MaxActiveLights;
    DWORD MaxUserClipPlanes;
    DWORD MaxVertexBlendMatrices;
    DWORD MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount;
    DWORD MaxVertexIndex;
    DWORD MaxStreams;
    DWORD MaxStreamStride;
    DWORD VertexShaderVersion;
    DWORD MaxVertexShaderConst;
    DWORD PixelShaderVersion;
    float PixelShader1xMaxValue;
    DWORD DevCaps2;
    float MaxNpatchTessellationLevel;
    DWORD Reserved5;
    UINT MasterAdapterOrdinal;
    UINT AdapterOrdinalInGroup;
    UINT NumberOfAdaptersInGroup;
    DWORD DeclTypes;
    DWORD NumSimultaneousRTs;
    DWORD StretchRectFilterCaps;
    D3DVSHADERCAPS2_0 VS20Caps;
    D3DPSHADERCAPS2_0 PS20Caps;
    DWORD VertexTextureFilterCaps;
    DWORD MaxVShaderInstructionsExecuted;
    DWORD MaxPShaderInstructionsExecuted;
    DWORD MaxVertexShader30InstructionSlots;
    DWORD MaxPixelShader30InstructionSlots;
} D3DCAPS9;

typedef struct _D3DVERTEXBUFFER_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    DWORD FVF;
} D3DVERTEXBUFFER_DESC;
typedef struct _D3DINDEXBUFFER_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
} D3DINDEXBUFFER_DESC;
typedef struct D3DDISPLAYMODEEX {
    UINT Size, Width, Height, RefreshRate;
    D3DFORMAT Format;
    UINT ScanLineOrdering;
} D3DDISPLAYMODEEX;
typedef struct _RGNDATA RGNDATA;

#ifdef __cplusplus
/* Only the methods the engine calls, as abstract classes. The renderer port implements them,
   so the method order only has to agree with itself. */
struct IUnknown {
    virtual HRESULT QueryInterface(REFIID iid, void **object) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
};
struct IDirect3DDevice9;
struct IDirect3DSurface9;

struct IDirect3DResource9 : IUnknown {
    virtual D3DRESOURCETYPE GetType() = 0;
    virtual void PreLoad() = 0;
};
struct IDirect3DBaseTexture9 : IDirect3DResource9 {
    virtual DWORD GetLevelCount() = 0;
};
struct IDirect3DTexture9 : IDirect3DBaseTexture9 {
    virtual HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) = 0;
    virtual HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface) = 0;
    virtual HRESULT LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) = 0;
    virtual HRESULT UnlockRect(UINT level) = 0;
    virtual HRESULT AddDirtyRect(const RECT *rect) = 0;
};
struct IDirect3DCubeTexture9 : IDirect3DBaseTexture9 {
    virtual HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) = 0;
    virtual HRESULT GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface) = 0;
    virtual HRESULT LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect,
                             DWORD flags) = 0;
    virtual HRESULT UnlockRect(D3DCUBEMAP_FACES face, UINT level) = 0;
};
struct IDirect3DVolumeTexture9 : IDirect3DBaseTexture9 {
    virtual HRESULT GetLevelDesc(UINT level, D3DVOLUME_DESC *desc) = 0;
    virtual HRESULT LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) = 0;
    virtual HRESULT UnlockBox(UINT level) = 0;
};
struct IDirect3DSurface9 : IDirect3DResource9 {
    virtual HRESULT GetDesc(D3DSURFACE_DESC *desc) = 0;
    virtual HRESULT LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) = 0;
    virtual HRESULT UnlockRect() = 0;
};
struct IDirect3DVertexBuffer9 : IDirect3DResource9 {
    virtual HRESULT Lock(UINT offset, UINT size, void **data, DWORD flags) = 0;
    virtual HRESULT Unlock() = 0;
    virtual HRESULT GetDesc(D3DVERTEXBUFFER_DESC *desc) = 0;
};
struct IDirect3DIndexBuffer9 : IDirect3DResource9 {
    virtual HRESULT Lock(UINT offset, UINT size, void **data, DWORD flags) = 0;
    virtual HRESULT Unlock() = 0;
    virtual HRESULT GetDesc(D3DINDEXBUFFER_DESC *desc) = 0;
};
struct IDirect3DVertexDeclaration9 : IUnknown {};
struct IDirect3DVertexShader9 : IUnknown {};
struct IDirect3DPixelShader9 : IUnknown {};
struct IDirect3DStateBlock9 : IUnknown {};
struct IDirect3DQuery9 : IUnknown {
    virtual HRESULT Issue(DWORD flags) = 0;
    virtual HRESULT GetData(void *data, DWORD size, DWORD flags) = 0;
};
struct IDirect3DSwapChain9 : IUnknown {
    virtual HRESULT Present(const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty,
                            DWORD flags) = 0;
    virtual HRESULT GetFrontBufferData(IDirect3DSurface9 *dest) = 0;
    virtual HRESULT GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface) = 0;
};

struct IDirect3DDevice9 : IUnknown {
    virtual HRESULT TestCooperativeLevel() = 0;
    virtual UINT GetAvailableTextureMem() = 0;
    virtual HRESULT GetDeviceCaps(D3DCAPS9 *caps) = 0;
    virtual HRESULT GetSwapChain(UINT index, IDirect3DSwapChain9 **swap_chain) = 0;
    virtual HRESULT Reset(D3DPRESENT_PARAMETERS *params) = 0;
    virtual HRESULT Present(const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty) = 0;
    virtual HRESULT GetBackBuffer(UINT swap_chain, UINT index, D3DBACKBUFFER_TYPE type,
                                  IDirect3DSurface9 **surface) = 0;
    virtual void SetGammaRamp(UINT swap_chain, DWORD flags, const D3DGAMMARAMP *ramp) = 0;

    virtual HRESULT CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
                                  D3DPOOL pool, IDirect3DTexture9 **texture, HANDLE *shared) = 0;
    virtual HRESULT CreateVolumeTexture(UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                        D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **texture,
                                        HANDLE *shared) = 0;
    virtual HRESULT CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                      IDirect3DCubeTexture9 **texture, HANDLE *shared) = 0;
    virtual HRESULT CreateVertexBuffer(UINT size, DWORD usage, DWORD fvf, D3DPOOL pool,
                                       IDirect3DVertexBuffer9 **buffer, HANDLE *shared) = 0;
    virtual HRESULT CreateIndexBuffer(UINT size, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                      IDirect3DIndexBuffer9 **buffer, HANDLE *shared) = 0;
    virtual HRESULT CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples,
                                       DWORD quality, BOOL lockable, IDirect3DSurface9 **surface,
                                       HANDLE *shared) = 0;
    virtual HRESULT CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format,
                                              D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL discard,
                                              IDirect3DSurface9 **surface, HANDLE *shared) = 0;
    virtual HRESULT CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DSurface9 **surface, HANDLE *shared) = 0;
    virtual HRESULT CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements,
                                            IDirect3DVertexDeclaration9 **decl) = 0;
    virtual HRESULT CreateVertexShader(const DWORD *bytecode, IDirect3DVertexShader9 **shader) = 0;
    virtual HRESULT CreatePixelShader(const DWORD *bytecode, IDirect3DPixelShader9 **shader) = 0;
    virtual HRESULT CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **query) = 0;

    virtual HRESULT UpdateSurface(IDirect3DSurface9 *source, const RECT *source_rect, IDirect3DSurface9 *dest,
                                  const POINT *dest_point) = 0;
    virtual HRESULT UpdateTexture(IDirect3DBaseTexture9 *source, IDirect3DBaseTexture9 *dest) = 0;
    virtual HRESULT GetRenderTargetData(IDirect3DSurface9 *render_target, IDirect3DSurface9 *dest) = 0;
    virtual HRESULT StretchRect(IDirect3DSurface9 *source, const RECT *source_rect, IDirect3DSurface9 *dest,
                                const RECT *dest_rect, D3DTEXTUREFILTERTYPE filter) = 0;

    virtual HRESULT SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) = 0;
    virtual HRESULT SetDepthStencilSurface(IDirect3DSurface9 *surface) = 0;
    virtual HRESULT BeginScene() = 0;
    virtual HRESULT EndScene() = 0;
    virtual HRESULT Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z,
                          DWORD stencil) = 0;
    virtual HRESULT SetViewport(const D3DVIEWPORT9 *viewport) = 0;
    virtual HRESULT SetScissorRect(const RECT *rect) = 0;
    virtual HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value) = 0;
    virtual HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture) = 0;
    virtual HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) = 0;
    virtual HRESULT SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) = 0;

    virtual HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl) = 0;
    virtual HRESULT SetFVF(DWORD fvf) = 0;
    virtual HRESULT SetVertexShader(IDirect3DVertexShader9 *shader) = 0;
    virtual HRESULT SetVertexShaderConstantF(UINT start, const float *data, UINT count) = 0;
    virtual HRESULT SetPixelShader(IDirect3DPixelShader9 *shader) = 0;
    virtual HRESULT SetPixelShaderConstantF(UINT start, const float *data, UINT count) = 0;
    virtual HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) = 0;
    virtual HRESULT SetIndices(IDirect3DIndexBuffer9 *buffer) = 0;
    virtual HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT start_vertex, UINT primitive_count) = 0;
    virtual HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base_vertex, UINT min_index,
                                         UINT vertex_count, UINT start_index, UINT primitive_count) = 0;
    virtual HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitive_count, const void *vertices,
                                    UINT stride) = 0;
};
struct IDirect3DDevice9Ex : IDirect3DDevice9 {};

struct IDirect3D9 : IUnknown {
    virtual UINT GetAdapterCount() = 0;
    virtual HRESULT GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *identifier) = 0;
    virtual UINT GetAdapterModeCount(UINT adapter, D3DFORMAT format) = 0;
    virtual HRESULT EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE *display_mode) = 0;
    virtual HRESULT GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *display_mode) = 0;
    virtual HRESULT CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage,
                                      D3DRESOURCETYPE resource_type, D3DFORMAT check_format) = 0;
    virtual HRESULT CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                               D3DMULTISAMPLE_TYPE samples, DWORD *quality_levels) = 0;
    virtual HRESULT CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format,
                                           D3DFORMAT render_target_format, D3DFORMAT depth_format) = 0;
    virtual HRESULT GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) = 0;
    virtual HMONITOR GetAdapterMonitor(UINT adapter) = 0;
    virtual HRESULT CreateDevice(UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior,
                                 D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **device) = 0;
};
struct IDirect3D9Ex : IDirect3D9 {
    virtual HRESULT CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior,
                                   D3DPRESENT_PARAMETERS *params, D3DDISPLAYMODEEX *fullscreen_mode,
                                   IDirect3DDevice9Ex **device) = 0;
};

extern "C" IDirect3D9 *Direct3DCreate9(UINT sdk_version);
/* PIX event markers */
static inline int D3DPERF_BeginEvent(D3DCOLOR color, const wchar_t *name) { (void)color; (void)name; return 0; }
static inline int D3DPERF_EndEvent() { return 0; }
extern "C" HRESULT Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex **d3d);
#else
typedef struct IDirect3DDevice9 IDirect3DDevice9;
typedef struct IDirect3DTexture9 IDirect3DTexture9;
typedef struct IDirect3DVertexBuffer9 IDirect3DVertexBuffer9;
typedef struct IDirect3DIndexBuffer9 IDirect3DIndexBuffer9;
#endif
typedef struct IDirect3DDevice9 *LPDIRECT3DDEVICE9;
typedef struct IDirect3DTexture9 *LPDIRECT3DTEXTURE9;
typedef struct IDirect3DVertexBuffer9 *LPDIRECT3DVERTEXBUFFER9;
typedef struct IDirect3DIndexBuffer9 *LPDIRECT3DINDEXBUFFER9;
