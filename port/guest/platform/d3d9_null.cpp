/* A Direct3D 9 device that draws nothing, so the engine's own renderer (gfx_d3d/) runs without a
 * GPU: d3d9_gles.cpp falls back to it when there's no OpenGL ES context.
 *
 * Resources are real: every lock returns memory sized for the format (DXT formats in 4x4 blocks),
 * because the engine's loaders write vertex, index and texture data through them. The adapter
 * reports a Shader Model 3 part with 512 MB, which is what the renderer's startup checks ask for. */
#include <windows.h>
#include <d3d9.h>

#include <stdlib.h>
#include <string.h>

#include "win32_internal.h"

/* r_caps.cpp checks the capabilities by byte offset, as laid out by Microsoft's D3DCAPS9 */
static_assert(offsetof(D3DCAPS9, Caps3) == 16, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, TextureCaps) == 60, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, TextureAddressCaps) == 76, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, MaxTextureWidth) == 88, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, StencilCaps) == 136, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, MaxStreams) == 188, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, VertexShaderVersion) == 196, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, PixelShaderVersion) == 204, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, DevCaps2) == 212, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, DeclTypes) == 236, "D3DCAPS9 layout");
static_assert(offsetof(D3DCAPS9, StretchRectFilterCaps) == 244, "D3DCAPS9 layout");

namespace {

/* ---- sizes ----------------------------------------------------------------------------------- */

bool is_block_compressed(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

/* bytes per pixel, or per 4x4 block for DXT */
UINT format_bytes(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_DXT1: return 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 16;
    case D3DFMT_A8: case D3DFMT_L8: return 1;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_INDEX16: return 2;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    default: return 4;
    }
}

UINT row_pitch(D3DFORMAT f, UINT width)
{
    return is_block_compressed(f) ? ((width + 3) / 4) * format_bytes(f) : width * format_bytes(f);
}

UINT row_count(D3DFORMAT f, UINT height) { return is_block_compressed(f) ? (height + 3) / 4 : height; }

UINT mip(UINT size, UINT level)
{
    size >>= level;
    return size ? size : 1;
}

UINT full_chain(UINT w, UINT h, UINT d = 1)
{
    UINT levels = 1;
    while (w > 1 || h > 1 || d > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        d = d > 1 ? d / 2 : 1;
        levels++;
    }
    return levels;
}

/* ---- reference counting ---------------------------------------------------------------------- */

template <typename Interface> struct Counted : Interface {
    ULONG refs = 1;
    virtual ~Counted() = default;
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
};

template <typename Interface> struct Resource : Counted<Interface> {
    D3DRESOURCETYPE type;
    explicit Resource(D3DRESOURCETYPE t) : type(t) {}
    D3DRESOURCETYPE GetType() override { return type; }
    void PreLoad() override {}
};

/* one level of an image: its memory, made on first lock */
struct Image {
    D3DFORMAT format;
    UINT width, height, depth;
    void *bits = NULL;
    ~Image() { free(bits); }
    UINT pitch() const { return row_pitch(format, width); }
    UINT slice() const { return pitch() * row_count(format, height); }
    void *memory()
    {
        if (!bits) bits = calloc(1, (size_t)slice() * depth);
        return bits;
    }
};

/* ---- surfaces and textures ------------------------------------------------------------------- */

struct Surface final : Resource<IDirect3DSurface9> {
    D3DSURFACE_DESC desc;
    Image own, *image;
    Surface(UINT w, UINT h, D3DFORMAT f, DWORD usage, D3DPOOL pool, Image *shared = NULL)
        : Resource(D3DRTYPE_SURFACE), own{f, w, h, 1}, image(shared ? shared : &own)
    {
        desc = {};
        desc.Format = f;
        desc.Type = D3DRTYPE_SURFACE;
        desc.Usage = usage;
        desc.Pool = pool;
        desc.Width = w;
        desc.Height = h;
    }
    HRESULT GetDesc(D3DSURFACE_DESC *d) override
    {
        *d = desc;
        return S_OK;
    }
    HRESULT LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD) override
    {
        char *base = (char *)image->memory();
        if (!base) return E_OUTOFMEMORY;
        locked->Pitch = (INT)image->pitch();
        size_t offset = 0;
        if (rect) {
            UINT bpp = format_bytes(image->format);
            offset = is_block_compressed(image->format)
                         ? (size_t)(rect->top / 4) * image->pitch() + (size_t)(rect->left / 4) * bpp
                         : (size_t)rect->top * image->pitch() + (size_t)rect->left * bpp;
        }
        locked->pBits = base + offset;
        return S_OK;
    }
    HRESULT UnlockRect() override { return S_OK; }
};

