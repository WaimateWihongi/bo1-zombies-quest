/* The D3DX helpers the renderer uses. Screenshots (D3DXSaveSurfaceToFileA) come from the guest
   renderer. HLSL compiling only serves loose (non-fastfile) shaders, which the Quest never loads:
   D3DXCompileShader reports failure there. */
#pragma once
#include <d3d9.h>

typedef enum _D3DXIMAGE_FILEFORMAT {
    D3DXIFF_BMP = 0,
    D3DXIFF_JPG = 1,
    D3DXIFF_TGA = 2,
    D3DXIFF_PNG = 3,
    D3DXIFF_DDS = 4,
} D3DXIMAGE_FILEFORMAT;

/* a vertex shader input or output: D3DDECLUSAGE and its index */
typedef struct _D3DXSEMANTIC {
    UINT Usage;
    UINT UsageIndex;
} D3DXSEMANTIC, *LPD3DXSEMANTIC;

/* The 'CTAB' comment block inside shader bytecode. Every *Info/Name/Target member is a byte offset
   from the start of the table. */
typedef struct _D3DXSHADER_CONSTANTTABLE {
    DWORD Size, Creator, Version, Constants, ConstantInfo, Flags, Target;
} D3DXSHADER_CONSTANTTABLE;
typedef struct _D3DXSHADER_CONSTANTINFO {
    DWORD Name;
    WORD RegisterSet, RegisterIndex, RegisterCount, Reserved;
    DWORD TypeInfo, DefaultValue;
} D3DXSHADER_CONSTANTINFO;

#ifdef __cplusplus
struct ID3DXBuffer : IUnknown {
    virtual void *GetBufferPointer() = 0;
    virtual DWORD GetBufferSize() = 0;
};
typedef ID3DXBuffer *LPD3DXBUFFER;
/* the engine only reads the raw table through GetBufferPointer */
struct ID3DXConstantTable : ID3DXBuffer {};
typedef ID3DXConstantTable *LPD3DXCONSTANTTABLE;

extern "C" {
HRESULT D3DXSaveSurfaceToFileA(const char *path, D3DXIMAGE_FILEFORMAT format, IDirect3DSurface9 *surface,
                               const void *palette, const RECT *rect);
HRESULT D3DXCreateBuffer(DWORD size, LPD3DXBUFFER *buffer);
HRESULT D3DXCompileShader(const char *source, UINT size, const void *defines, void *include, const char *function,
                          const char *profile, DWORD flags, LPD3DXBUFFER *shader, LPD3DXBUFFER *errors,
                          LPD3DXCONSTANTTABLE *constants);
HRESULT D3DXGetShaderConstantTable(const void *function, LPD3DXCONSTANTTABLE *constants);
HRESULT D3DXGetShaderInputSemantics(const void *function, D3DXSEMANTIC *semantics, UINT *count);
HRESULT D3DXGetShaderOutputSemantics(const void *function, D3DXSEMANTIC *semantics, UINT *count);
}
#define D3DXSaveSurfaceToFile D3DXSaveSurfaceToFileA
#endif