struct Texture final : Resource<IDirect3DTexture9> {
    D3DFORMAT format;
    DWORD usage;
    D3DPOOL pool;
    UINT width, height, levels;
    Image *images;
    Texture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p)
        : Resource(D3DRTYPE_TEXTURE), format(f), usage(u), pool(p), width(w), height(h),
          levels(l ? l : full_chain(w, h))
    {
        images = new Image[levels];
        for (UINT i = 0; i < levels; i++) images[i] = Image{f, mip(w, i), mip(h, i), 1};
    }
    ~Texture() override { delete[] images; }
    DWORD GetLevelCount() override { return levels; }
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *d) override
    {
        if (level >= levels) return E_INVALIDARG;
        *d = {};
        d->Format = format;
        d->Type = D3DRTYPE_SURFACE;
        d->Usage = usage;
        d->Pool = pool;
        d->Width = images[level].width;
        d->Height = images[level].height;
        return S_OK;
    }
    HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface) override
    {
        if (level >= levels) return E_INVALIDARG;
        *surface = new Surface(images[level].width, images[level].height, format, usage, pool, &images[level]);
        return S_OK;
    }
    HRESULT LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override
    {
        if (level >= levels) return E_INVALIDARG;
        Surface s(images[level].width, images[level].height, format, usage, pool, &images[level]);
        return s.LockRect(locked, rect, flags);
    }
    HRESULT UnlockRect(UINT) override { return S_OK; }
    HRESULT AddDirtyRect(const RECT *) override { return S_OK; }
};

struct CubeTexture final : Resource<IDirect3DCubeTexture9> {
    D3DFORMAT format;
    DWORD usage;
    D3DPOOL pool;
    UINT edge, levels;
    Image *images; /* [face * levels + level] */
    CubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p)
        : Resource(D3DRTYPE_CUBETEXTURE), format(f), usage(u), pool(p), edge(e), levels(l ? l : full_chain(e, e))
    {
        images = new Image[6 * levels];
        for (UINT face = 0; face < 6; face++)
            for (UINT i = 0; i < levels; i++) images[face * levels + i] = Image{f, mip(e, i), mip(e, i), 1};
    }
    ~CubeTexture() override { delete[] images; }
    DWORD GetLevelCount() override { return levels; }
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *d) override
    {
        if (level >= levels) return E_INVALIDARG;
        *d = {};
        d->Format = format;
        d->Type = D3DRTYPE_SURFACE;
        d->Usage = usage;
        d->Pool = pool;
        d->Width = d->Height = mip(edge, level);
        return S_OK;
    }
    HRESULT GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface) override
    {
        if (level >= levels || (UINT)face >= 6) return E_INVALIDARG;
        Image *image = &images[face * levels + level];
        *surface = new Surface(image->width, image->height, format, usage, pool, image);
        return S_OK;
    }
    HRESULT LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override
    {
        if (level >= levels || (UINT)face >= 6) return E_INVALIDARG;
        Image *image = &images[face * levels + level];
        Surface s(image->width, image->height, format, usage, pool, image);
        return s.LockRect(locked, rect, flags);
    }
    HRESULT UnlockRect(D3DCUBEMAP_FACES, UINT) override { return S_OK; }
};

struct VolumeTexture final : Resource<IDirect3DVolumeTexture9> {
    D3DFORMAT format;
    DWORD usage;
    D3DPOOL pool;
    UINT levels;
    Image *images;
    VolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p)
        : Resource(D3DRTYPE_VOLUMETEXTURE), format(f), usage(u), pool(p), levels(l ? l : full_chain(w, h, d))
    {
        images = new Image[levels];
        for (UINT i = 0; i < levels; i++) images[i] = Image{f, mip(w, i), mip(h, i), mip(d, i)};
    }
    ~VolumeTexture() override { delete[] images; }
    DWORD GetLevelCount() override { return levels; }
    HRESULT GetLevelDesc(UINT level, D3DVOLUME_DESC *d) override
    {
        if (level >= levels) return E_INVALIDARG;
        *d = {};
        d->Format = format;
        d->Type = D3DRTYPE_VOLUME;
        d->Usage = usage;
        d->Pool = pool;
        d->Width = images[level].width;
        d->Height = images[level].height;
        d->Depth = images[level].depth;
        return S_OK;
    }
    HRESULT LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD) override
    {
        if (level >= levels) return E_INVALIDARG;
        Image &image = images[level];
        char *base = (char *)image.memory();
        if (!base) return E_OUTOFMEMORY;
        locked->RowPitch = (INT)image.pitch();
        locked->SlicePitch = (INT)image.slice();
        size_t offset = box ? (size_t)box->Front * image.slice() + (size_t)box->Top * image.pitch() +
                                  (size_t)box->Left * format_bytes(format)
                            : 0;
        locked->pBits = base + offset;
        return S_OK;
    }
    HRESULT UnlockBox(UINT) override { return S_OK; }
};

/* ---- buffers --------------------------------------------------------------------------------- */

template <typename Interface, typename Desc> struct Buffer final : Resource<Interface> {
    Desc desc;
    void *data;
    Buffer(D3DRESOURCETYPE type, const Desc &d) : Resource<Interface>(type), desc(d), data(calloc(1, d.Size ? d.Size : 1)) {}
    ~Buffer() override { free(data); }
    HRESULT Lock(UINT offset, UINT, void **out, DWORD) override
    {
        if (!data) return E_OUTOFMEMORY;
        *out = (char *)data + offset;
        return S_OK;
    }
    HRESULT Unlock() override { return S_OK; }
    HRESULT GetDesc(Desc *d) override
    {
        *d = desc;
        return S_OK;
    }
};
typedef Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC> VertexBuffer;
typedef Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC> IndexBuffer;

/* ---- the rest -------------------------------------------------------------------------------- */

struct VertexDeclaration final : Counted<IDirect3DVertexDeclaration9> {};
struct VertexShader final : Counted<IDirect3DVertexShader9> {};
struct PixelShader final : Counted<IDirect3DPixelShader9> {};

struct Query final : Counted<IDirect3DQuery9> {
    HRESULT Issue(DWORD) override { return S_OK; }
    HRESULT GetData(void *data, DWORD size, DWORD) override
    {
        /* every query is complete: event queries read TRUE, occlusion queries "all pixels visible" */
        if (data && size) {
            memset(data, 0, size);
            if (size >= 4) *(DWORD *)data = size == 4 ? 1 : 0;
        }
        return S_OK;
    }
};

struct Device;

struct SwapChain final : Counted<IDirect3DSwapChain9> {
    Device *device;
    explicit SwapChain(Device *d) : device(d) {}
    HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *, DWORD) override { return S_OK; }
    HRESULT GetFrontBufferData(IDirect3DSurface9 *) override { return S_OK; }
    HRESULT GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface) override;
};

void fill_caps(D3DCAPS9 *caps)
{
    memset(caps, 0, sizeof *caps);
    caps->DeviceType = D3DDEVTYPE_HAL;
    caps->Caps2 = 0x20000000 | 0x40000000 | 0x20000; /* dynamic textures, autogen mipmaps, full-screen gamma */
    caps->Caps3 = 0x20 | 0x80 | 0x100 | 0x200;       /* alpha flip/discard, linear->sRGB, copy to vidmem/sysmem */
    caps->PresentationIntervals = 0x80000001; /* immediate, one */
    caps->DevCaps = 0x000FFFFF;
    caps->PrimitiveMiscCaps = 0x003FFFFF;
    caps->RasterCaps = 0x07FFFFFF;
    caps->ZCmpCaps = caps->AlphaCmpCaps = 0xFF;
    caps->SrcBlendCaps = caps->DestBlendCaps = 0x3FFF;
    caps->ShadeCaps = 0x000FFFFF;
    caps->TextureCaps = 0x0001EDC5 & ~0x2u & ~0x100u; /* no POW2 or NONPOW2CONDITIONAL limits */
    caps->TextureFilterCaps = caps->CubeTextureFilterCaps = caps->VolumeTextureFilterCaps = 0x07070707;
    caps->TextureAddressCaps = caps->VolumeTextureAddressCaps = 0x3F;
    caps->LineCaps = 0x3F;
    caps->MaxTextureWidth = caps->MaxTextureHeight = 8192;
    caps->MaxVolumeExtent = 2048;
    caps->MaxTextureRepeat = 8192;
    caps->MaxTextureAspectRatio = 8192;
    caps->MaxAnisotropy = 16;
    caps->MaxVertexW = 1e10f;
    caps->GuardBandLeft = caps->GuardBandTop = -1e8f;
    caps->GuardBandRight = caps->GuardBandBottom = 1e8f;
    caps->StencilCaps = 0x1FF;
    caps->FVFCaps = 0x00180008;
    caps->TextureOpCaps = 0x03FFFFFF;
    caps->MaxTextureBlendStages = caps->MaxSimultaneousTextures = 8;
    caps->VertexProcessingCaps = 0x17B;
    caps->MaxUserClipPlanes = 8;
    caps->MaxPointSize = 8192.0f;
    caps->MaxPrimitiveCount = caps->MaxVertexIndex = 0x00FFFFFF;
    caps->MaxStreams = 16;
    caps->MaxStreamStride = 508;
    caps->VertexShaderVersion = 0xFFFE0300; /* vs_3_0 */
    caps->MaxVertexShaderConst = 256;
    caps->PixelShaderVersion = 0xFFFF0300; /* ps_3_0 */
    caps->PixelShader1xMaxValue = 65504.0f;
    caps->DevCaps2 = 0x71;
    caps->NumberOfAdaptersInGroup = 1;
    caps->DeclTypes = 0x3FF;
    caps->NumSimultaneousRTs = 4;
    caps->StretchRectFilterCaps = 0x03030303;
    caps->VertexTextureFilterCaps = 0x03030303;
    caps->MaxVShaderInstructionsExecuted = caps->MaxPShaderInstructionsExecuted = 0xFFFF;
    caps->MaxVertexShader30InstructionSlots = caps->MaxPixelShader30InstructionSlots = 32768;
}

struct Device final : Counted<IDirect3DDevice9Ex> {
    D3DPRESENT_PARAMETERS params;
    Surface *back_buffer = NULL, *depth = NULL;

    explicit Device(const D3DPRESENT_PARAMETERS &p) { make_buffers(p); }
    ~Device() override { drop_buffers(); }

    void make_buffers(const D3DPRESENT_PARAMETERS &p)
    {
        params = p;
        if (!params.BackBufferWidth) params.BackBufferWidth = 1920;
        if (!params.BackBufferHeight) params.BackBufferHeight = 1080;
        if (params.BackBufferFormat == D3DFMT_UNKNOWN) params.BackBufferFormat = D3DFMT_X8R8G8B8;
        back_buffer = new Surface(params.BackBufferWidth, params.BackBufferHeight, params.BackBufferFormat,
                                  1 /* D3DUSAGE_RENDERTARGET */, D3DPOOL_DEFAULT);
        if (params.EnableAutoDepthStencil)
            depth = new Surface(params.BackBufferWidth, params.BackBufferHeight, params.AutoDepthStencilFormat,
                                2 /* D3DUSAGE_DEPTHSTENCIL */, D3DPOOL_DEFAULT);
    }
    void drop_buffers()
    {
        if (back_buffer) back_buffer->Release();
        if (depth) depth->Release();
        back_buffer = depth = NULL;
    }

    HRESULT TestCooperativeLevel() override { return S_OK; }
    UINT GetAvailableTextureMem() override { return 512u << 20; }
    HRESULT GetDeviceCaps(D3DCAPS9 *caps) override
    {
        fill_caps(caps);
        return S_OK;
    }
    HRESULT GetSwapChain(UINT, IDirect3DSwapChain9 **swap_chain) override
    {
        *swap_chain = new SwapChain(this);
        return S_OK;
    }
    HRESULT Reset(D3DPRESENT_PARAMETERS *p) override
    {
        drop_buffers();
        make_buffers(*p);
        return S_OK;
    }
    HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *) override { return S_OK; }
    HRESULT GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **surface) override
    {
        back_buffer->AddRef();
        *surface = back_buffer;
        return S_OK;
    }
    void SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) override {}

    HRESULT CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                          IDirect3DTexture9 **texture, HANDLE *) override
    {
        *texture = new Texture(w, h, levels, usage, f, pool);
        return S_OK;
    }
    HRESULT CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                                IDirect3DVolumeTexture9 **texture, HANDLE *) override
    {
        *texture = new VolumeTexture(w, h, d, levels, usage, f, pool);
        return S_OK;
    }
    HRESULT CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                              IDirect3DCubeTexture9 **texture, HANDLE *) override
    {
        *texture = new CubeTexture(edge, levels, usage, f, pool);
        return S_OK;
    }
    HRESULT CreateVertexBuffer(UINT size, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **buffer,
                               HANDLE *) override
    {
        D3DVERTEXBUFFER_DESC d = {};
        d.Format = (D3DFORMAT)100; /* D3DFMT_VERTEXDATA */
        d.Type = D3DRTYPE_VERTEXBUFFER;
        d.Usage = usage;
        d.Pool = pool;
        d.Size = size;
        d.FVF = fvf;
        *buffer = new VertexBuffer(D3DRTYPE_VERTEXBUFFER, d);
        return S_OK;
    }
    HRESULT CreateIndexBuffer(UINT size, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DIndexBuffer9 **buffer,
                              HANDLE *) override
    {
        D3DINDEXBUFFER_DESC d = {};
        d.Format = f;
        d.Type = D3DRTYPE_INDEXBUFFER;
        d.Usage = usage;
        d.Pool = pool;
        d.Size = size;
        *buffer = new IndexBuffer(D3DRTYPE_INDEXBUFFER, d);
        return S_OK;
    }
    HRESULT CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                               IDirect3DSurface9 **surface, HANDLE *) override
    {
        *surface = new Surface(w, h, f, 1, D3DPOOL_DEFAULT);
        return S_OK;
    }
    HRESULT CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                      IDirect3DSurface9 **surface, HANDLE *) override
    {
        *surface = new Surface(w, h, f, 2, D3DPOOL_DEFAULT);
        return S_OK;
    }
    HRESULT CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL pool, IDirect3DSurface9 **surface,
                                        HANDLE *) override
    {
        *surface = new Surface(w, h, f, 0, pool);
        return S_OK;
    }
    HRESULT CreateVertexDeclaration(const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **decl) override
    {
        *decl = new VertexDeclaration();
        return S_OK;
    }
    HRESULT CreateVertexShader(const DWORD *, IDirect3DVertexShader9 **shader) override
    {
        *shader = new VertexShader();
        return S_OK;
    }
    HRESULT CreatePixelShader(const DWORD *, IDirect3DPixelShader9 **shader) override
    {
        *shader = new PixelShader();
        return S_OK;
    }
    HRESULT CreateQuery(D3DQUERYTYPE, IDirect3DQuery9 **query) override
    {
        if (query) *query = new Query();
        return S_OK;
    }
    HRESULT UpdateSurface(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *) override { return S_OK; }
    HRESULT UpdateTexture(IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *) override { return S_OK; }
    HRESULT GetRenderTargetData(IDirect3DSurface9 *, IDirect3DSurface9 *) override { return S_OK; }
    HRESULT StretchRect(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE) override
    {
        return S_OK;
    }
    HRESULT SetRenderTarget(DWORD, IDirect3DSurface9 *) override { return S_OK; }
    HRESULT SetDepthStencilSurface(IDirect3DSurface9 *) override { return S_OK; }
    HRESULT BeginScene() override { return S_OK; }
    HRESULT EndScene() override { return S_OK; }
    HRESULT Clear(DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD) override { return S_OK; }
    HRESULT SetViewport(const D3DVIEWPORT9 *) override { return S_OK; }
    HRESULT SetScissorRect(const RECT *) override { return S_OK; }
    HRESULT SetRenderState(D3DRENDERSTATETYPE, DWORD) override { return S_OK; }
    HRESULT SetTexture(DWORD, IDirect3DBaseTexture9 *) override { return S_OK; }
    HRESULT SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override { return S_OK; }
    HRESULT SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD) override { return S_OK; }
    HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9 *) override { return S_OK; }
    HRESULT SetFVF(DWORD) override { return S_OK; }
    HRESULT SetVertexShader(IDirect3DVertexShader9 *) override { return S_OK; }
    HRESULT SetVertexShaderConstantF(UINT, const float *, UINT) override { return S_OK; }
    HRESULT SetPixelShader(IDirect3DPixelShader9 *) override { return S_OK; }
    HRESULT SetPixelShaderConstantF(UINT, const float *, UINT) override { return S_OK; }
    HRESULT SetStreamSource(UINT, IDirect3DVertexBuffer9 *, UINT, UINT) override { return S_OK; }
    HRESULT SetIndices(IDirect3DIndexBuffer9 *) override { return S_OK; }
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT) override { return S_OK; }
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) override { return S_OK; }
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, const void *, UINT) override { return S_OK; }
};

HRESULT SwapChain::GetBackBuffer(UINT, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface)
{
    return device->GetBackBuffer(0, 0, type, surface);
}

} // namespace

/* for d3d9_gles.cpp: the device when there's no GL, and the capabilities both report */
IDirect3DDevice9 *bo1_d3d9_null_device(D3DPRESENT_PARAMETERS *params) { return new Device(*params); }
void bo1_d3d9_fill_caps(D3DCAPS9 *caps) { fill_caps(caps); }
