/* Direct3D 9 on OpenGL ES 3.2: the device the engine's own renderer (gfx_d3d/) draws through.
 *
 * Threads. GL contexts belong to one thread; the engine makes resources on its loader thread and
 * draws on its render thread. So every resource keeps its data in CPU memory and its GL object
 * is made (and filled) on the GL thread when a draw first needs it. The GL thread is whichever
 * thread first does GL work (a clear, a draw, a present): the render thread. Releases from other
 * threads queue their GL names for deletion there.
 *
 * Shaders. The fastfiles' shaders are D3D9 shader-model-3 bytecode; MojoShader turns them into
 * GLSL ES 3.00 and manages their constants and vertex inputs. Two D3D9 features are added on
 * top: vertex colours (D3DCOLOR is BGRA in memory: MojoShader swizzles that input per vertex
 * declaration) and the alpha test (fixed-function in D3D9, missing from GL ES: injected into each
 * pixel shader as a comparison against two uniforms).
 *
 * Coordinates. Everything renders into framebuffer objects, the back buffer included, upside
 * down by GL's convention so that row 0 in memory is D3D's top row (MojoShader's vpFlip). Then
 * textures rendered to and textures loaded from files sample alike, viewports and scissors use
 * D3D's numbers directly, and Present flips the back buffer once onto the window. Depth is
 * mapped from D3D's [0,1] to GL's [-1,1] in each vertex shader. */
#include <windows.h>
#include <d3d9.h>

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <mojoshader.h>
#include "gl_state_cache.h" /* from here on, GL state calls skip what wouldn't change */
#include <algorithm>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <sys/stat.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "win32_internal.h"

extern "C" unsigned int host_gl_begin(unsigned int *size);
extern "C" void host_gl_present(void);
extern "C" int32_t host_gl_has_multiview(void);
extern "C" void host_gl_multiview_attach(uint32_t target, uint32_t attachment, uint32_t texture, int32_t level,
                                         int32_t base_view, int32_t views);
extern bool g_bo1_mv_capable;      /* quest_vr.cpp: the driver can draw both eyes in one pass */
void bo1_vr_present_eye(int eye);  /* quest_vr.cpp: which eye's picture Present hands over next */
/* where the finished picture goes: a framebuffer (0 = the window, 0xFFFFFFFF = nowhere) and
   size[0..2] = width, height, sRGB target */
extern "C" unsigned int host_gl_frame_begin(unsigned int *size);
extern "C" unsigned int host_xr_panel_begin(unsigned int *size);
extern "C" void host_xr_panel_end(void);
/* the panel (stereo): while set, what the engine draws into the back buffer (its 2D: menus, HUD) goes
   into a transparent image of its own, which the left eye's Present hands to the headset as a panel
   floating in the world (host_xr.c). Set by the back end around its 2D commands (rb_backend.cpp). */
int g_bo1_ui_redirect;
extern "C" void bo1_d3d9_ui(int on);
extern "C" void host_gl_stream_write(unsigned int offset, unsigned int size, const void *data);
extern "C" unsigned int host_gl_stream_create(unsigned int size);
extern "C" void host_gl_stream_fence(unsigned int part);
extern "C" void host_gl_stream_wait(unsigned int part);
extern "C" void *bo1_gl_lookup(const char *name);
uint64_t g_bo1_backend_start_us; /* quest_vr.cpp: when the render thread got this frame's data */
extern int g_bo1_render_eye; /* quest_vr.cpp */
IDirect3DDevice9 *bo1_d3d9_null_device(D3DPRESENT_PARAMETERS *params);
void bo1_d3d9_fill_caps(D3DCAPS9 *caps);

#define D3DERR_NOTAVAILABLE ((HRESULT)0x8876086A)
#define D3DERR_INVALIDCALL ((HRESULT)0x8876086C)
#define D3DVERTEXTEXTURESAMPLER0 257
#define MAX_SAMPLERS 20 /* 16 pixel + 4 vertex (units 16-19) */

unsigned g_debug_flags; /* <data>/d3d9_debug; see the depth state in apply_state (also quest_vr.cpp) */

namespace {

/* ---- the GL thread and its housekeeping ------------------------------------------------------ */

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* deletions, the framebuffer cache */
pthread_t g_gl_thread;
bool g_gl_owned, g_gl_failed;
unsigned int g_window_size[2];
bool g_has_s3tc, g_has_aniso;

enum GLKind { GL_KIND_TEXTURE, GL_KIND_BUFFER, GL_KIND_RENDERBUFFER, GL_KIND_FRAMEBUFFER };
std::vector<std::pair<GLKind, GLuint>> g_deletions;

void delete_later(GLKind kind, GLuint name)
{
    if (!name) return;
    pthread_mutex_lock(&g_lock);
    g_deletions.push_back({kind, name});
    pthread_mutex_unlock(&g_lock);
}

void run_deletions()
{
    pthread_mutex_lock(&g_lock);
    std::vector<std::pair<GLKind, GLuint>> pending;
    pending.swap(g_deletions);
    pthread_mutex_unlock(&g_lock);
    for (auto &d : pending) {
        switch (d.first) {
        case GL_KIND_TEXTURE: glDeleteTextures(1, &d.second); break;
        case GL_KIND_BUFFER: glDeleteBuffers(1, &d.second); break;
        case GL_KIND_RENDERBUFFER: glDeleteRenderbuffers(1, &d.second); break;
        case GL_KIND_FRAMEBUFFER: glDeleteFramebuffers(1, &d.second); break;
        }
    }
}

bool has_extension(const char *name)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name)) return true;
    }
    return false;
}

/* ---- shader source hooks: the alpha test ----------------------------------------------------- */

std::unordered_map<GLuint, GLenum> g_shader_types;
std::string g_last_injected; /* for link-failure reports */
struct ShaderSource {
    std::string source;
    bool compiled;
};
std::unordered_map<GLuint, ShaderSource> g_shader_sources; /* MojoShader's shaders, compiled when a link needs them */

GLuint GL_APIENTRY hooked_glCreateShader(GLenum type)
{
    GLuint s = glCreateShader(type);
    g_shader_types[s] = type;
    return s;
}

/* bo1_alpha: x = the reference, y/z/w = 1 to keep alpha below / equal to / above it. Every D3D
   compare function is a set of those three; all ones = no alpha test. Float-only on purpose: the
   Adreno driver failed to link (with an empty log) an int-uniform if/else version of this. */
const char ALPHA_TEST_MAIN[] =
    "\nuniform highp vec4 bo1_alpha;\n"
    "void main()\n"
    "{\n"
    "    bo1_ps_main();\n"
    "    highp float d = _gl_FragData_0.a - bo1_alpha.x;\n"
    "    highp vec3 side = vec3(step(d, -0.002), step(abs(d), 0.002), step(0.002, d));\n"
    "    if (dot(side, bo1_alpha.yzw) < 0.5) discard;\n"
    "}\n";

/* D3DCMP_* (1 NEVER .. 8 ALWAYS) as below/equal/above */
void alpha_sides(DWORD func, float *sides)
{
    static const float table[9][3] = {{1, 1, 1}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                      {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    memcpy(sides, table[func < 9 ? func : 8], sizeof table[0]);
}

/* ---- multiview shader variants ------------------------------------------------------------------
 * The device asks MojoShader for a shader with g_compile_mode set; its GLSL is rewritten here:
 * multiview (both eyes in one pass, gl_ViewID_OVR): the vertex shader declares two views, and every
 * read of the shader's constants (MojoShader packs them into one array, xs_uniforms_vec4) picks the
 * right eye's copy (bo1_e1_xs, uploaded by the device) in view 1. Layered textures (the eyes'
 * render targets, two-layer arrays): their samplers become sampler2DArray, read at the view's layer
 * (layer 0 outside multiview). */
/* multiview: the right eye's clip-space fix (identity, or quest_multiview.cpp's re-projection of what
   the CPU placed in the left eye's clip space) */
float g_clip1[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
extern "C" void bo1_d3d9_clip1(const float *m)
{
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(g_clip1, m ? m : identity, sizeof g_clip1);
}

struct CompileMode {
    bool multiview = false;
    unsigned layered = 0; /* pixel shaders: the sampler units holding layered textures */
};
CompileMode g_compile_mode;

/* the index of the ']' closing the '[' at open, or npos */
size_t closing_bracket(const std::string &s, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == '[') depth++;
        else if (s[i] == ']' && --depth == 0) return i;
    }
    return std::string::npos;
}

/* every read of array `name` (not its declaration) as (bo1_view1 ? twin[i] : name[i]); the twin
   declared next to it, the same size */
void twin_uniform_array(std::string &source, const char *name, const char *twin)
{
    std::string decl = std::string("uniform vec4 ") + name + "[";
    size_t d = source.find(decl);
    if (d == std::string::npos) return;
    size_t decl_end = source.find(';', d);
    if (decl_end == std::string::npos) return;
    std::string size = source.substr(d + decl.size(), source.find(']', d) - d - decl.size());
    std::string twin_decl = std::string("\nuniform vec4 ") + twin + "[" + size + "];";
    source.insert(decl_end + 1, twin_decl);
    size_t skip_from = d, skip_to = decl_end + 1 + twin_decl.size();
    std::string use = std::string(name) + "[";
    for (size_t at = source.find(use); at != std::string::npos;) {
        if (at >= skip_from && at < skip_to) {
            at = source.find(use, skip_to);
            continue;
        }
        size_t open = at + use.size() - 1, close = closing_bracket(source, open);
        if (close == std::string::npos) break;
        std::string index = source.substr(open + 1, close - open - 1);
        std::string replaced = std::string("(bo1_view1 ? ") + twin + "[" + index + "] : " + name + "[" + index + "])";
        source.replace(at, close + 1 - at, replaced);
        at = source.find(use, at + replaced.size());
    }
}

void replace_all(std::string &s, const std::string &from, const std::string &to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

void rewrite_for_mode(std::string &source, bool fragment)
{
    const CompileMode &m = g_compile_mode;
    if (!m.multiview && !(fragment && m.layered)) return;
    size_t version_end = source.find('\n', source.find("#version"));
    if (version_end == std::string::npos) return;
    std::string head;
    if (m.multiview) {
        /* What the Quest's Adreno driver takes in multiview (found by testing): gl_Position written
           once (MojoShader reads it back to flip and remap it; with that the second view's other
           outputs arrived as garbage), and gl_ViewID_OVR read only in the vertex shader (in a pixel
           shader it is always 0). So the vertex shader works its position out with the view's
           constants, everything else with the left eye's (two runs of its body), and hands the
           view to the pixel shader as a flat varying, which picks its constants and layers by it. */
        if (!fragment) {
            head = "#extension GL_OVR_multiview2 : require\nlayout(num_views = 2) in;\n"
                   "flat out highp uint bo1_vid;\nuniform highp mat4 bo1_clip1;\n"
                   "bool bo1_sel;\nhighp vec4 bo1_pos;\n#define bo1_view1 bo1_sel\n#define bo1_layer 0.0\n";
            /* the position in a variable of its own, written to gl_Position once at the end (MojoShader
               reads gl_Position back to flip and remap it, which the driver's multiview doesn't take) */
            replace_all(source, "gl_Position", "bo1_pos");
            size_t at = source.rfind("void main()");
            if (at != std::string::npos) {
                source.replace(at, 11, "void bo1_vs_main()");
                source += "\nvoid main()\n{\n    bo1_sel = gl_ViewID_OVR == 1u;\n    bo1_vs_main();\n"
                          "    highp vec4 bo1_position = bo1_pos;\n    bo1_sel = false;\n    bo1_vs_main();\n"
                          /* the fix is in D3D's clip space: MojoShader's flip of y and remap of z undone
                             around it */
                          "    if (gl_ViewID_OVR == 1u) {\n"
                          "        bo1_position.y *= vpFlip;\n"
                          "        bo1_position.z = (bo1_position.z + bo1_position.w) * 0.5;\n"
                          "        bo1_position = bo1_clip1 * bo1_position;\n"
                          "        bo1_position.z = bo1_position.z * 2.0 - bo1_position.w;\n"
                          "        bo1_position.y *= vpFlip;\n"
                          "    }\n"
                          "    gl_Position = bo1_position;\n"
                          "    bo1_vid = gl_ViewID_OVR;\n}\n";
            }
        } else {
            head = "flat in highp uint bo1_vid;\n#define bo1_view1 (bo1_vid == 1u)\n#define bo1_layer float(bo1_vid)\n";
        }
        if (g_debug_flags & (1u << 30)) { /* debug: the left eye's constants everywhere, layers per view */
            size_t at = head.find("#define bo1_view1");
            head = head.substr(0, at) + "#define bo1_view1 false\n" + head.substr(head.find('\n', at) + 1);
        }
    } else {
        head = "#define bo1_view1 false\nuniform highp float bo1_layer_u;\n#define bo1_layer bo1_layer_u\n";
    }
    source.insert(version_end + 1, head);
    if (m.multiview && fragment && (g_debug_flags & (1u << 22))) {
        /* debug: show the shader's first constant (the same for both views) instead of the picture */
        size_t at = source.rfind("void main()");
        if (at != std::string::npos && source.find("_gl_FragData_0") != std::string::npos) {
            source.replace(at, 11, "void bo1_dbg_main()");
            bool has = source.find("uniform vec4 ps_uniforms_vec4[") != std::string::npos;
            source += std::string("\nvoid main()\n{\n    bo1_dbg_main();\n    _gl_FragData_0 = ") +
                      (has ? "vec4(fract(abs(ps_uniforms_vec4[0].xyz) * 0.37 + 0.05), 1.0);" : "vec4(0.5, 0.5, 0.5, 1.0);") + "\n}\n";
        }
    }
    if (m.multiview && fragment && (g_debug_flags & (1u << 23))) {
        /* debug: show unit 0's texture, tiled over the screen, instead of the picture */
        size_t at = source.rfind("void main()");
        if (at != std::string::npos && source.find("_gl_FragData_0") != std::string::npos) {
            source.replace(at, 11, "void bo1_dbg_main()");
            bool has = source.find("uniform sampler2D ps_s0;") != std::string::npos;
            source += std::string("\nvoid main()\n{\n    bo1_dbg_main();\n    _gl_FragData_0 = ") +
                      (has ? "vec4(texture(ps_s0, gl_FragCoord.xy / 256.0).rgb, 1.0);" : "vec4(0.0, 0.0, 0.3, 1.0);") + "\n}\n";
        }
    }
    if (fragment && (g_debug_flags & (3u << 8)) && source.find("vec4(64.0, 8.0, 0.0, 0.0)") != std::string::npos) {
        /* debug: the final composite shows one of its inputs: bit 8 the first sampler, bit 9 the second */
        size_t at = source.rfind("void main()");
        if (at != std::string::npos) {
            source.replace(at, 11, "void bo1_dbg_main()");
            if ((g_debug_flags & (3u << 8)) == (3u << 8)) {
                /* both bits: its constants c5, c6, c7 in thirds of the screen */
                source += "\nvoid main()\n{\n    bo1_dbg_main();\n    highp float x = gl_FragCoord.x / 1920.0;\n"
                          "    _gl_FragData_0 = vec4(abs(x < 0.333 ? ps_c5.xyz : x < 0.667 ? ps_c6.xyz : ps_c7.xyz) * 0.5, 1.0);\n}\n";
            } else {
                bool second = (g_debug_flags & (1u << 9)) != 0;
                bool layered = source.find(second ? "bo1_tex_ps_s1(" : "bo1_tex_ps_s0(") != std::string::npos;
                std::string call = layered ? (second ? "bo1_tex_ps_s1(ps_v0.xy)" : "bo1_tex_ps_s0(ps_v0.xy)")
                                           : (second ? "texture(ps_s1, ps_v0.xy)" : "texture(ps_s0, ps_v0.xy)");
                source += std::string("\nvoid main()\n{\n    bo1_dbg_main();\n    _gl_FragData_0 = vec4(") + call + ".rgb * " +
                          (second ? "4.0" : "16.0") + ", 1.0);\n}\n";
            }
        }
    }
    if (m.multiview && fragment && (g_debug_flags & (1u << 11))) {
        /* debug: the first input from the vertex shader as the colour */
        size_t at = source.rfind("void main()");
        size_t in = source.find("\nin ");
        if (at != std::string::npos && in != std::string::npos && in < at && source.find("_gl_FragData_0") != std::string::npos) {
            size_t semi = source.find(';', in);
            size_t name = source.rfind(' ', semi) + 1;
            std::string var = source.substr(name, semi - name);
            if (source.substr(in, semi - in).find("vec4") != std::string::npos) {
                source.replace(at, 11, "void bo1_dbg_main()");
                source += std::string("\nvoid main()\n{\n    bo1_dbg_main();\n    _gl_FragData_0 = vec4(fract(") + var + ".xy), fract(" +
                          var + ".z), 1.0);\n}\n";
            }
        }
    }
    if (m.multiview && fragment && (g_debug_flags & (1u << 12))) {
        /* debug: red 1, green the view index / 4, blue 0.5 (black: not drawn) */
        size_t at = source.rfind("void main()");
        if (at != std::string::npos && source.find("_gl_FragData_0") != std::string::npos) {
            source.replace(at, 11, "void bo1_dbg_main()");
            source += "\nvoid main()\n{\n    bo1_dbg_main();\n    _gl_FragData_0 = vec4(1.0, float(gl_ViewID_OVR) * 0.25, 0.5, 1.0);\n}\n";
        }
    }
    if (m.multiview && fragment && (g_debug_flags & (1u << 18))) {
        /* debug: tint by view (red the first, green the second) */
        size_t at = source.rfind("void main()");
        if (at != std::string::npos && source.find("_gl_FragData_0") != std::string::npos) {
            source.replace(at, 11, "void bo1_dbg_main()");
            source += "\nvoid main()\n{\n    bo1_dbg_main();\n    _gl_FragData_0 = vec4(bo1_vid == 0u ? 1.0 : 0.0, bo1_vid == 1u ? 1.0 : 0.0, 0.0, 1.0);\n}\n";
        }
    }
    /* only the vertex shader's constants get a right-eye copy: in a pixel shader the driver's view 1
       read such a second array as zeros (though it was set and read back right), and what differs per
       eye there (near-plane and pixel-to-world constants for depth effects) barely shows */
    if (m.multiview && !fragment) twin_uniform_array(source, "vs_uniforms_vec4", "bo1_e1_vs");
    if (!fragment) return;
    for (unsigned unit = 0; unit < 16; unit++) {
        if (!(m.layered >> unit & 1)) continue;
        char sampler[16], decl[48];
        snprintf(sampler, sizeof sampler, "ps_s%u", unit);
        snprintf(decl, sizeof decl, "uniform sampler2D %s;", sampler);
        size_t d = source.find(decl);
        if (d == std::string::npos) continue;
        std::string s = sampler;
        std::string helpers = std::string("uniform highp sampler2DArray ") + s + ";\n";
        helpers += std::string("vec4 bo1_tex_") + s + "(vec2 c) { return texture(" + s + ", vec3(c, bo1_layer)); }\n";
        helpers += std::string("vec4 bo1_tex_") + s + "(vec2 c, float b) { return texture(" + s + ", vec3(c, bo1_layer), b); }\n";
        helpers += std::string("vec4 bo1_texproj_") + s + "(vec3 c) { return texture(" + s + ", vec3(c.xy / c.z, bo1_layer)); }\n";
        helpers += std::string("vec4 bo1_texproj_") + s + "(vec4 c) { return texture(" + s + ", vec3(c.xy / c.w, bo1_layer)); }\n";
        helpers += std::string("vec4 bo1_texlod_") + s + "(vec2 c, float l) { return textureLod(" + s + ", vec3(c, bo1_layer), l); }\n";
        helpers += std::string("vec4 bo1_texgrad_") + s + "(vec2 c, vec2 x, vec2 y) { return textureGrad(" + s + ", vec3(c, bo1_layer), x, y); }\n";
        source.replace(d, strlen(decl), helpers);
        size_t after = d + helpers.size();
        std::string body = source.substr(after);
        replace_all(body, std::string("texture2DLod(") + s + ",", std::string("bo1_texlod_") + s + "(");
        replace_all(body, std::string("texture2DProj(") + s + ",", std::string("bo1_texproj_") + s + "(");
        replace_all(body, std::string("texture2DGrad(") + s + ",", std::string("bo1_texgrad_") + s + "(");
        replace_all(body, std::string("texture2D(") + s + ",", std::string("bo1_tex_") + s + "(");
        source.replace(after, std::string::npos, body);
    }
}

void GL_APIENTRY hooked_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    std::string source;
    for (GLsizei i = 0; i < count; i++)
        source.append(strings[i], lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]));
    rewrite_for_mode(source, g_shader_types[shader] == GL_FRAGMENT_SHADER);
    if (g_compile_mode.multiview && (g_debug_flags & (1u << 31))) {
        /* debug: the first few multiview shaders as written, to <data>/mv_shaders/ */
        static int dumped;
        if (dumped < 300 && (g_shader_types[shader] != GL_FRAGMENT_SHADER || source.find("sampler2DArray") != std::string::npos)) {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s/mv_shaders", bo1_data_root());
            mkdir(path, 0777);
            snprintf(path, sizeof path, "%s/mv_shaders/%02d_%s.glsl", bo1_data_root(), dumped++,
                     g_shader_types[shader] == GL_FRAGMENT_SHADER ? "ps" : "vs");
            if (FILE *f = fopen(path, "w")) {
                fputs(source.c_str(), f);
                fclose(f);
            }
        }
    }
    size_t main_at = source.rfind("void main()");
    if (g_shader_types[shader] == GL_FRAGMENT_SHADER && main_at != std::string::npos &&
        source.find("_gl_FragData_0") != std::string::npos) {
        source.replace(main_at, 11, "void bo1_ps_main()");
        source += ALPHA_TEST_MAIN;
        g_last_injected = source;
    }
    const GLchar *text = source.c_str();
    glShaderSource(shader, 1, &text, NULL);
    g_shader_sources[shader] = {source, false};
}

/* ---- the program cache: linked programs kept on disk ------------------------------------------
 * The Adreno driver compiles at glLinkProgram, 20-50 ms a program, and a zombies map meets new
 * shader pairs for minutes (stutters, ~13% of the render thread in Kino). MojoShader's GL calls come
 * through gl_lookup, so: glCompileShader only notes the shader, and glLinkProgram first tries
 * <data>/shader_cache/<hash of both sources>.bin with glProgramBinary; only when that is missing
 * or refused (a driver update) does it compile the shaders and link, saving the binary. MojoShader
 * asks for locations after linking, which works the same on a program from a binary. */
std::unordered_map<GLuint, std::vector<GLuint>> g_program_shaders;
unsigned g_cache_hits, g_cache_misses; /* for the 5 s stats */

void compile_pending(GLuint shader)
{
    auto f = g_shader_sources.find(shader);
    if (f == g_shader_sources.end() || f->second.compiled) return;
    f->second.compiled = true;
    glCompileShader(shader);
}

void GL_APIENTRY hooked_glCompileShader(GLuint shader)
{
    if (g_shader_sources.find(shader) == g_shader_sources.end()) glCompileShader(shader);
}

void GL_APIENTRY hooked_glGetShaderiv(GLuint shader, GLenum pname, GLint *out)
{
    auto f = g_shader_sources.find(shader);
    if (pname == GL_COMPILE_STATUS && f != g_shader_sources.end() && !f->second.compiled) {
        *out = GL_TRUE; /* compiled at link time if it's needed at all; a failure shows as the link's */
        return;
    }
    compile_pending(shader);
    glGetShaderiv(shader, pname, out);
}

void GL_APIENTRY hooked_glDeleteShader(GLuint shader)
{
    g_shader_sources.erase(shader);
    g_shader_types.erase(shader);
    glDeleteShader(shader);
}

void GL_APIENTRY hooked_glAttachShader(GLuint program, GLuint shader)
{
    g_program_shaders[program].push_back(shader);
    glAttachShader(program, shader);
}

/* programs deleted since the device last looked: GL may give a new program the same name, so what
   the device keeps per program (the alpha test's last value, the right eye's last constants) goes */
std::vector<GLuint> g_deleted_programs;

void GL_APIENTRY hooked_glDeleteProgram(GLuint program)
{
    g_program_shaders.erase(program);
    g_deleted_programs.push_back(program);
    glDeleteProgram(program);
}

uint64_t fnv64(uint64_t h, const std::string &s)
{
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

void GL_APIENTRY hooked_glLinkProgram(GLuint program)
{
    if (g_debug_flags & (1u << 15)) {
        /* debug: no program cache, a plain link */
        for (GLuint sh : g_program_shaders[program]) compile_pending(sh);
        glLinkProgram(program);
        return;
    }
    static int binary_ok = -1;
    if (binary_ok < 0) {
        GLint formats = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
        binary_ok = formats > 0;
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s/shader_cache", bo1_data_root());
        mkdir(dir, 0777);
        bo1_log("d3d9: program cache in %s (%d binary formats)", dir, formats);
    }
    std::vector<GLuint> &shaders = g_program_shaders[program];
    std::string all;
    bool known = binary_ok;
    for (GLuint sh : shaders) {
        auto f = g_shader_sources.find(sh);
        if (f == g_shader_sources.end()) {
            known = false;
            break;
        }
        all += g_shader_types[sh] == GL_FRAGMENT_SHADER ? "\nFS\n" : "\nVS\n";
        all += f->second.source;
    }
    char path[MAX_PATH] = "";
    if (known) {
        snprintf(path, sizeof path, "%s/shader_cache/%016llx%016llx.bin", bo1_data_root(),
                 (unsigned long long)fnv64(0xcbf29ce484222325ull, all), (unsigned long long)fnv64(0x84222325cbf29ce4ull, all));
        if (FILE *f = fopen(path, "rb")) {
            uint32_t header[2] = {0, 0}; /* format, length of the sources (a cheap collision check) */
            std::vector<uint8_t> data;
            if (fread(header, sizeof header, 1, f) == 1 && header[1] == (uint32_t)all.size()) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f) - (long)sizeof header;
                fseek(f, sizeof header, SEEK_SET);
                if (size > 0) {
                    data.resize((size_t)size);
                    if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
                }
            }
            fclose(f);
            if (!data.empty()) {
                glProgramBinary(program, header[0], data.data(), (GLsizei)data.size());
                GLint ok = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &ok);
                if (ok) {
                    g_cache_hits++;
                    return;
                }
            }
            remove(path); /* refused (another driver): made again below */
        }
    }
    g_cache_misses++;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (GLuint sh : shaders) compile_pending(sh);
    if (known) glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(program);
    GLint ok = 0, length = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    bo1_log("d3d9: program compiled and linked in %.1f ms (not in the cache yet)",
            (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    if (!ok || !known) return;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0) return;
    std::vector<uint8_t> data((size_t)length);
    GLenum format = 0;
    glGetProgramBinary(program, length, &length, &format, data.data());
    if (FILE *f = fopen(path, "wb")) {
        uint32_t header[2] = {format, (uint32_t)all.size()};
        fwrite(header, sizeof header, 1, f);
        fwrite(data.data(), 1, (size_t)length, f);
        fclose(f);
    }
}

void *MOJOSHADERCALL gl_lookup(const char *name, void *)
{
    if (!strcmp(name, "glCreateShader")) return (void *)hooked_glCreateShader;
    if (!strcmp(name, "glShaderSource")) return (void *)hooked_glShaderSource;
    if (!strcmp(name, "glCompileShader")) return (void *)hooked_glCompileShader;
    if (!strcmp(name, "glGetShaderiv")) return (void *)hooked_glGetShaderiv;
    if (!strcmp(name, "glDeleteShader")) return (void *)hooked_glDeleteShader;
    if (!strcmp(name, "glAttachShader")) return (void *)hooked_glAttachShader;
    if (!strcmp(name, "glDeleteProgram")) return (void *)hooked_glDeleteProgram;
    if (!strcmp(name, "glLinkProgram")) return (void *)hooked_glLinkProgram;
    if (!strcmp(name, "glVertexAttribDivisorARB")) return bo1_gl_lookup("glVertexAttribDivisor");
    if (!strcmp(name, "glVertexAttribPointer")) return (void *)gl_cache::vertex_attrib_pointer; /* skips repeats */
    return bo1_gl_lookup(name);
}

/* ---- formats --------------------------------------------------------------------------------- */

enum Convert { CONVERT_NONE, CONVERT_BGRA, CONVERT_BGRX, CONVERT_A1R5G5B5, CONVERT_X1R5G5B5, CONVERT_A4R4G4B4,
               CONVERT_DXT1, CONVERT_DXT3, CONVERT_DXT5, CONVERT_UNORM16_TO_HALF };

unsigned g_upload_bytes, g_upload_count; /* texture uploads, for the 5 s stats */
unsigned g_vs_compiles, g_ps_compiles, g_links; /* shader work, for the 5 s stats */

#define FOURCC(a, b, c, d) ((DWORD)(a) | (DWORD)(b) << 8 | (DWORD)(c) << 16 | (DWORD)(d) << 24)
const DWORD FMT_INTZ = FOURCC('I', 'N', 'T', 'Z'), FMT_DF24 = FOURCC('D', 'F', '2', '4'),
            FMT_DF16 = FOURCC('D', 'F', '1', '6'), FMT_NULL = FOURCC('N', 'U', 'L', 'L'),
            FMT_RESZ = FOURCC('R', 'E', 'S', 'Z');
const DWORD RESZ_CODE = 0x7fa05000; /* D3DRS_POINTSIZE value that asks for the depth buffer -> INTZ copy */

struct Format {
    GLenum internal = 0, format = 0, type = 0; /* internal 0: not supported */
    bool compressed = false, depth = false, stencil = false;
    Convert convert = CONVERT_NONE;
    GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    UINT source_bytes = 4; /* per pixel, or per 4x4 block when block_source */
    bool block_source = false;
    UINT upload_bytes = 4; /* per pixel of what GL receives (uncompressed) */
};

Format format_of(D3DFORMAT f, bool volume = false)
{
    Format r;
    auto set = [&](GLenum i, GLenum fmt, GLenum t, UINT src, UINT up) {
        r.internal = i;
        r.format = fmt;
        r.type = t;
        r.source_bytes = src;
        r.upload_bytes = up;
    };
    auto swizzle = [&](GLint a, GLint b, GLint c, GLint d) {
        r.swizzle[0] = a;
        r.swizzle[1] = b;
        r.swizzle[2] = c;
        r.swizzle[3] = d;
    };
    auto dxt = [&](GLenum gl_format, Convert decode, UINT block) {
        r.block_source = true;
        r.source_bytes = block;
        if (g_has_s3tc && !volume) {
            r.internal = gl_format;
            r.compressed = true;
        } else {
            set(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, block, 4);
            r.convert = decode;
        }
    };
    switch ((DWORD)f) {
    case D3DFMT_A8R8G8B8: set(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4); r.convert = CONVERT_BGRA; break;
    case D3DFMT_X8R8G8B8: set(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4); r.convert = CONVERT_BGRX; break;
    case D3DFMT_A8B8G8R8: set(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4); break;
    case D3DFMT_R5G6B5: set(GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, 2); break;
    case D3DFMT_A1R5G5B5: set(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 2, 4); r.convert = CONVERT_A1R5G5B5; break;
    case D3DFMT_X1R5G5B5: set(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 2, 4); r.convert = CONVERT_X1R5G5B5; break;
    case D3DFMT_A4R4G4B4: set(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 2, 4); r.convert = CONVERT_A4R4G4B4; break;
    case D3DFMT_A8: set(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 1); swizzle(GL_ZERO, GL_ZERO, GL_ZERO, GL_RED); break;
    case D3DFMT_L8: set(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 1); swizzle(GL_RED, GL_RED, GL_RED, GL_ONE); break;
    case D3DFMT_A8L8: set(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, 2); swizzle(GL_RED, GL_RED, GL_RED, GL_GREEN); break;
    case D3DFMT_G16R16: set(GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, 4); r.convert = CONVERT_UNORM16_TO_HALF; break;
    case D3DFMT_A16B16G16R16: set(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, 8); r.convert = CONVERT_UNORM16_TO_HALF; break;
    case D3DFMT_G16R16F: set(GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, 4); break;
    case D3DFMT_A16B16G16R16F: set(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, 8); break;
    case D3DFMT_R32F: set(GL_R32F, GL_RED, GL_FLOAT, 4, 4); break;
    case D3DFMT_G32R32F: set(GL_RG32F, GL_RG, GL_FLOAT, 8, 8); break;
    case D3DFMT_A32B32G32R32F: set(GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, 16); break;
    case D3DFMT_DXT1: dxt(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, CONVERT_DXT1, 8); break;
    case D3DFMT_DXT2: case D3DFMT_DXT3: dxt(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, CONVERT_DXT3, 16); break;
    case D3DFMT_DXT4: case D3DFMT_DXT5: dxt(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, CONVERT_DXT5, 16); break;
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
        set(GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, 2);
        r.depth = true;
        break;
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24FS8: case D3DFMT_D15S1:
        set(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 4);
        r.depth = r.stencil = true;
        break;
    default:
        if ((DWORD)f == FMT_NULL) set(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4);
        if ((DWORD)f == FMT_INTZ) {
            /* a depth texture the shaders sample: depth in every channel, as D3D's INTZ gives. The
               engine fills it from the scene's depth buffer with the RESZ trick (Device::DrawPrimitiveUP),
               which spares it the separate float-z pass over the whole scene. */
            set(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 4);
            r.depth = r.stencil = true;
            swizzle(GL_RED, GL_RED, GL_RED, GL_RED);
        }
        break; /* DF24 and other vendor formats: reported unsupported */
    }
    return r;
}

UINT pitch_of(const Format &f, UINT width)
{
    return f.block_source ? ((width + 3) / 4) * f.source_bytes : width * f.source_bytes;
}
UINT rows_of(const Format &f, UINT height) { return f.block_source ? (height + 3) / 4 : height; }

UINT mip_size(UINT size, UINT level)
{
    size >>= level;
    return size ? size : 1;
}

UINT chain_length(UINT w, UINT h, UINT d = 1)
{
    UINT n = 1;
    while (w > 1 || h > 1 || d > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        d = d > 1 ? d / 2 : 1;
        n++;
    }
    return n;
}

/* ---- conversions to what GL takes ------------------------------------------------------------ */

uint16_t to_half(float value)
{
    uint32_t x;
    memcpy(&x, &value, 4);
    uint32_t sign = (x >> 16) & 0x8000, exponent = (x >> 23) & 0xff, mantissa = x & 0x7fffff;
    if (exponent < 113) return (uint16_t)sign; /* tiny: zero */
    if (exponent > 142) return (uint16_t)(sign | 0x7bff);
    return (uint16_t)(sign | (exponent - 112) << 10 | mantissa >> 13);
}

void rgb565_to(uint16_t c, uint8_t *rgb)
{
    rgb[0] = (uint8_t)((c >> 11 & 31) * 255 / 31);
    rgb[1] = (uint8_t)((c >> 5 & 63) * 255 / 63);
    rgb[2] = (uint8_t)((c & 31) * 255 / 31);
}

/* one 4x4 DXT block to RGBA8 rows of `pitch` bytes, clipped to w x h */
void decode_block(const uint8_t *block, Convert kind, uint8_t *out, UINT pitch, UINT w, UINT h)
{
    const uint8_t *colour = kind == CONVERT_DXT1 ? block : block + 8;
    uint16_t c0 = (uint16_t)(colour[0] | colour[1] << 8), c1 = (uint16_t)(colour[2] | colour[3] << 8);
    uint8_t palette[4][4];
    rgb565_to(c0, palette[0]);
    rgb565_to(c1, palette[1]);
    palette[0][3] = palette[1][3] = palette[2][3] = palette[3][3] = 255;
    bool four = kind != CONVERT_DXT1 || c0 > c1;
    for (int i = 0; i < 3; i++) {
        palette[2][i] = (uint8_t)(four ? (2 * palette[0][i] + palette[1][i]) / 3 : (palette[0][i] + palette[1][i]) / 2);
        palette[3][i] = (uint8_t)(four ? (palette[0][i] + 2 * palette[1][i]) / 3 : 0);
    }
    if (!four) palette[3][3] = 0;
    uint32_t indices = (uint32_t)colour[4] | (uint32_t)colour[5] << 8 | (uint32_t)colour[6] << 16 | (uint32_t)colour[7] << 24;
    uint8_t alpha[16];
    if (kind == CONVERT_DXT3) {
        for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);
    } else if (kind == CONVERT_DXT5) {
        uint8_t a[8] = {block[0], block[1]};
        for (int i = 2; i < 8; i++)
            a[i] = (uint8_t)(a[0] > a[1] ? ((8 - i) * a[0] + (i - 1) * a[1]) / 7
                                          : i < 6 ? ((6 - i) * a[0] + (i - 1) * a[1]) / 5 : i == 6 ? 0 : 255);
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= (uint64_t)block[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7];
    }
    for (UINT y = 0; y < h && y < 4; y++) {
        for (UINT x = 0; x < w && x < 4; x++) {
            int i = (int)(y * 4 + x);
            uint8_t *p = out + y * pitch + x * 4;
            memcpy(p, palette[(indices >> (2 * i)) & 3], 4);
            if (kind != CONVERT_DXT1) p[3] = alpha[i];
        }
    }
}

/* source image (D3D layout) -> buffer GL takes; returns NULL when no conversion is needed */
uint8_t *convert_image(const Format &f, const uint8_t *src, UINT w, UINT h, UINT depth)
{
    if (f.convert == CONVERT_NONE) return NULL;
    UINT src_pitch = pitch_of(f, w), src_slice = src_pitch * rows_of(f, h);
    uint8_t *out = (uint8_t *)malloc((size_t)w * h * depth * f.upload_bytes);
    if (!out) return NULL;
    for (UINT z = 0; z < depth; z++) {
        const uint8_t *s = src + (size_t)z * src_slice;
        uint8_t *d = out + (size_t)z * w * h * f.upload_bytes;
        if (f.convert >= CONVERT_DXT1 && f.convert <= CONVERT_DXT5) {
            for (UINT by = 0; by < (h + 3) / 4; by++)
                for (UINT bx = 0; bx < (w + 3) / 4; bx++)
                    decode_block(s + by * src_pitch + bx * f.source_bytes, f.convert, d + ((size_t)by * 4 * w + bx * 4) * 4,
                                 w * 4, w - bx * 4, h - by * 4);
            continue;
        }
        for (UINT y = 0; y < h; y++) {
            const uint8_t *row = s + (size_t)y * src_pitch;
            uint8_t *o = d + (size_t)y * w * f.upload_bytes;
            for (UINT x = 0; x < w; x++) {
                switch (f.convert) {
                case CONVERT_BGRA: case CONVERT_BGRX:
                    o[4 * x] = row[4 * x + 2];
                    o[4 * x + 1] = row[4 * x + 1];
                    o[4 * x + 2] = row[4 * x];
                    o[4 * x + 3] = f.convert == CONVERT_BGRX ? 255 : row[4 * x + 3];
                    break;
                case CONVERT_A1R5G5B5: case CONVERT_X1R5G5B5: {
                    uint16_t c = (uint16_t)(row[2 * x] | row[2 * x + 1] << 8);
                    o[4 * x] = (uint8_t)((c >> 10 & 31) * 255 / 31);
                    o[4 * x + 1] = (uint8_t)((c >> 5 & 31) * 255 / 31);
                    o[4 * x + 2] = (uint8_t)((c & 31) * 255 / 31);
                    o[4 * x + 3] = f.convert == CONVERT_X1R5G5B5 || (c & 0x8000) ? 255 : 0;
                    break;
                }
                case CONVERT_A4R4G4B4: {
                    uint16_t c = (uint16_t)(row[2 * x] | row[2 * x + 1] << 8);
                    o[4 * x] = (uint8_t)((c >> 8 & 15) * 17);
                    o[4 * x + 1] = (uint8_t)((c >> 4 & 15) * 17);
                    o[4 * x + 2] = (uint8_t)((c & 15) * 17);
                    o[4 * x + 3] = (uint8_t)((c >> 12 & 15) * 17);
                    break;
                }
                case CONVERT_UNORM16_TO_HALF: {
                    UINT channels = f.upload_bytes / 2;
                    for (UINT c = 0; c < channels; c++) {
                        uint16_t v = (uint16_t)(row[(x * channels + c) * 2] | row[(x * channels + c) * 2 + 1] << 8);
                        uint16_t hv = to_half(v / 65535.0f);
                        memcpy(o + (x * channels + c) * 2, &hv, 2);
                    }
                    break;
                }
                default: break;
                }
            }
        }
    }
    return out;
}

/* ---- reference counting ---------------------------------------------------------------------- */

template <typename Interface> struct Counted : Interface {
    volatile ULONG refs = 1;
    virtual ~Counted() = default;
    HRESULT QueryInterface(REFIID, void **out) override
    {
        *out = NULL;
        return E_FAIL;
    }
    ULONG AddRef() override { return __atomic_add_fetch(&refs, 1, __ATOMIC_ACQ_REL); }
    ULONG Release() override
    {
        ULONG left = __atomic_sub_fetch(&refs, 1, __ATOMIC_ACQ_REL);
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

/* ---- textures -------------------------------------------------------------------------------- */

struct Surface;

/* What all three texture interfaces share: images in CPU memory until the GL thread uploads them */
struct TextureData {
    GLenum target;
    D3DFORMAT d3d_format;
    Format format;
    DWORD usage;
    D3DPOOL pool;
    UINT width, height, depth, levels, faces;
    GLuint name = 0;
    struct Image {
        uint8_t *bits = NULL;
        bool dirty = false;
    };
    std::vector<Image> images; /* [face * levels + level] */
    std::vector<Surface *> surfaces;
    bool dirty = false;
    /* multiview: 2 for the eyes' render targets, made as a two-layer GL_TEXTURE_2D_ARRAY (one layer
       an eye; bo1_d3d9_stereo_surface) */
    UINT layers = 1;
    /* locked now (no upload until unlocked), and whether the CPU copy must outlive uploads: the
       engine rewrites some textures through locks every frame (the model lighting volume) */
    int locked = 0;
    bool keep_bits = false;

    TextureData(GLenum t, D3DFORMAT f, DWORD u, D3DPOOL p, UINT w, UINT h, UINT d, UINT l, UINT fc)
        : target(t), d3d_format(f), format(format_of(f, t == GL_TEXTURE_3D)), usage(u), pool(p), width(w), height(h),
          depth(d), levels(l ? l : chain_length(w, h, d)), faces(fc)
    {
        if (levels > chain_length(w, h, d)) levels = chain_length(w, h, d);
        /* BGRA bytes go up as they are and GL's swizzle swaps red and blue when sampled: no
           per-pixel conversion on the CPU (it cost ~9% of the render thread for the textures the
           engine rewrites every frame). Not for render targets: GL writes those in its own order. */
        if ((format.convert == CONVERT_BGRA || format.convert == CONVERT_BGRX) && !render_target()) {
            format.swizzle[0] = GL_BLUE;
            format.swizzle[2] = GL_RED;
            if (format.convert == CONVERT_BGRX) format.swizzle[3] = GL_ONE;
            format.convert = CONVERT_NONE;
        }
        images.resize((size_t)faces * levels);
        surfaces.resize((size_t)faces * levels, NULL);
    }
    ~TextureData()
    {
        for (Image &i : images) free(i.bits);
        delete_later(GL_KIND_TEXTURE, name);
    }
    bool render_target() const { return usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL); }
    /* system-memory textures never reach GL; they're sources for UpdateTexture */
    bool gpu() const { return pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH; }
    UINT w(UINT level) const { return mip_size(width, level); }
    UINT h(UINT level) const { return mip_size(height, level); }
    UINT d(UINT level) const { return mip_size(depth, level); }
    size_t image_bytes(UINT level) const
    {
        return (size_t)pitch_of(format, w(level)) * rows_of(format, h(level)) * d(level);
    }
    uint8_t *memory(UINT face, UINT level)
    {
        Image &i = images[face * levels + level];
        if (!i.bits) i.bits = (uint8_t *)calloc(1, image_bytes(level));
        return i.bits;
    }
    void mark_dirty(UINT face, UINT level)
    {
        images[face * levels + level].dirty = true;
        dirty = true;
    }

    /* GL thread: make the GL texture and upload what changed */
    void realize()
    {
        if (!format.internal || !gpu()) return;
        if (!name) {
            glGenTextures(1, &name);
            glBindTexture(target, name);
            if (target == GL_TEXTURE_3D) glTexStorage3D(target, (GLsizei)levels, format.internal, width, height, depth);
            else if (target == GL_TEXTURE_2D_ARRAY) glTexStorage3D(target, (GLsizei)levels, format.internal, width, height, 2);
            else glTexStorage2D(target, (GLsizei)levels, format.internal, width, height);
            glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, format.swizzle[0]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, format.swizzle[1]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, format.swizzle[2]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, format.swizzle[3]);
        } else if (dirty) {
            glBindTexture(target, name);
        }
        if (!dirty || locked) return;
        dirty = false;
        if (target == GL_TEXTURE_2D_ARRAY) return; /* the eyes' render targets: the GPU fills them */
        for (UINT face = 0; face < faces; face++) {
            for (UINT level = 0; level < levels; level++) {
                Image &image = images[face * levels + level];
                if (!image.dirty || !image.bits) continue;
                image.dirty = false;
                g_upload_bytes += (unsigned)image_bytes(level);
                g_upload_count++;
                GLenum t = target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : target;
                if (format.compressed) {
                    glCompressedTexSubImage2D(t, (GLint)level, 0, 0, w(level), h(level), format.internal,
                                              (GLsizei)image_bytes(level), image.bits);
                } else {
                    uint8_t *converted = convert_image(format, image.bits, w(level), h(level), d(level));
                    const void *data = converted ? converted : image.bits;
                    if (target == GL_TEXTURE_3D)
                        glTexSubImage3D(t, (GLint)level, 0, 0, 0, w(level), h(level), d(level), format.format,
                                        format.type, data);
                    else
                        glTexSubImage2D(t, (GLint)level, 0, 0, w(level), h(level), format.format, format.type, data);
                    free(converted);
                }
                /* the file's copy has done its job; dynamic textures keep theirs for partial updates */
                if (!(usage & D3DUSAGE_DYNAMIC) && !keep_bits) {
                    free(image.bits);
                    image.bits = NULL;
                }
            }
        }
    }

    HRESULT lock(UINT face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags)
    {
        if (level >= levels || face >= faces) return D3DERR_INVALIDCALL;
        uint8_t *base = memory(face, level);
        if (!base) return E_OUTOFMEMORY;
        UINT pitch = pitch_of(format, w(level));
        size_t offset = 0;
        if (rect) {
            offset = format.block_source ? (size_t)(rect->top / 4) * pitch + (size_t)(rect->left / 4) * format.source_bytes
                                         : (size_t)rect->top * pitch + (size_t)rect->left * format.source_bytes;
        }
        locked->Pitch = (INT)pitch;
        locked->pBits = base + offset;
        if (!(flags & D3DLOCK_READONLY)) mark_dirty(face, level);
        return S_OK;
    }

    void describe(UINT level, D3DSURFACE_DESC *d)
    {
        *d = {};
        d->Format = d3d_format;
        d->Type = D3DRTYPE_SURFACE;
        d->Usage = usage;
        d->Pool = pool;
        d->Width = w(level);
        d->Height = h(level);
    }

    IDirect3DSurface9 *surface(IUnknown *container, UINT face, UINT level);
};

/* ---- surfaces -------------------------------------------------------------------------------- */

void forget_framebuffers_with(Surface *s);

/* A render target, a depth buffer, a texture level, or plain memory */
struct Surface final : Resource<IDirect3DSurface9> {
    enum Kind { RENDERBUFFER, TEXTURE_LEVEL, MEMORY } kind;
    D3DSURFACE_DESC desc;
    Format format;
    UINT samples = 0;
    GLuint renderbuffer = 0;
    /* a texture instead of a renderbuffer, for surfaces that are drawn later (the back buffer) */
    bool sampled = false;
    GLuint texture_name = 0;
    uint8_t *bits = NULL;
    /* texture levels: the texture's references count for the surface (as in D3D9) */
    IUnknown *container = NULL;
    TextureData *texture = NULL;
    UINT face = 0, level = 0;
    UINT layers = 1; /* multiview: 2 for a stand-alone eye target (a two-layer texture array) */
    bool depth_only_array = false; /* debug: made without stencil (d3d9_debug bit 21) */

    Surface(Kind k, UINT w, UINT h, D3DFORMAT f, DWORD usage, D3DPOOL pool, UINT msaa = 0)
        : Resource(D3DRTYPE_SURFACE), kind(k), format(format_of(f)), samples(msaa)
    {
        desc = {};
        desc.Format = f;
        desc.Type = D3DRTYPE_SURFACE;
        desc.Usage = usage;
        desc.Pool = pool;
        desc.Width = w;
        desc.Height = h;
    }
    ~Surface() override
    {
        forget_framebuffers_with(this);
        delete_later(GL_KIND_RENDERBUFFER, renderbuffer);
        delete_later(GL_KIND_TEXTURE, texture_name);
        free(bits);
    }
    ULONG AddRef() override { return container ? container->AddRef() : Resource::AddRef(); }
    ULONG Release() override { return container ? container->Release() : Resource::Release(); }

    HRESULT GetDesc(D3DSURFACE_DESC *d) override
    {
        *d = desc;
        return S_OK;
    }
    HRESULT LockRect(D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override
    {
        if (texture) return texture->lock(face, level, locked, rect, flags);
        if (!bits) bits = (uint8_t *)calloc(1, (size_t)pitch_of(format, desc.Width) * rows_of(format, desc.Height) + 16);
        if (!bits) return E_OUTOFMEMORY;
        locked->Pitch = (INT)pitch_of(format, desc.Width);
        locked->pBits = bits + (rect ? (size_t)rect->top * locked->Pitch + (size_t)rect->left * format.source_bytes : 0);
        return S_OK;
    }
    HRESULT UnlockRect() override { return S_OK; }

    bool is_depth() const { return format.depth; }
    bool layered() const { return texture ? texture->layers == 2 : layers == 2; }
    /* GL thread: the GL texture of a layered surface (made if need be) */
    GLuint layered_texture()
    {
        if (texture) {
            texture->realize();
            return texture->name;
        }
        if (!texture_name && format.internal) {
            glGenTextures(1, &texture_name);
            glBindTexture(GL_TEXTURE_2D_ARRAY, texture_name);
            GLenum internal = format.internal;
            if (format.depth && (g_debug_flags & (1u << 21))) {
                /* debug: a depth-only array (no stencil) */
                internal = GL_DEPTH_COMPONENT32F;
                depth_only_array = true;
            }
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, internal, desc.Width, desc.Height, 2);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        return texture_name;
    }

    /* GL thread: attach to the bound draw framebuffer. A layered surface goes in as both layers
       (mode FB_MULTIVIEW), or just one (FB_LAYER0 / FB_LAYER1) */
    void attach(GLenum attachment, uint32_t mode = 0)
    {
        if (layered()) {
            GLuint name = layered_texture();
            GLint lvl = texture ? (GLint)level : 0;
            if (mode == 1) host_gl_multiview_attach(GL_DRAW_FRAMEBUFFER, attachment, name, lvl, 0, 2);
            else glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, attachment, name, lvl, mode == 2 ? 1 : 0);
            return;
        }
        if (texture) {
            texture->realize();
            if (texture->target == GL_TEXTURE_3D)
                glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, attachment, texture->name, (GLint)level, 0);
            else
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment,
                                       texture->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face
                                                                              : GL_TEXTURE_2D,
                                       texture->name, (GLint)level);
            return;
        }
        if (sampled) {
            if (!texture_name && format.internal) {
                glGenTextures(1, &texture_name);
                glBindTexture(GL_TEXTURE_2D, texture_name);
                glTexStorage2D(GL_TEXTURE_2D, 1, format.internal, desc.Width, desc.Height);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture_name, 0);
            return;
        }
        if (!renderbuffer && format.internal) {
            glGenRenderbuffers(1, &renderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            if (samples > 1)
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, (GLsizei)samples, format.internal, desc.Width, desc.Height);
            else
                glRenderbufferStorage(GL_RENDERBUFFER, format.internal, desc.Width, desc.Height);
        }
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, GL_RENDERBUFFER, renderbuffer);
    }
};

IDirect3DSurface9 *TextureData::surface(IUnknown *owner, UINT face, UINT level)
{
    Surface *&s = surfaces[face * levels + level];
    if (!s) {
        s = new Surface(Surface::TEXTURE_LEVEL, w(level), h(level), d3d_format, usage, pool);
        s->container = owner;
        s->texture = this;
        s->face = face;
        s->level = level;
    }
    return s;
}

/* the texture's level surfaces die with it, whatever the counts say */
void delete_level_surfaces(TextureData &t)
{
    for (Surface *&s : t.surfaces) {
        if (!s) continue;
        s->container = NULL;
        s->Resource::Release();
        s = NULL;
    }
}

struct Texture final : Resource<IDirect3DTexture9> {
    TextureData data;
    Texture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p)
        : Resource(D3DRTYPE_TEXTURE), data(GL_TEXTURE_2D, f, u, p, w, h, 1, l, 1) {}
    ~Texture() override { delete_level_surfaces(data); }
    DWORD GetLevelCount() override { return data.levels; }
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *d) override
    {
        if (level >= data.levels) return D3DERR_INVALIDCALL;
        data.describe(level, d);
        return S_OK;
    }
    HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface) override
    {
        if (level >= data.levels) return D3DERR_INVALIDCALL;
        *surface = data.surface(this, 0, level);
        (*surface)->AddRef();
        return S_OK;
    }
    HRESULT LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override
    {
        return data.lock(0, level, locked, rect, flags);
    }
    HRESULT UnlockRect(UINT) override { return S_OK; }
    HRESULT AddDirtyRect(const RECT *) override { return S_OK; }
};

struct CubeTexture final : Resource<IDirect3DCubeTexture9> {
    TextureData data;
    CubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p)
        : Resource(D3DRTYPE_CUBETEXTURE), data(GL_TEXTURE_CUBE_MAP, f, u, p, e, e, 1, l, 6) {}
    ~CubeTexture() override { delete_level_surfaces(data); }
    DWORD GetLevelCount() override { return data.levels; }
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC *d) override
    {
        if (level >= data.levels) return D3DERR_INVALIDCALL;
        data.describe(level, d);
        return S_OK;
    }
    HRESULT GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface) override
    {
        if (level >= data.levels || (UINT)face >= 6) return D3DERR_INVALIDCALL;
        *surface = data.surface(this, (UINT)face, level);
        (*surface)->AddRef();
        return S_OK;
    }
    HRESULT LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override
    {
        return data.lock((UINT)face, level, locked, rect, flags);
    }
    HRESULT UnlockRect(D3DCUBEMAP_FACES, UINT) override { return S_OK; }
};

struct VolumeTexture final : Resource<IDirect3DVolumeTexture9> {
    TextureData data;
    VolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p)
        : Resource(D3DRTYPE_VOLUMETEXTURE), data(GL_TEXTURE_3D, f, u, p, w, h, d, l, 1) {}
    DWORD GetLevelCount() override { return data.levels; }
    HRESULT GetLevelDesc(UINT level, D3DVOLUME_DESC *d) override
    {
        if (level >= data.levels) return D3DERR_INVALIDCALL;
        *d = {};
        d->Format = data.d3d_format;
        d->Type = D3DRTYPE_VOLUME;
        d->Usage = data.usage;
        d->Pool = data.pool;
        d->Width = data.w(level);
        d->Height = data.h(level);
        d->Depth = data.d(level);
        return S_OK;
    }
    HRESULT LockBox(UINT level, D3DLOCKED_BOX *locked, const D3DBOX *box, DWORD flags) override
    {
        if (level >= data.levels) return D3DERR_INVALIDCALL;
        uint8_t *base = data.memory(0, level);
        if (!base) return E_OUTOFMEMORY;
        UINT pitch = pitch_of(data.format, data.w(level)), slice = pitch * rows_of(data.format, data.h(level));
        locked->RowPitch = (INT)pitch;
        locked->SlicePitch = (INT)slice;
        size_t offset = 0;
        if (box) {
            UINT rows = data.format.block_source ? box->Top / 4 : box->Top;
            UINT cols = data.format.block_source ? box->Left / 4 : box->Left;
            offset = (size_t)box->Front * slice + (size_t)rows * pitch + (size_t)cols * data.format.source_bytes;
        }
        locked->pBits = base + offset;
        if (!(flags & D3DLOCK_READONLY)) {
            /* written while locked, uploaded once unlocked; the copy stays for the next lock */
            data.keep_bits = true;
            data.locked++;
            locked_levels |= 1u << level;
        }
        return S_OK;
    }
    unsigned locked_levels = 0;
    HRESULT UnlockBox(UINT level) override
    {
        if (level < 32 && (locked_levels & 1u << level)) {
            locked_levels &= ~(1u << level);
            data.locked--;
            data.mark_dirty(0, level);
        }
        return S_OK;
    }
};

TextureData *texture_data(IDirect3DBaseTexture9 *t)
{
    if (!t) return NULL;
    switch (t->GetType()) {
    case D3DRTYPE_TEXTURE: return &static_cast<Texture *>(t)->data;
    case D3DRTYPE_CUBETEXTURE: return &static_cast<CubeTexture *>(t)->data;
    case D3DRTYPE_VOLUMETEXTURE: return &static_cast<VolumeTexture *>(t)->data;
    default: return NULL;
    }
}

/* ---- buffers: CPU copy + GL buffer, uploaded where they changed ------------------------------ */

/* Two kinds of buffer. Static ones (world geometry, models) go up to a GL buffer once and stay.
 * Streamed ones are those the engine keeps rewriting (D3DUSAGE_DYNAMIC, or locked for writing
 * again after their upload: skinned vertices, per-frame index lists). They never get a GL buffer
 * of their own: each draw copies just the range it uses into the device's ring (Device::stream).
 * Updating a GL buffer the GPU is still reading made the Adreno driver stall or copy the whole
 * buffer on every draw: 4 frames a second at the title screen. */
template <typename Interface, typename Desc> struct Buffer final : Resource<Interface> {
    Desc desc;
    uint8_t *data;
    GLuint name = 0;
    UINT dirty_from = ~0u, dirty_to = 0;
    bool streamed;
    /* streamed buffers: bumped by each write lock; the last range copied into the ring, reused by
       later draws of the same unchanged data in the same ring part (Device::stream_cached) */
    uint32_t generation = 0;
    uint32_t cache_generation = ~0u, cache_serial = ~0u;
    UINT cache_from = 0, cache_bytes = 0, cache_at = 0;
    Buffer(D3DRESOURCETYPE type, const Desc &d)
        : Resource<Interface>(type), desc(d), data((uint8_t *)calloc(1, d.Size ? d.Size : 4)),
          streamed(d.Usage & D3DUSAGE_DYNAMIC)
    {
        dirty_from = 0;
        dirty_to = d.Size;
    }
    ~Buffer() override
    {
        free(data);
        delete_later(GL_KIND_BUFFER, name);
    }
    HRESULT Lock(UINT offset, UINT size, void **out, DWORD flags) override
    {
        if (!data) return E_OUTOFMEMORY;
        if (!size || offset + size > desc.Size) size = desc.Size - (offset < desc.Size ? offset : desc.Size);
        *out = data + offset;
        if (flags & D3DLOCK_READONLY) return S_OK;
        generation++;
        if (name && !streamed && desc.Size <= (4u << 20)) {
            /* rewritten after its upload: stream it from now on. Big buffers (world geometry)
               stay static and take a partial update instead; draws often name their whole range. */
            streamed = true;
            delete_later(GL_KIND_BUFFER, name);
            name = 0;
        }
        if (offset < dirty_from) dirty_from = offset;
        if (offset + size > dirty_to) dirty_to = offset + size;
        return S_OK;
    }
    HRESULT Unlock() override { return S_OK; }
    HRESULT GetDesc(Desc *d) override
    {
        *d = desc;
        return S_OK;
    }
    /* GL thread, static buffers: binds it to `target`, uploading what changed */
    void bind(GLenum target)
    {
        if (!name) {
            glGenBuffers(1, &name);
            glBindBuffer(target, name);
            glBufferData(target, desc.Size, data, GL_STATIC_DRAW);
            dirty_from = ~0u;
            dirty_to = 0;
            return;
        }
        glBindBuffer(target, name);
        if (dirty_from >= dirty_to) return;
        glBufferSubData(target, dirty_from, dirty_to - dirty_from, data + dirty_from);
        dirty_from = ~0u;
        dirty_to = 0;
    }
};
typedef Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC> VertexBuffer;
typedef Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC> IndexBuffer;

/* ---- shaders and declarations ---------------------------------------------------------------- */

struct VertexDeclaration final : Counted<IDirect3DVertexDeclaration9> {
    std::vector<D3DVERTEXELEMENT9> elements;
    std::vector<MOJOSHADER_swizzle> swizzles; /* D3DCOLOR inputs read as BGRA */
    std::string key;
    explicit VertexDeclaration(const D3DVERTEXELEMENT9 *e)
    {
        for (; e->Stream != 0xFF; e++) {
            elements.push_back(*e);
            if (e->Type == D3DDECLTYPE_D3DCOLOR) {
                MOJOSHADER_swizzle s = {(MOJOSHADER_usage)e->Usage, e->UsageIndex, {2, 1, 0, 3}};
                swizzles.push_back(s);
                key += (char)('A' + e->Usage);
                key += (char)('0' + e->UsageIndex);
            }
        }
    }
};

struct VertexShader final : Counted<IDirect3DVertexShader9> {
    std::vector<uint8_t> bytecode;
    std::map<std::string, MOJOSHADER_glShader *> variants; /* by the declaration's colour swizzles */
    explicit VertexShader(const DWORD *code) { bytecode.assign((const uint8_t *)code, (const uint8_t *)code + length(code)); }
    static size_t length(const DWORD *code)
    {
        const DWORD *t = code + 1; /* past the version token */
        while (*t != 0x0000FFFF) {
            DWORD op = *t & 0xFFFF;
            t += op == 0xFFFE ? 1 + (*t >> 16) : 1 + ((*t >> 24) & 0x0F);
        }
        return (size_t)(t + 1 - code) * 4;
    }
};

struct PixelShader final : Counted<IDirect3DPixelShader9> {
    std::vector<uint8_t> bytecode;
    MOJOSHADER_glShader *compiled = NULL; /* the plain variant */
    bool failed = false;
    unsigned sampler_units = 0;           /* the sampler registers it reads */
    std::map<unsigned, MOJOSHADER_glShader *> variants; /* multiview / layered-sampler ones (CompileMode) */
    explicit PixelShader(const DWORD *code)
    {
        bytecode.assign((const uint8_t *)code, (const uint8_t *)code + VertexShader::length(code));
    }
};

struct Query final : Counted<IDirect3DQuery9> {
    D3DQUERYTYPE type;
    explicit Query(D3DQUERYTYPE t) : type(t) {}
    HRESULT Issue(DWORD) override { return S_OK; }
    HRESULT GetData(void *data, DWORD size, DWORD) override
    {
        /* finished at once: event queries TRUE, occlusion queries "everything visible" */
        if (data && size) {
            memset(data, 0, size);
            if (size >= 4) *(DWORD *)data = type == D3DQUERYTYPE_OCCLUSION ? 1000000 : 1;
        }
        return S_OK;
    }
};

/* ---- framebuffers, by attachment set --------------------------------------------------------- */

/* mode: how layered (multiview) attachments go in - FB_LAYER0 (also all there is to a single
   surface), FB_MULTIVIEW (both layers, drawn by gl_ViewID_OVR) or FB_LAYER1 */
enum { FB_LAYER0 = 0, FB_MULTIVIEW = 1, FB_LAYER1 = 2 };
struct FramebufferKey {
    Surface *colour[4];
    Surface *depth;
    uint32_t mode; /* no padding: memcmp compares keys */
    bool operator<(const FramebufferKey &o) const { return memcmp(this, &o, sizeof *this) < 0; }
};
std::map<FramebufferKey, GLuint> g_framebuffers;
FramebufferKey g_fb_last_key = {}; /* GL thread: the last framebuffer looked up (most draws ask for it again) */
GLuint g_fb_last = 0;

void forget_framebuffers_with(Surface *s)
{
    pthread_mutex_lock(&g_lock);
    g_fb_last = 0;
    for (auto i = g_framebuffers.begin(); i != g_framebuffers.end();) {
        const FramebufferKey &k = i->first;
        if (k.depth == s || k.colour[0] == s || k.colour[1] == s || k.colour[2] == s || k.colour[3] == s) {
            g_deletions.push_back({GL_KIND_FRAMEBUFFER, i->second});
            i = g_framebuffers.erase(i);
        } else {
            ++i;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* GL thread: binds (making if need be) the draw framebuffer for these attachments */
/* GPU time per render pass, when <data>/d3d9_debug has bit 25 (33554432): a GL_TIME_ELAPSED query
   from each change of the draw framebuffer to the next, totalled by the target's size and format
   and logged with the 5 s stats. Results are read back frames later, when they're ready. */
struct GpuPass {
    char label[40];
    GLuint query;
};
std::vector<GpuPass> g_gpu_pending;
std::vector<GLuint> g_gpu_free;
bool g_gpu_open = false;
std::map<std::string, std::pair<uint64_t, unsigned>> g_gpu_totals;
std::map<std::string, unsigned> g_pass_draws; /* draws per pass label, same window */
unsigned *g_pass_draw_counter;               /* the open pass's */
const GLenum TIME_ELAPSED = 0x88BF;

void gpu_pass(const char *label)
{
    if (g_gpu_open) glEndQuery(TIME_ELAPSED);
    g_gpu_open = false;
    if (!(g_debug_flags & (1u << 25)) || g_gpu_pending.size() > 8192) return;
    GpuPass p;
    snprintf(p.label, sizeof p.label, "%s", label);
    g_pass_draw_counter = &g_pass_draws[p.label];
    if (g_gpu_free.empty()) glGenQueries(1, &p.query);
    else {
        p.query = g_gpu_free.back();
        g_gpu_free.pop_back();
    }
    glBeginQuery(TIME_ELAPSED, p.query);
    g_gpu_open = true;
    g_gpu_pending.push_back(p);
}

/* end of a frame: close the open pass, total what's ready */
void gpu_collect()
{
    if (g_gpu_open) glEndQuery(TIME_ELAPSED);
    g_gpu_open = false;
    size_t done = 0;
    for (; done < g_gpu_pending.size(); done++) {
        GpuPass &p = g_gpu_pending[done];
        GLuint ready = 0, ns = 0;
        glGetQueryObjectuiv(p.query, 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &ready);
        if (!ready) break;
        glGetQueryObjectuiv(p.query, 0x8866 /* GL_QUERY_RESULT */, &ns);
        auto &t = g_gpu_totals[p.label];
        t.first += ns;
        t.second++;
        g_gpu_free.push_back(p.query);
    }
    g_gpu_pending.erase(g_gpu_pending.begin(), g_gpu_pending.begin() + done);
}

void gpu_report(unsigned frames)
{
    if (g_gpu_totals.empty()) return;
    std::vector<std::pair<uint64_t, std::string>> rows;
    uint64_t all = 0;
    for (auto &t : g_gpu_totals) {
        rows.push_back({t.second.first, t.first});
        all += t.second.first;
    }
    std::sort(rows.rbegin(), rows.rend());
    bo1_log("d3d9 gpu: %u us per frame in passes", (unsigned)(all / 1000 / frames));
    for (size_t i = 0; i < rows.size() && i < 14; i++)
        bo1_log("d3d9 gpu:   %6u us  %s (%u passes, %u draws per frame)", (unsigned)(rows[i].first / 1000 / frames),
                rows[i].second.c_str(), g_gpu_totals[rows[i].second].second / frames, g_pass_draws[rows[i].second] / frames);
    g_gpu_totals.clear();
    for (auto &d : g_pass_draws) d.second = 0;
}

GLuint g_last_draw_fbo = ~0u;
void bind_framebuffer(const FramebufferKey &key, GLenum binding = GL_DRAW_FRAMEBUFFER)
{
    if (g_fb_last && !(g_debug_flags & (1u << 25)) && !g_gpu_open && !memcmp(&key, &g_fb_last_key, sizeof key)) {
        glBindFramebuffer(binding, g_fb_last);
        return;
    }
    if (binding != GL_READ_FRAMEBUFFER && (g_debug_flags & (1u << 25) || g_gpu_open)) {
        pthread_mutex_lock(&g_lock);
        auto f = g_framebuffers.find(key);
        GLuint id = f == g_framebuffers.end() ? 0 : f->second;
        pthread_mutex_unlock(&g_lock);
        if (id != g_last_draw_fbo || !id) {
            g_last_draw_fbo = id;
            char label[40];
            const Surface *c = key.colour[0];
            if (c) snprintf(label, sizeof label, "%ux%u fmt %u%s%s", c->desc.Width, c->desc.Height, (unsigned)c->desc.Format,
                            key.depth ? " +depth" : "", key.mode == FB_MULTIVIEW ? " mv" : "");
            else if (key.depth) snprintf(label, sizeof label, "depth only %ux%u", key.depth->desc.Width, key.depth->desc.Height);
            else snprintf(label, sizeof label, "none");
            gpu_pass(label);
        }
    }
    pthread_mutex_lock(&g_lock);
    auto found = g_framebuffers.find(key);
    GLuint fbo = found == g_framebuffers.end() ? 0 : found->second;
    pthread_mutex_unlock(&g_lock);
    if (fbo) {
        g_fb_last_key = key;
        g_fb_last = fbo;
        glBindFramebuffer(binding, fbo);
        return;
    }
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    GLenum buffers[4];
    for (int i = 0; i < 4; i++) {
        buffers[i] = key.colour[i] ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
        if (key.colour[i]) key.colour[i]->attach(GL_COLOR_ATTACHMENT0 + i, key.mode);
    }
    glDrawBuffers(4, buffers);
    if (key.depth)
        key.depth->attach(key.depth->format.stencil && !key.depth->depth_only_array ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, key.mode);
    GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        bo1_log("d3d9: framebuffer incomplete (0x%x): colour %ux%u fmt %u, depth %ux%u", status,
                key.colour[0] ? key.colour[0]->desc.Width : 0, key.colour[0] ? key.colour[0]->desc.Height : 0,
                key.colour[0] ? (unsigned)key.colour[0]->desc.Format : 0, key.depth ? key.depth->desc.Width : 0,
                key.depth ? key.depth->desc.Height : 0);
    pthread_mutex_lock(&g_lock);
    g_framebuffers[key] = fbo;
    pthread_mutex_unlock(&g_lock);
    if (binding != GL_DRAW_FRAMEBUFFER) glBindFramebuffer(binding, fbo);
}

/* ---- GL enums for D3D ones ------------------------------------------------------------------- */

/* Pointing at an in-world screen (the front end's TV menus, drawn on a UI3D render texture): the
   game asks where on that texture a back-buffer pixel shows (bo1_d3d9_pick_request, every frame it
   wants to know); the device works it out from the frame's draws that sample the texture
   (Device::pick) and publishes the answer at Present (bo1_d3d9_pick_result). */
volatile int g_pick_x = -1, g_pick_y = -1;
void *volatile g_pick_texture;
volatile unsigned g_pick_serial;
volatile int g_pick_valid;
volatile float g_pick_u, g_pick_v;

GLenum gl_compare(DWORD f) { return f >= 1 && f <= 8 ? GL_NEVER + (f - 1) : GL_ALWAYS; }

GLenum gl_blend(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO: return GL_ZERO;
    case D3DBLEND_ONE: return GL_ONE;
    case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
    case 14: return GL_CONSTANT_COLOR;           /* D3DBLEND_BLENDFACTOR */
    case 15: return GL_ONE_MINUS_CONSTANT_COLOR; /* D3DBLEND_INVBLENDFACTOR */
    default: return GL_ONE;
    }
}

GLenum gl_blend_op(DWORD op)
{
    switch (op) {
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

GLenum gl_stencil_op(DWORD op)
{
    static const GLenum ops[] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    return op < 9 ? ops[op] : GL_KEEP;
}

GLenum gl_address(DWORD a)
{
    switch (a) {
    case D3DTADDRESS_MIRROR: case D3DTADDRESS_MIRRORONCE: return GL_MIRRORED_REPEAT;
    case D3DTADDRESS_CLAMP: return GL_CLAMP_TO_EDGE;
    case D3DTADDRESS_BORDER: return GL_CLAMP_TO_BORDER;
    default: return GL_REPEAT;
    }
}

GLenum gl_primitive(D3DPRIMITIVETYPE t, UINT count, GLsizei *vertices)
{
    switch (t) {
    case D3DPT_POINTLIST: *vertices = (GLsizei)count; return GL_POINTS;
    case D3DPT_LINELIST: *vertices = (GLsizei)count * 2; return GL_LINES;
    case D3DPT_LINESTRIP: *vertices = (GLsizei)count + 1; return GL_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP: *vertices = (GLsizei)count + 2; return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN: *vertices = (GLsizei)count + 2; return GL_TRIANGLE_FAN;
    default: *vertices = (GLsizei)count * 3; return GL_TRIANGLES;
    }
}

/* a declaration element as a MojoShader attribute: false for types GL ES can't take */
bool attribute_type(BYTE t, unsigned int *size, MOJOSHADER_attributeType *type, int *normalized)
{
    *normalized = 0;
    switch (t) {
    case D3DDECLTYPE_FLOAT1: *size = 1; *type = MOJOSHADER_ATTRIBUTE_FLOAT; return true;
    case D3DDECLTYPE_FLOAT2: *size = 2; *type = MOJOSHADER_ATTRIBUTE_FLOAT; return true;
    case D3DDECLTYPE_FLOAT3: *size = 3; *type = MOJOSHADER_ATTRIBUTE_FLOAT; return true;
    case D3DDECLTYPE_FLOAT4: *size = 4; *type = MOJOSHADER_ATTRIBUTE_FLOAT; return true;
    case D3DDECLTYPE_D3DCOLOR: case D3DDECLTYPE_UBYTE4N:
        *size = 4; *type = MOJOSHADER_ATTRIBUTE_UBYTE; *normalized = 1; return true;
    case D3DDECLTYPE_UBYTE4: *size = 4; *type = MOJOSHADER_ATTRIBUTE_UBYTE; return true;
    case D3DDECLTYPE_SHORT2: *size = 2; *type = MOJOSHADER_ATTRIBUTE_SHORT; return true;
    case D3DDECLTYPE_SHORT4: *size = 4; *type = MOJOSHADER_ATTRIBUTE_SHORT; return true;
    case D3DDECLTYPE_SHORT2N: *size = 2; *type = MOJOSHADER_ATTRIBUTE_SHORT; *normalized = 1; return true;
    case D3DDECLTYPE_SHORT4N: *size = 4; *type = MOJOSHADER_ATTRIBUTE_SHORT; *normalized = 1; return true;
    case D3DDECLTYPE_USHORT2N: *size = 2; *type = MOJOSHADER_ATTRIBUTE_USHORT; *normalized = 1; return true;
    case D3DDECLTYPE_USHORT4N: *size = 4; *type = MOJOSHADER_ATTRIBUTE_USHORT; *normalized = 1; return true;
    case D3DDECLTYPE_FLOAT16_2: *size = 2; *type = MOJOSHADER_ATTRIBUTE_HALF_FLOAT; return true;
    case D3DDECLTYPE_FLOAT16_4: *size = 4; *type = MOJOSHADER_ATTRIBUTE_HALF_FLOAT; return true;
    default: return false;
    }
}

/* ---- the fixed-function path: pre-transformed 2D (FVF XYZRHW) -------------------------------- */

const char FIXED_VS[] =
    "#version 300 es\n"
    "uniform highp vec4 viewport;\n" /* x, y, w, h */
    "layout(location = 0) in highp vec4 a_position;\n"
    "layout(location = 1) in lowp vec4 a_colour;\n"
    "layout(location = 2) in highp vec2 a_texcoord;\n"
    "out lowp vec4 v_colour;\n"
    "out highp vec2 v_texcoord;\n"
    "void main()\n"
    "{\n"
    "    vec2 ndc = vec2((a_position.x - viewport.x) / viewport.z * 2.0 - 1.0,\n"
    "                    (a_position.y - viewport.y) / viewport.w * 2.0 - 1.0);\n" /* D3D rows down = our rows */
    "    gl_Position = vec4(ndc, a_position.z * 2.0 - 1.0, 1.0);\n"
    "    v_colour = a_colour.bgra;\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";
const char FIXED_PS[] =
    "#version 300 es\n"
    "uniform lowp sampler2D tex;\n"
    "uniform int textured;\n"
    "in lowp vec4 v_colour;\n"
    "in highp vec2 v_texcoord;\n"
    "layout(location = 0) out lowp vec4 colour;\n"
    "void main()\n"
    "{\n"
    "    colour = textured != 0 ? texture(tex, v_texcoord) * v_colour : v_colour;\n"
    "}\n";

/* Present: the back buffer onto the window as one triangle covering it. The back buffer's row 0
   is the top of the picture, the window's the bottom, hence 1 - y. On the way it goes through the
   game's gamma ramp (SetGammaRamp: r_gamma, the Brightness setting), which on Windows the display
   hardware applies. */
const char PRESENT_VS[] =
    "#version 300 es\n"
    "out highp vec2 uv;\n"
    "void main()\n"
    "{\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
const char PRESENT_PS[] =
    "#version 300 es\n"
    "uniform lowp sampler2D image;\n"
    "uniform lowp sampler2D ramp;\n"
    "uniform lowp float linear_out;\n" /* 1: an sRGB target (the headset), which encodes on write */
    "uniform lowp float keep_alpha;\n" /* 1: the panel, whose alpha is its coverage */
    "in highp vec2 uv;\n"
    "layout(location = 0) out lowp vec4 colour;\n"
    "void main()\n"
    "{\n"
    "    highp vec4 t = texture(image, uv);\n"
    "    highp vec3 c = t.rgb * (255.0 / 256.0) + 0.5 / 256.0;\n"
    "    highp vec3 g = vec3(texture(ramp, vec2(c.r, 0.5)).r, texture(ramp, vec2(c.g, 0.5)).g,\n"
    "                        texture(ramp, vec2(c.b, 0.5)).b);\n"
    "    highp vec3 lin = mix(pow((g + 0.055) / 1.055, vec3(2.4)), g / 12.92, vec3(lessThanEqual(g, vec3(0.04045))));\n"
    "    colour = vec4(mix(g, lin, linear_out), mix(1.0, t.a, keep_alpha));\n"
    "}\n";

GLuint compile(GLenum type, const char *text)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &text, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        bo1_log("d3d9: built-in shader: %s", log);
    }
    return s;
}

/* ---- the device ------------------------------------------------------------------------------ */

struct Stream {
    VertexBuffer *buffer = NULL;
    UINT offset = 0, stride = 0;
};

/* Where a draw's vertices come from: client memory (DrawPrimitiveUP), slices of streamed buffers
   (the draw's vertices `first`.. become 0..), or static GL buffers as they are */
struct VertexSource {
    const uint8_t *client = NULL;
    UINT client_stride = 0, client_at = 0;
    bool streaming = false;
    UINT first = 0, count = 0;
    INT base = 0; /* static buffers: a base vertex applied to the attribute pointers instead of the draw */
    UINT at[16];
    bool uploaded[16] = {};
};

struct ProgramExtras {
    GLint alpha = -1;
    float last[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    GLint layer = -2;      /* bo1_layer_u: the layer a single-view draw reads layered textures at */
    float layer_last = -1.0f;
};

struct Device;
Device *g_device_for_ui; /* the device bo1_d3d9_ui talks to */
struct Device final : Counted<IDirect3DDevice9Ex> {
    D3DPRESENT_PARAMETERS params;
    Surface *back_buffer = NULL, *auto_depth = NULL;
    Surface *targets[4] = {}, *depth = NULL;
    DWORD render_states[256] = {};
    DWORD sampler_states[MAX_SAMPLERS][14] = {};
    bool sampler_dirty[MAX_SAMPLERS] = {};
    bool sampler_depth[MAX_SAMPLERS] = {}; /* the unit's sampler was made for a depth texture */
    IDirect3DBaseTexture9 *textures[MAX_SAMPLERS] = {};
    TextureData *unit_data[MAX_SAMPLERS] = {}; /* texture_data(textures[unit]), worked out by SetTexture */
    DWORD stage_states[8][8] = {};
    Stream streams[16];
    IndexBuffer *indices = NULL;
    VertexDeclaration *declaration = NULL;
    VertexShader *vertex_shader = NULL;
    PixelShader *pixel_shader = NULL;
    DWORD fvf = 0;
    D3DVIEWPORT9 viewport = {};
    RECT scissor = {};
    float vs_constants[256][4] = {}, ps_constants[224][4] = {};
    /* multiview: the right eye's constants - the same but where the engine sends its own
       (bo1_d3d9_eye1_constants) */
    float vs_constants_eye1[256][4] = {}, ps_constants_eye1[224][4] = {};
    bool mv_draw = false; /* the bound framebuffer draws both eyes (FB_MULTIVIEW) */
    unsigned last_layered = 0; /* debug: the layered sampler units of the last pixel shader picked */
    /* multiview, post-processing (bo1_d3d9_split_post until Present): draws are made once per eye
       layer with single-view shaders, the right eye's with its vertex constants. In multiview the
       driver's second view read the final composite's constants as zeros. */
    bool split_post = false;
    int force_layer = -1;
    UINT vs_dirty_from = 256, vs_dirty_to = 0, ps_dirty_from = 224, ps_dirty_to = 0;

    /* GL-thread state */
    bool gl_ready = false;
    MOJOSHADER_glContext *mojo = NULL;
    std::map<std::pair<MOJOSHADER_glShader *, MOJOSHADER_glShader *>, MOJOSHADER_glProgram *> programs;
    std::unordered_map<GLuint, ProgramExtras> extras;
    GLuint samplers[MAX_SAMPLERS] = {};
    std::unordered_map<uint64_t, GLuint> sampler_cache; /* one GL sampler per distinct D3D sampler state */
    GLuint fixed_program = 0, fixed_vao = 0, stream_buffer = 0, present_program = 0, gamma_texture = 0;
    GLuint fixed_program_mv = 0, present_program_array = 0; /* multiview: both eyes; a layered back buffer */
    GLint fixed_viewport_mv = -1, fixed_textured_mv = -1, present_layer = -1, present_array_linear_out = -1,
          present_array_keep_alpha = -1;
    GLint present_linear_out = -1, present_keep_alpha = -1;
    Surface *ui_surface = NULL; /* the panel's image (stereo 2D) */
    bool ui_clear_pending = false, ui_used = false, ui_on = false, ui_paused = false;
    uint8_t gamma[256][4]; /* the gamma ramp as RGBA8 texels: identity until SetGammaRamp */
    bool gamma_dirty = true;
    GLint fixed_viewport = -1, fixed_textured = -1;
    UINT stream_used = 0;

    explicit Device(const D3DPRESENT_PARAMETERS &p)
    {
        g_device_for_ui = this;
        make_buffers(p);
        for (int i = 0; i < 256; i++) gamma[i][0] = gamma[i][1] = gamma[i][2] = (uint8_t)i, gamma[i][3] = 255;
        /* D3D9's defaults */
        render_states[D3DRS_ZENABLE] = D3DZB_TRUE;
        render_states[D3DRS_ZWRITEENABLE] = TRUE;
        render_states[D3DRS_ZFUNC] = 4; /* LESSEQUAL */
        render_states[D3DRS_SRCBLEND] = D3DBLEND_ONE;
        render_states[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
        render_states[D3DRS_CULLMODE] = D3DCULL_CCW;
        render_states[D3DRS_ALPHAFUNC] = 8;
        render_states[D3DRS_STENCILFUNC] = render_states[D3DRS_CCW_STENCILFUNC] = 8;
        render_states[D3DRS_STENCILFAIL] = render_states[D3DRS_STENCILZFAIL] = render_states[D3DRS_STENCILPASS] = 1;
        render_states[D3DRS_CCW_STENCILFAIL] = render_states[D3DRS_CCW_STENCILZFAIL] = render_states[D3DRS_CCW_STENCILPASS] = 1;
        render_states[D3DRS_STENCILMASK] = render_states[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
        render_states[D3DRS_COLORWRITEENABLE] = 0xF;
        render_states[D3DRS_BLENDOP] = render_states[D3DRS_BLENDOPALPHA] = 1;
        render_states[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
        render_states[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
        for (int i = 0; i < MAX_SAMPLERS; i++) {
            sampler_states[i][D3DSAMP_ADDRESSU] = sampler_states[i][D3DSAMP_ADDRESSV] = sampler_states[i][D3DSAMP_ADDRESSW] =
                D3DTADDRESS_WRAP;
            sampler_states[i][D3DSAMP_MAGFILTER] = sampler_states[i][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
            sampler_states[i][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
            sampler_states[i][D3DSAMP_MAXANISOTROPY] = 1;
            sampler_dirty[i] = true;
        }
        stage_states[0][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    }
    ~Device() override { drop_buffers(); }

    void make_buffers(const D3DPRESENT_PARAMETERS &p)
    {
        params = p;
        if (!params.BackBufferWidth) params.BackBufferWidth = 1920;
        if (!params.BackBufferHeight) params.BackBufferHeight = 1080;
        if (params.BackBufferFormat == D3DFMT_UNKNOWN) params.BackBufferFormat = D3DFMT_X8R8G8B8;
        back_buffer = new Surface(Surface::RENDERBUFFER, params.BackBufferWidth, params.BackBufferHeight,
                                  params.BackBufferFormat, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
        back_buffer->sampled = true; /* Present draws it onto the window (and later the eyes) */
        if (params.EnableAutoDepthStencil)
            auto_depth = new Surface(Surface::RENDERBUFFER, params.BackBufferWidth, params.BackBufferHeight,
                                     params.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
        set_target(0, back_buffer);
        set_depth(auto_depth);
        viewport = {0, 0, params.BackBufferWidth, params.BackBufferHeight, 0.0f, 1.0f};
    }
    void drop_buffers()
    {
        for (int i = 0; i < 4; i++) set_target(i, NULL);
        set_depth(NULL);
        if (back_buffer) back_buffer->Release();
        if (auto_depth) auto_depth->Release();
        back_buffer = auto_depth = NULL;
    }

    template <typename T> static void hold(T *&slot, T *value)
    {
        if (value) value->AddRef();
        if (slot) slot->Release();
        slot = value;
    }
    void set_target(int i, Surface *s) { hold(targets[i], s); }
    void set_depth(Surface *s) { hold(depth, s); }

    /* ---- the GL thread ---- */

    bool ensure_gl()
    {
        if (g_gl_failed) return false;
        if (!g_gl_owned) {
            g_gl_thread = pthread_self();
            g_gl_owned = true;
            if (!host_gl_begin(g_window_size)) {
                g_gl_failed = true;
                return false;
            }
        } else if (!pthread_equal(g_gl_thread, pthread_self())) {
            static int warned;
            if (warned++ < 5) bo1_log("d3d9: GL work from a second thread skipped");
            return false;
        }
        if (!gl_ready) {
            gl_ready = true;
            init_gl();
        }
        run_deletions();
        return mojo != NULL;
    }

    void init_gl()
    {
        g_has_s3tc = has_extension("GL_EXT_texture_compression_s3tc");
        g_has_aniso = has_extension("GL_EXT_texture_filter_anisotropic");
        bo1_log("d3d9: S3TC %s, anisotropic filtering %s", g_has_s3tc ? "yes" : "no (DXT decoded on the CPU)",
                g_has_aniso ? "yes" : "no");
        mojo = MOJOSHADER_glCreateContext(MOJOSHADER_PROFILE_GLSLES3, gl_lookup, NULL, NULL, NULL, NULL);
        if (!mojo) {
            bo1_log("d3d9: MojoShader: %s", MOJOSHADER_glGetError());
            return;
        }
        MOJOSHADER_glMakeContextCurrent(mojo);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glFrontFace(GL_CCW); /* D3D's clockwise front faces, seen through the flipped rows */
        glGenSamplers(MAX_SAMPLERS, samplers);

        GLuint vs = compile(GL_VERTEX_SHADER, FIXED_VS), ps = compile(GL_FRAGMENT_SHADER, FIXED_PS);
        fixed_program = glCreateProgram();
        glAttachShader(fixed_program, vs);
        glAttachShader(fixed_program, ps);
        glLinkProgram(fixed_program);
        glDeleteShader(vs);
        glDeleteShader(ps);
        fixed_viewport = glGetUniformLocation(fixed_program, "viewport");
        fixed_textured = glGetUniformLocation(fixed_program, "textured");
        glUseProgram(fixed_program);
        glUniform1i(glGetUniformLocation(fixed_program, "tex"), 0);
        glUseProgram(0);
        g_bo1_mv_capable = host_gl_has_multiview() != 0;
        if (g_bo1_mv_capable) {
            std::string mv_vs = FIXED_VS;
            mv_vs.insert(mv_vs.find('\n') + 1, "#extension GL_OVR_multiview2 : require\nlayout(num_views = 2) in;\n");
            vs = compile(GL_VERTEX_SHADER, mv_vs.c_str());
            ps = compile(GL_FRAGMENT_SHADER, FIXED_PS);
            fixed_program_mv = glCreateProgram();
            glAttachShader(fixed_program_mv, vs);
            glAttachShader(fixed_program_mv, ps);
            glLinkProgram(fixed_program_mv);
            glDeleteShader(vs);
            glDeleteShader(ps);
            fixed_viewport_mv = glGetUniformLocation(fixed_program_mv, "viewport");
            fixed_textured_mv = glGetUniformLocation(fixed_program_mv, "textured");
            glUseProgram(fixed_program_mv);
            glUniform1i(glGetUniformLocation(fixed_program_mv, "tex"), 0);
            glUseProgram(0);
        }
        {
            /* a layered back buffer (multiview storage) is presented a layer at a time */
            std::string array_ps = PRESENT_PS;
            replace_all(array_ps, "uniform lowp sampler2D image;", "uniform lowp sampler2DArray image;\nuniform highp float layer;");
            replace_all(array_ps, "texture(image, uv)", "texture(image, vec3(uv, layer))");
            GLuint avs = compile(GL_VERTEX_SHADER, PRESENT_VS), aps = compile(GL_FRAGMENT_SHADER, array_ps.c_str());
            present_program_array = glCreateProgram();
            glAttachShader(present_program_array, avs);
            glAttachShader(present_program_array, aps);
            glLinkProgram(present_program_array);
            glDeleteShader(avs);
            glDeleteShader(aps);
            glUseProgram(present_program_array);
            glUniform1i(glGetUniformLocation(present_program_array, "image"), 0);
            glUniform1i(glGetUniformLocation(present_program_array, "ramp"), 1);
            present_layer = glGetUniformLocation(present_program_array, "layer");
            present_array_linear_out = glGetUniformLocation(present_program_array, "linear_out");
            present_array_keep_alpha = glGetUniformLocation(present_program_array, "keep_alpha");
            glUseProgram(0);
        }
        glGenVertexArrays(1, &fixed_vao);
        stream_buffer = host_gl_stream_create(STREAM_PARTS * STREAM_PART); /* stays bound to GL_COPY_WRITE_BUFFER */

        vs = compile(GL_VERTEX_SHADER, PRESENT_VS);
        ps = compile(GL_FRAGMENT_SHADER, PRESENT_PS);
        present_program = glCreateProgram();
        glAttachShader(present_program, vs);
        glAttachShader(present_program, ps);
        glLinkProgram(present_program);
        glDeleteShader(vs);
        glDeleteShader(ps);
        glUseProgram(present_program);
        glUniform1i(glGetUniformLocation(present_program, "image"), 0);
        glUniform1i(glGetUniformLocation(present_program, "ramp"), 1);
        present_linear_out = glGetUniformLocation(present_program, "linear_out");
        present_keep_alpha = glGetUniformLocation(present_program, "keep_alpha");
        glUseProgram(0);
        glGenTextures(1, &gamma_texture);
        glBindTexture(GL_TEXTURE_2D, gamma_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 256, 1);
        gamma_dirty = true;
    }

    bool ui_target() const { return g_bo1_ui_redirect && targets[0] == back_buffer && ui_surface; }
    FramebufferKey current_key() const
    {
        FramebufferKey k;
        memcpy(k.colour, targets, sizeof k.colour);
        if (ui_target()) k.colour[0] = ui_surface;
        k.depth = depth;
        k.mode = FB_LAYER0;
        if (g_bo1_render_eye == 2) {
            /* a multiview frame: targets that are all layered take both eyes at once */
            int count = 0, layered = 0;
            for (Surface *a : {k.colour[0], k.colour[1], k.colour[2], k.colour[3], k.depth})
                if (a) {
                    count++;
                    layered += a->layered() ? 1 : 0;
                }
            if (count && layered == count) {
                k.mode = force_layer >= 0 ? (force_layer ? FB_LAYER1 : FB_LAYER0) : FB_MULTIVIEW;
            } else if (layered) {
                static int warned;
                if (warned++ < 10)
                    bo1_log("d3d9: multiview frame drawing into layered and single targets together (colour %ux%u fmt %u); "
                            "left eye only", k.colour[0] ? k.colour[0]->desc.Width : 0, k.colour[0] ? k.colour[0]->desc.Height : 0,
                            k.colour[0] ? (unsigned)k.colour[0]->desc.Format : 0);
            }
        }
        return k;
    }
    /* binds the current targets; the panel's image starts each frame clear (transparent) */
    void bind_current()
    {
        FramebufferKey key = current_key();
        if (memcmp(&key, &last_bind_key, sizeof key)) {
            last_bind_key = key;
            binds_this_frame++;
            if (mid_shot_pending && mid_shot_at >= 0 && binds_this_frame == mid_shot_at) {
                mid_shot_pending = false;
                write_shot();
                bo1_log("d3d9: shot at framebuffer change %d (%ux%u next)", mid_shot_at, key.colour[0] ? key.colour[0]->desc.Width : 0,
                        key.colour[0] ? key.colour[0]->desc.Height : 0);
            }
        }
        if (mid_shot_pending && mid_shot_at < 0 && key.colour[0] && key.colour[0]->desc.Width == params.BackBufferWidth / 2) {
            mid_shot_pending = false;
            write_shot();
        }
        mv_draw = key.mode == FB_MULTIVIEW;
        bind_framebuffer(key);
        if (ui_target()) {
            ui_used = true;
            if (ui_clear_pending) {
                ui_clear_pending = false;
                glDisable(GL_SCISSOR_TEST);
                glColorMask(1, 1, 1, 1);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
    }
    void ui_redirect(bool on)
    {
        if (on && !ui_surface && back_buffer) {
            ui_surface = new Surface(Surface::RENDERBUFFER, params.BackBufferWidth, params.BackBufferHeight, D3DFMT_A8R8G8B8,
                                     D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
            ui_surface->sampled = true;
        }
        if (on) ui_clear_pending = true;
        ui_on = on && ui_surface;
        ui_paused = false;
        g_bo1_ui_redirect = ui_on;
    }

    void apply_sampler(int unit)
    {
        TextureData *t = unit_data[unit];
        /* realize binds the texture to make or fill it: on this unit, not on whichever was active
           (that put a texture over the previous unit's) */
        glActiveTexture(GL_TEXTURE0 + unit);
        if (t) t->realize();
        bool depth_texture = t && t->format.depth;
        if (depth_texture != sampler_depth[unit]) {
            sampler_depth[unit] = depth_texture;
            sampler_dirty[unit] = true;
        }
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(t ? t->target : GL_TEXTURE_2D, t && t->gpu() ? t->name : 0);
        if (!sampler_dirty[unit]) {
            glBindSampler(unit, samplers[unit]);
            return;
        }
        sampler_dirty[unit] = false;
        const DWORD *s = sampler_states[unit];
        /* the states that make up a GL sampler; one is made the first time each combination is seen */
        uint64_t key = (uint64_t)(s[D3DSAMP_ADDRESSU] & 15) | (uint64_t)(s[D3DSAMP_ADDRESSV] & 15) << 4 |
                       (uint64_t)(s[D3DSAMP_ADDRESSW] & 15) << 8 | (uint64_t)(s[D3DSAMP_MAGFILTER] & 15) << 12 |
                       (uint64_t)(s[D3DSAMP_MINFILTER] & 15) << 16 | (uint64_t)(s[D3DSAMP_MIPFILTER] & 15) << 20 |
                       (uint64_t)(s[D3DSAMP_MAXMIPLEVEL] & 255) << 24 | (uint64_t)(s[D3DSAMP_MAXANISOTROPY] & 255) << 32 |
                       (uint64_t)depth_texture << 40;
        GLuint &g = sampler_cache[key];
        if (g) {
            samplers[unit] = g;
            glBindSampler(unit, g);
            return;
        }
        glGenSamplers(1, &g);
        samplers[unit] = g;
        glSamplerParameteri(g, GL_TEXTURE_WRAP_S, gl_address(s[D3DSAMP_ADDRESSU]));
        glSamplerParameteri(g, GL_TEXTURE_WRAP_T, gl_address(s[D3DSAMP_ADDRESSV]));
        glSamplerParameteri(g, GL_TEXTURE_WRAP_R, gl_address(s[D3DSAMP_ADDRESSW]));
        bool mag_linear = s[D3DSAMP_MAGFILTER] >= D3DTEXF_LINEAR && !depth_texture,
             min_linear = s[D3DSAMP_MINFILTER] >= D3DTEXF_LINEAR;
        glSamplerParameteri(g, GL_TEXTURE_MAG_FILTER, mag_linear ? GL_LINEAR : GL_NEAREST);
        GLenum min;
        switch (s[D3DSAMP_MIPFILTER]) {
        case D3DTEXF_NONE: min = min_linear ? GL_LINEAR : GL_NEAREST; break;
        case D3DTEXF_POINT: min = min_linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST; break;
        default: min = min_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR; break;
        }
        if (depth_texture) mag_linear = false, min = GL_NEAREST;
        glSamplerParameteri(g, GL_TEXTURE_MIN_FILTER, min);
        glSamplerParameterf(g, GL_TEXTURE_MIN_LOD, (float)s[D3DSAMP_MAXMIPLEVEL]);
        if (g_has_aniso) {
            float aniso = s[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC || s[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC
                              ? (float)(s[D3DSAMP_MAXANISOTROPY] ? s[D3DSAMP_MAXANISOTROPY] : 1)
                              : 1.0f;
            glSamplerParameterf(g, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
        }
        glBindSampler(unit, g);
    }

    void apply_render_states(bool fixed)
    {
        const DWORD *rs = render_states;
        bind_current();
        glViewport((GLint)viewport.X, (GLint)viewport.Y, (GLsizei)viewport.Width, (GLsizei)viewport.Height);
        glDepthRangef(viewport.MinZ, viewport.MaxZ);
        if (rs[D3DRS_SCISSORTESTENABLE]) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor.left, scissor.top, scissor.right - scissor.left, scissor.bottom - scissor.top);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        bool depth_on = rs[D3DRS_ZENABLE] != D3DZB_FALSE && depth;
        if (depth_on) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        {
            /* <data>/d3d9_debug bits (read every 36 frames): 1 = EQUAL depth tests as LEQUAL,
               2 = no depth test at all */
            DWORD zfunc = rs[D3DRS_ZFUNC];
            if ((g_debug_flags & 1) && zfunc == 3 /* EQUAL */) zfunc = 4; /* LESSEQUAL */
            if (g_debug_flags & 2) zfunc = 8; /* ALWAYS */
            glDepthFunc(gl_compare(zfunc));
        }
        glDepthMask(rs[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);

        if (rs[D3DRS_ALPHABLENDENABLE]) {
            glEnable(GL_BLEND);
            if (rs[D3DRS_SEPARATEALPHABLENDENABLE]) {
                glBlendFuncSeparate(gl_blend(rs[D3DRS_SRCBLEND]), gl_blend(rs[D3DRS_DESTBLEND]),
                                    gl_blend(rs[D3DRS_SRCBLENDALPHA]), gl_blend(rs[D3DRS_DESTBLENDALPHA]));
                glBlendEquationSeparate(gl_blend_op(rs[D3DRS_BLENDOP]), gl_blend_op(rs[D3DRS_BLENDOPALPHA]));
            } else {
                glBlendFuncSeparate(gl_blend(rs[D3DRS_SRCBLEND]), gl_blend(rs[D3DRS_DESTBLEND]),
                                    gl_blend(rs[D3DRS_SRCBLEND]), gl_blend(rs[D3DRS_DESTBLEND]));
                glBlendEquationSeparate(gl_blend_op(rs[D3DRS_BLENDOP]), gl_blend_op(rs[D3DRS_BLENDOP]));
            }
            DWORD f = rs[D3DRS_BLENDFACTOR];
            glBlendColor((f >> 16 & 255) / 255.0f, (f >> 8 & 255) / 255.0f, (f & 255) / 255.0f, (f >> 24) / 255.0f);
        } else {
            glDisable(GL_BLEND);
        }
        DWORD mask = rs[D3DRS_COLORWRITEENABLE];
        if (ui_target()) {
            /* the panel: its alpha is how much of the world the 2D covers - "over" whatever the colour
               blend, written even where the engine (drawing onto an opaque back buffer) masked it */
            if (rs[D3DRS_ALPHABLENDENABLE]) {
                glBlendFuncSeparate(gl_blend(rs[D3DRS_SRCBLEND]), gl_blend(rs[D3DRS_DESTBLEND]), GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glBlendEquationSeparate(gl_blend_op(rs[D3DRS_BLENDOP]), GL_FUNC_ADD);
            }
            if (mask & 7) mask |= 8;
        }
        glColorMask(mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
        if ((g_debug_flags & 64) && rs[D3DRS_ZENABLE]) {
            /* debug: depth-tested draws write solid magenta whatever the shader says */
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ZERO, GL_ONE, GL_ZERO);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendColor(1.0f, 0.0f, 1.0f, 1.0f);
        }

        DWORD cull = fixed || (g_debug_flags & 128) ? D3DCULL_NONE : rs[D3DRS_CULLMODE]; /* 128: debug, no culling */
        if (cull == D3DCULL_NONE) glDisable(GL_CULL_FACE);
        else {
            glEnable(GL_CULL_FACE);
            glCullFace(cull == D3DCULL_CCW ? GL_BACK : GL_FRONT);
        }

        if (rs[D3DRS_STENCILENABLE] && depth && depth->format.stencil) {
            glEnable(GL_STENCIL_TEST);
            bool two = rs[D3DRS_TWOSIDEDSTENCILMODE];
            GLint ref = (GLint)rs[D3DRS_STENCILREF];
            GLuint read = rs[D3DRS_STENCILMASK];
            glStencilFuncSeparate(GL_FRONT, gl_compare(rs[D3DRS_STENCILFUNC]), ref, read);
            glStencilOpSeparate(GL_FRONT, gl_stencil_op(rs[D3DRS_STENCILFAIL]), gl_stencil_op(rs[D3DRS_STENCILZFAIL]),
                                gl_stencil_op(rs[D3DRS_STENCILPASS]));
            glStencilFuncSeparate(GL_BACK, gl_compare(rs[two ? D3DRS_CCW_STENCILFUNC : D3DRS_STENCILFUNC]), ref, read);
            glStencilOpSeparate(GL_BACK, gl_stencil_op(rs[two ? D3DRS_CCW_STENCILFAIL : D3DRS_STENCILFAIL]),
                                gl_stencil_op(rs[two ? D3DRS_CCW_STENCILZFAIL : D3DRS_STENCILZFAIL]),
                                gl_stencil_op(rs[two ? D3DRS_CCW_STENCILPASS : D3DRS_STENCILPASS]));
            glStencilMaskSeparate(GL_FRONT_AND_BACK, rs[D3DRS_STENCILWRITEMASK]);
        } else {
            glDisable(GL_STENCIL_TEST);
        }

        float bias, slope;
        memcpy(&bias, &rs[D3DRS_DEPTHBIAS], 4);
        memcpy(&slope, &rs[D3DRS_SLOPESCALEDEPTHBIAS], 4);
        if (bias != 0.0f || slope != 0.0f) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(slope, bias * 16777216.0f);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    VertexShader *variant_shader = NULL;
    VertexDeclaration *variant_declaration = NULL;
    MOJOSHADER_glShader *variant_last = NULL;
    bool variant_mv = false;
    MOJOSHADER_glShader *vertex_variant()
    {
        if (vertex_shader == variant_shader && declaration == variant_declaration && variant_mv == mv_draw && variant_last)
            return variant_last;
        variant_shader = vertex_shader;
        variant_declaration = declaration;
        variant_mv = mv_draw;
        variant_last = vertex_variant_lookup();
        return variant_last;
    }
    MOJOSHADER_glShader *vertex_variant_lookup()
    {
        static const std::string none;
        std::string key = declaration ? declaration->key : none;
        if (mv_draw) key += "|mv";
        auto found = vertex_shader->variants.find(key);
        if (found != vertex_shader->variants.end()) return found->second;
        g_vs_compiles++;
        g_compile_mode.multiview = mv_draw;
        g_compile_mode.layered = 0;
        struct Reset {
            ~Reset() { g_compile_mode = CompileMode(); }
        } reset;
        MOJOSHADER_glShader *s = MOJOSHADER_glCompileShader(
            vertex_shader->bytecode.data(), (unsigned)vertex_shader->bytecode.size(),
            declaration && !declaration->swizzles.empty() ? declaration->swizzles.data() : NULL,
            declaration ? (unsigned)declaration->swizzles.size() : 0, NULL, 0);
        if (!s) bo1_log("d3d9: vertex shader: %s", MOJOSHADER_glGetError());
        vertex_shader->variants[key] = s;
        return s;
    }

    /* the GLSL of both halves goes to <data folder>/glsl_errors/, the first few times */
    void report_link_failure(MOJOSHADER_glShader *vs, MOJOSHADER_glShader *ps)
    {
        static int reported;
        bo1_log("d3d9: link failed: %s", MOJOSHADER_glGetError());
        if (reported >= 8) return;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s/glsl_errors", bo1_data_root());
        CreateDirectoryA(path, NULL);
        snprintf(path, sizeof path, "%s/glsl_errors/link_%d.txt", bo1_data_root(), reported++);
        FILE *f = fopen(path, "w");
        if (!f) return;
        const MOJOSHADER_parseData *v = MOJOSHADER_glGetShaderParseData(vs), *p = MOJOSHADER_glGetShaderParseData(ps);
        fprintf(f, "error: %s\n", MOJOSHADER_glGetError());
        /* the same link by hand, for the driver's own verdict */
        if (v && v->output && p && p->output) {
            GLuint shaders[2] = {hooked_glCreateShader(GL_VERTEX_SHADER), hooked_glCreateShader(GL_FRAGMENT_SHADER)};
            const char *texts[2] = {v->output, p->output};
            GLuint program = glCreateProgram();
            for (int i = 0; i < 2; i++) {
                hooked_glShaderSource(shaders[i], 1, &texts[i], NULL);
                glCompileShader(shaders[i]);
                GLint ok = -1;
                char log[2048] = "";
                glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
                glGetShaderInfoLog(shaders[i], sizeof log, NULL, log);
                fprintf(f, "%s compile status %d: %s\n", i ? "pixel" : "vertex", ok, log);
                glAttachShader(program, shaders[i]);
            }
            glLinkProgram(program);
            GLint ok = -1;
            char log[2048] = "";
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            glGetProgramInfoLog(program, sizeof log, NULL, log);
            fprintf(f, "link status %d, GL error 0x%x: %s\n", ok, glGetError(), log);
            glDeleteProgram(program);
            glDeleteShader(shaders[0]);
            glDeleteShader(shaders[1]);
            /* and without the alpha test added */
            program = glCreateProgram();
            for (int i = 0; i < 2; i++) {
                shaders[i] = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
                glShaderSource(shaders[i], 1, &texts[i], NULL);
                glCompileShader(shaders[i]);
                glAttachShader(program, shaders[i]);
            }
            glLinkProgram(program);
            ok = -1;
            log[0] = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            glGetProgramInfoLog(program, sizeof log, NULL, log);
            fprintf(f, "plain link status %d: %s\n\n--- pixel as injected ---\n%s\n", ok, log, g_last_injected.c_str());
            glDeleteProgram(program);
            glDeleteShader(shaders[0]);
            glDeleteShader(shaders[1]);
        }
        fprintf(f, "\n--- vertex ---\n%s\n\n--- pixel ---\n%s\n", v && v->output ? v->output : "(none)",
                p && p->output ? p->output : "(none)");
        fclose(f);
    }

    MOJOSHADER_glShader *pixel_compiled()
    {
        if (!pixel_shader->compiled && !pixel_shader->failed) {
            g_ps_compiles++;
            pixel_shader->compiled = MOJOSHADER_glCompileShader(pixel_shader->bytecode.data(),
                                                                (unsigned)pixel_shader->bytecode.size(), NULL, 0, NULL, 0);
            if (!pixel_shader->compiled) {
                pixel_shader->failed = true;
                bo1_log("d3d9: pixel shader: %s", MOJOSHADER_glGetError());
            } else if (const MOJOSHADER_parseData *pd = MOJOSHADER_glGetShaderParseData(pixel_shader->compiled)) {
                for (int i = 0; i < pd->sampler_count; i++)
                    if (pd->samplers[i].index >= 0 && pd->samplers[i].index < 16)
                        pixel_shader->sampler_units |= 1u << pd->samplers[i].index;
            }
        }
        if (!pixel_shader->compiled) return NULL;
        /* the variant for this draw: multiview, and which of its samplers read layered textures */
        unsigned layered = 0;
        for (int unit = 0; unit < MAX_SAMPLERS && unit < 16; unit++)
            if ((pixel_shader->sampler_units >> unit & 1) && unit_data[unit] && unit_data[unit]->layers == 2) layered |= 1u << unit;
        last_layered = layered;
        if (!mv_draw && !layered) return pixel_shader->compiled;
        unsigned key = layered | (mv_draw ? 1u << 16 : 0);
        auto found = pixel_shader->variants.find(key);
        if (found != pixel_shader->variants.end()) return found->second;
        g_ps_compiles++;
        g_compile_mode.multiview = mv_draw;
        g_compile_mode.layered = layered;
        MOJOSHADER_glShader *v = MOJOSHADER_glCompileShader(pixel_shader->bytecode.data(), (unsigned)pixel_shader->bytecode.size(),
                                                            NULL, 0, NULL, 0);
        g_compile_mode = CompileMode();
        if (!v) bo1_log("d3d9: pixel shader variant %#x: %s", key, MOJOSHADER_glGetError());
        pixel_shader->variants[key] = v;
        return v;
    }

    /* multiview: the right eye's copy of the constants a program reads, laid out as MojoShader
       packs them (its parse data's float uniforms, in order) */
    struct EyeOneUniforms {
        GLint vs_location = -1, ps_location = -1, clip_location = -1;
        float clip_last[16] = {-1};
        std::vector<uint16_t> vs_registers, ps_registers;
        std::vector<float> vs_last, ps_last;
    };
    std::unordered_map<GLuint, EyeOneUniforms> eye1_uniforms;
    static void float_registers(MOJOSHADER_glShader *shader, std::vector<uint16_t> &out)
    {
        const MOJOSHADER_parseData *pd = MOJOSHADER_glGetShaderParseData(shader);
        if (!pd) return;
        for (int i = 0; i < pd->uniform_count; i++) {
            const MOJOSHADER_uniform &u = pd->uniforms[i];
            if (u.constant || u.type != MOJOSHADER_UNIFORM_FLOAT) continue;
            int size = u.array_count ? u.array_count : 1;
            for (int r = 0; r < size; r++) out.push_back((uint16_t)(u.index + r));
        }
    }
    static void push_eye1(GLint location, const std::vector<uint16_t> &regs, std::vector<float> &last, const float (*file)[4],
                          unsigned limit)
    {
        if (location < 0 || regs.empty()) return;
        static float data[256 * 4];
        size_t n = regs.size() < 256 ? regs.size() : 256;
        for (size_t i = 0; i < n; i++) memcpy(&data[i * 4], file[regs[i] < limit ? regs[i] : 0], 16);
        if (last.size() == n * 4 && !memcmp(last.data(), data, n * 16)) return;
        last.assign(data, data + n * 4);
        glUniform4fv(location, (GLsizei)n, data);
    }
    void upload_eye1(GLuint program, MOJOSHADER_glShader *vs, MOJOSHADER_glShader *ps)
    {
        auto found = eye1_uniforms.find(program);
        if (found == eye1_uniforms.end()) {
            EyeOneUniforms u;
            u.vs_location = glGetUniformLocation(program, "bo1_e1_vs");
            u.ps_location = glGetUniformLocation(program, "bo1_e1_ps");
            u.clip_location = glGetUniformLocation(program, "bo1_clip1");
            float_registers(vs, u.vs_registers);
            float_registers(ps, u.ps_registers);
            found = eye1_uniforms.emplace(program, std::move(u)).first;
        }
        EyeOneUniforms &u = found->second;
        push_eye1(u.vs_location, u.vs_registers, u.vs_last, vs_constants_eye1, 256);
        push_eye1(u.ps_location, u.ps_registers, u.ps_last, ps_constants_eye1, 224);
        if (u.clip_location >= 0 && memcmp(u.clip_last, g_clip1, sizeof g_clip1)) {
            /* row vectors in the engine's layout read as columns: bo1_clip1 * p is p * m */
            glUniformMatrix4fv(u.clip_location, 1, GL_FALSE, g_clip1);
            memcpy(u.clip_last, g_clip1, sizeof g_clip1);
        }
    }

    /* shader draws: program, constants, textures, vertex inputs; false to skip the draw */
    bool prepare_shader_draw(VertexSource &source)
    {
        if (!vertex_shader || !pixel_shader || !declaration) return false;
        MOJOSHADER_glShader *vs = vertex_variant(), *ps = pixel_compiled();
        if (!vs || !ps) return false;
        static MOJOSHADER_glShader *last_vs, *last_ps;
        static MOJOSHADER_glProgram **last_slot;
        if (vs != last_vs || ps != last_ps || !last_slot) {
            last_slot = &programs[{vs, ps}];
            last_vs = vs;
            last_ps = ps;
        }
        MOJOSHADER_glProgram *&program = *last_slot;
        if (!program) {
            g_links++;
            program = MOJOSHADER_glLinkProgram(vs, ps);
            if (!program) {
                report_link_failure(vs, ps);
                return false;
            }
        }
        glBindVertexArray(0);
        MOJOSHADER_glBindProgram(program);

        if (vs_dirty_from < vs_dirty_to) {
            MOJOSHADER_glSetVertexShaderUniformF(vs_dirty_from, vs_constants[vs_dirty_from], vs_dirty_to - vs_dirty_from);
            vs_dirty_from = 256;
            vs_dirty_to = 0;
        }
        if (force_layer == 1) {
            /* a split draw's right eye: its vertex constants, put back after the draw (draw_split) */
            MOJOSHADER_glSetVertexShaderUniformF(0, vs_constants_eye1[0], 256);
            vs_dirty_from = 0;
            vs_dirty_to = 256;
        }
        if (ps_dirty_from < ps_dirty_to) {
            MOJOSHADER_glSetPixelShaderUniformF(ps_dirty_from, ps_constants[ps_dirty_from], ps_dirty_to - ps_dirty_from);
            ps_dirty_from = 224;
            ps_dirty_to = 0;
        }

        for (int unit = 0; unit < MAX_SAMPLERS; unit++)
            if (textures[unit] || sampler_dirty[unit]) {
                /* nothing to do when the unit already holds it: same texture, uploaded, same sampler */
                const TextureData *t = unit_data[unit];
                if (t && !sampler_dirty[unit] && t->name && !t->dirty && t->gpu() && t->format.depth == sampler_depth[unit] &&
                    gl_cache::unit_has((unsigned)unit, t->target, t->name, samplers[unit]))
                    continue;
                apply_sampler(unit);
            }
        if ((g_debug_flags & 16) && render_states[D3DRS_ZENABLE]) {
            /* debug: the units in bits 8.. of the flags sample plain white */
            static GLuint white;
            if (!white) {
                const uint8_t px[4] = {255, 255, 255, 255};
                glGenTextures(1, &white);
                glBindTexture(GL_TEXTURE_2D, white);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            }
            for (int unit = 0; unit < MAX_SAMPLERS; unit++) {
                TextureData *t = texture_data(textures[unit]);
                if (!(g_debug_flags >> 8 & (1u << unit)) || !t) continue;
                static GLuint white3d;
                if (!white3d) {
                    const uint8_t px[4] = {255, 255, 255, 255};
                    glGenTextures(1, &white3d);
                    glBindTexture(GL_TEXTURE_3D, white3d);
                    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                }
                if (t->target != GL_TEXTURE_2D && t->target != GL_TEXTURE_3D) continue;
                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(t->target, t->target == GL_TEXTURE_3D ? white3d : white);
                sampler_dirty[unit] = true; /* rebind the real one next draw */
            }
        }

        for (const D3DVERTEXELEMENT9 &e : declaration->elements) {
            unsigned int size;
            MOJOSHADER_attributeType type;
            int normalized;
            if (!attribute_type(e.Type, &size, &type, &normalized)) {
                static uint32_t reported; /* each unsupported type once */
                if (e.Type < 32 && !(reported & 1u << e.Type)) {
                    reported |= 1u << e.Type;
                    bo1_log("d3d9: vertex element type %u (usage %u.%u) unsupported; left out", e.Type, e.Usage, e.UsageIndex);
                }
                continue;
            }
            UINT stride;
            const void *pointer;
            if (!vertex_pointer(source, e.Stream, e.Offset, &stride, &pointer)) continue;
            MOJOSHADER_glSetVertexAttribute((MOJOSHADER_usage)e.Usage, e.UsageIndex, size, type, normalized, stride, pointer);
        }

        MOJOSHADER_glProgramReady();
        MOJOSHADER_glProgramViewportInfo((int)viewport.Width, (int)viewport.Height, (int)params.BackBufferWidth,
                                         (int)params.BackBufferHeight, 1);

        /* the GL program MojoShader bound: asked of GL only when the program changes */
        static MOJOSHADER_glProgram *last_program;
        static GLint current;
        if (program != last_program) {
            glGetIntegerv(GL_CURRENT_PROGRAM, &current);
            last_program = program;
        }
        auto found = extras.find((GLuint)current);
        if (found == extras.end()) {
            ProgramExtras x;
            x.alpha = glGetUniformLocation((GLuint)current, "bo1_alpha");
            found = extras.emplace((GLuint)current, x).first;
        }
        if (!g_deleted_programs.empty()) {
            for (GLuint gone : g_deleted_programs) {
                eye1_uniforms.erase(gone);
                if (gone != (GLuint)current) extras.erase(gone);
            }
            g_deleted_programs.clear();
        }
        if (mv_draw) upload_eye1((GLuint)current, vs, ps);
        if (mv_draw && (g_debug_flags & 1)) {
            /* debug: 3D draws whose vertex constants are the same for both eyes (drawn where the left eye
               sees them, in both) - the first few of each shader pair a frame */
            auto e1 = eye1_uniforms.find((GLuint)current);
            bool same = true;
            if (e1 != eye1_uniforms.end())
                for (uint16_t r : e1->second.vs_registers)
                    if (r < 256 && memcmp(vs_constants[r], vs_constants_eye1[r], 16)) {
                        same = false;
                        break;
                    }
            static std::set<std::pair<const void *, const void *>> seen;
            static unsigned frame_seen;
            if (same && e1 != eye1_uniforms.end() && seen.count({vertex_shader, pixel_shader}) == 0)
                for (uint16_t r : e1->second.vs_registers)
                    bo1_log("DEBUG   c%u = %g %g %g %g", r, vs_constants[r][0], vs_constants[r][1], vs_constants[r][2], vs_constants[r][3]);
            if (same && seen.insert({vertex_shader, pixel_shader}).second && frame_seen++ < 60)
                bo1_log("DEBUG same-for-both-eyes draw: vs %p ps %p blend %lu %lu/%lu z %lu/%lu, %u vs regs, c0 %g %g %g %g",
                        (void *)vertex_shader, (void *)pixel_shader, (unsigned long)render_states[D3DRS_ALPHABLENDENABLE],
                        (unsigned long)render_states[D3DRS_SRCBLEND], (unsigned long)render_states[D3DRS_DESTBLEND],
                        (unsigned long)render_states[D3DRS_ZENABLE], (unsigned long)render_states[D3DRS_ZWRITEENABLE],
                        e1 != eye1_uniforms.end() ? (unsigned)e1->second.vs_registers.size() : 0u, vs_constants[0][0],
                        vs_constants[0][1], vs_constants[0][2], vs_constants[0][3]);
        }
        ProgramExtras &x = found->second;
        if (!mv_draw) {
            if (x.layer == -2) x.layer = glGetUniformLocation((GLuint)current, "bo1_layer_u");
            float layer = force_layer == 1 ? 1.0f : 0.0f;
            if (x.layer >= 0 && x.layer_last != layer) {
                glUniform1f(x.layer, layer);
                x.layer_last = layer;
            }
        }
        if (x.alpha >= 0) {
            float v[4] = {(render_states[D3DRS_ALPHAREF] & 255) / 255.0f};
            alpha_sides(render_states[D3DRS_ALPHATESTENABLE] ? render_states[D3DRS_ALPHAFUNC] : 8, v + 1);
            if (memcmp(v, x.last, sizeof v)) {
                glUniform4fv(x.alpha, 1, v);
                memcpy(x.last, v, sizeof v);
            }
        }
        return true;
    }

    /* fixed-function draws: XYZRHW positions with a colour and one texture */
    bool prepare_fixed_draw(VertexSource &source)
    {
        if (!(fvf & D3DFVF_XYZRHW)) return false;
        MOJOSHADER_glBindProgram(NULL); /* so MojoShader binds its program again after this one */
        glBindVertexArray(fixed_vao);
        bool mv = mv_draw && fixed_program_mv;
        glUseProgram(mv ? fixed_program_mv : fixed_program);
        set_fixed_viewport();
        TextureData *t = texture_data(textures[0]);
        bool textured = t && (fvf & D3DFVF_TEX1) && stage_states[0][D3DTSS_COLOROP] != D3DTOP_DISABLE;
        glUniform1i(mv ? fixed_textured_mv : fixed_textured, textured ? 1 : 0);
        if (textured) apply_sampler(0);
        UINT offset = 0, stride;
        const void *base;
        if (!vertex_pointer(source, 0, 0, &stride, &base)) return false;
        const uint8_t *b = (const uint8_t *)base;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, b);
        offset = 16;
        if (fvf & D3DFVF_DIFFUSE) {
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, b + offset);
            offset += 4;
        } else {
            glDisableVertexAttribArray(1);
            glVertexAttrib4f(1, 1, 1, 1, 1);
        }
        if (fvf & D3DFVF_SPECULAR) offset += 4;
        if (fvf & D3DFVF_TEX1) {
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (GLsizei)stride, b + offset);
        } else {
            glDisableVertexAttribArray(2);
        }
        return true;
    }
    void set_fixed_viewport()
    {
        float v[4] = {(float)viewport.X, (float)viewport.Y, (float)viewport.Width, (float)viewport.Height};
        glUniform4fv(mv_draw && fixed_program_mv ? fixed_viewport_mv : fixed_viewport, 1, v);
    }

    bool fixed_function() const { return !vertex_shader; }

    /* ---- the streaming ring ----
       The host's persistently mapped buffer (host_gl.c), in STREAM_PARTS parts of STREAM_PART
       bytes. Draws append their changing data to the current part; the part changes at the end
       of each frame, or when a frame fills one. Leaving a part fences it; entering one waits for
       its fence, set frames ago, so the GPU is done with the old contents. */
    static const UINT STREAM_PARTS = 3, STREAM_PART = 16u << 20;
    UINT stream_part = 0;
    UINT stats_draws = 0, stats_streamed_draws = 0, stats_stream_bytes = 0;
    /* render-thread timing (us), summed over the 5 s stats window */
    uint64_t t_idle = 0, t_draw = 0, t_wait = 0, t_present = 0, t_total = 0, t_last_present = 0;
    static uint64_t now_us()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    }
    struct DrawTimer {
        uint64_t &sum, start;
        explicit DrawTimer(uint64_t &s) : sum(s), start(now_us()) {}
        ~DrawTimer() { sum += now_us() - start; }
    };

    uint32_t stream_serial = 0; /* bumped each time the ring enters a part */
    UINT stats_reused_bytes = 0;
    void next_stream_part()
    {
        stream_serial++;
        host_gl_stream_fence(stream_part);
        stream_part = (stream_part + 1) % STREAM_PARTS;
        host_gl_stream_wait(stream_part);
        stream_used = stream_part * STREAM_PART;
    }
    /* room for a whole draw's data in the current part; false if it can never fit */
    bool reserve(UINT bytes)
    {
        if (bytes > STREAM_PART) {
            static int warned;
            if (warned++ < 5) bo1_log("d3d9: a draw needs %u KB of streamed data; skipped", bytes >> 10);
            return false;
        }
        UINT at = (stream_used + 15) & ~15u;
        if (at + bytes > (stream_part + 1) * STREAM_PART) next_stream_part();
        return true;
    }
    UINT stream(const void *data, UINT bytes)
    {
        stats_stream_bytes += bytes;
        UINT at = (stream_used + 15) & ~15u;
        if (bytes) host_gl_stream_write(at, bytes, data);
        stream_used = at + bytes;
        return at;
    }

    /* a streamed buffer's bytes in the ring: the copy an earlier draw made, when this range is
       inside it, the data is unchanged and the copy is in the current part (an older part's
       fence was set before this draw, so its space may be rewritten) */
    template <typename B> UINT stream_cached(B *b, UINT from, UINT bytes)
    {
        if (b->cache_generation == b->generation && b->cache_serial == stream_serial && from >= b->cache_from &&
            from + bytes <= b->cache_from + b->cache_bytes && !((from - b->cache_from) & 3)) {
            stats_reused_bytes += bytes;
            return b->cache_at + (from - b->cache_from);
        }
        UINT at = stream(b->data + from, bytes);
        b->cache_generation = b->generation;
        b->cache_serial = stream_serial;
        b->cache_from = from;
        b->cache_bytes = bytes;
        b->cache_at = at;
        return at;
    }

    /* the bytes of stream s a streaming draw reads, clipped to the buffer */
    bool stream_slice(const VertexSource &source, UINT s, UINT *from, UINT *bytes) const
    {
        const Stream &st = streams[s];
        if (!st.buffer) return false;
        UINT size = st.buffer->desc.Size;
        *from = st.offset + source.first * st.stride;
        if (*from >= size) return false;
        *bytes = source.count * st.stride;
        if (*from + *bytes > size) *bytes = size - *from;
        return true;
    }

    /* the streams this draw reads (the declaration's, or stream 0 for fixed-function) */
    unsigned streams_used() const
    {
        if (fixed_function()) return 1;
        unsigned mask = 0;
        if (declaration)
            for (const D3DVERTEXELEMENT9 &e : declaration->elements) mask |= 1u << e.Stream;
        return mask;
    }

    bool any_streamed(unsigned mask) const
    {
        for (unsigned s = 0; s < 16; s++)
            if ((mask >> s & 1) && streams[s].buffer && streams[s].buffer->streamed) return true;
        return false;
    }

    UINT streamed_bytes(const VertexSource &source, unsigned mask) const
    {
        UINT total = 0, from, bytes;
        for (unsigned s = 0; s < 16; s++)
            if ((mask >> s & 1) && stream_slice(source, s, &from, &bytes)) total += bytes + 16;
        return total;
    }

    /* GL thread: binds GL_ARRAY_BUFFER for stream s and gives the attribute pointer */
    bool vertex_pointer(VertexSource &source, UINT s, UINT element_offset, UINT *stride, const void **pointer)
    {
        if (source.client) {
            glBindBuffer(GL_ARRAY_BUFFER, stream_buffer);
            *stride = source.client_stride;
            *pointer = (const void *)(uintptr_t)(source.client_at + element_offset);
            return true;
        }
        const Stream &st = streams[s];
        if (!st.buffer) return false;
        *stride = st.stride;
        if (source.streaming) {
            if (!source.uploaded[s]) {
                UINT from, bytes;
                if (!stream_slice(source, s, &from, &bytes)) return false;
                source.at[s] = stream_cached(st.buffer, from, bytes);
                source.uploaded[s] = true;
            }
            glBindBuffer(GL_ARRAY_BUFFER, stream_buffer);
            *pointer = (const void *)(uintptr_t)(source.at[s] + element_offset);
            return true;
        }
        st.buffer->bind(GL_ARRAY_BUFFER);
        *pointer = (const void *)(uintptr_t)(st.offset + element_offset + (INT)st.stride * source.base);
        return true;
    }

    /* ---- IDirect3DDevice9 ---- */

    HRESULT TestCooperativeLevel() override { return S_OK; }
    UINT GetAvailableTextureMem() override { return 1024u << 20; }
    HRESULT GetDeviceCaps(D3DCAPS9 *caps) override
    {
        bo1_d3d9_fill_caps(caps);
        return S_OK;
    }
    HRESULT GetSwapChain(UINT, IDirect3DSwapChain9 **swap_chain) override;
    HRESULT Reset(D3DPRESENT_PARAMETERS *p) override
    {
        drop_buffers();
        make_buffers(*p);
        return S_OK;
    }
    HRESULT Present(const RECT *, const RECT *, HWND, const RGNDATA *) override
    {
        if (!ensure_gl()) return S_OK;
        /* the back buffer as a texture, drawn over the whole window (Adreno ignores a
           glBlitFramebuffer from it to the window). Layered (multiview storage): a two-layer array,
           one layer an eye; a multiview frame hands both eyes over here, one after the other. */
        bool layered = back_buffer->layered();
        GLuint image = layered ? back_buffer->layered_texture() : back_buffer->texture_name;
        if (!layered && !image) {
            FramebufferKey key = {};
            key.colour[0] = back_buffer;
            bind_framebuffer(key); /* makes the texture */
            image = back_buffer->texture_name;
        }
        publish_pick();
        const bool both = g_bo1_render_eye == 2;
        uint64_t present_start = now_us();
        for (int eye = 0; eye < (both ? 2 : 1); eye++) {
            if (both) bo1_vr_present_eye(eye);
            unsigned int frame[3] = {0, 0, 0};
            uint64_t wait_start = now_us();
            unsigned int target = host_gl_frame_begin(frame);
            t_wait += now_us() - wait_start;
            if (target == 0xFFFFFFFFu) {
                /* the headset isn't showing us: nothing to draw into this frame */
                host_gl_present();
                gl_cache_forget();
                continue;
            }
            if (g_gpu_open || (g_debug_flags & (1u << 25))) gpu_pass("present to headset");
            g_last_draw_fbo = ~0u;
            glBindFramebuffer(GL_FRAMEBUFFER, target);
            glViewport(0, 0, (GLsizei)frame[0], (GLsizei)frame[1]);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_POLYGON_OFFSET_FILL);
            glColorMask(1, 1, 1, 1);
            MOJOSHADER_glBindProgram(NULL);
            glBindVertexArray(fixed_vao);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, gamma_texture);
            glBindSampler(1, 0);
            if (gamma_dirty) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, gamma);
                gamma_dirty = false;
            }
            glActiveTexture(GL_TEXTURE0);
            glBindSampler(0, 0);
            if (layered) {
                glUseProgram(present_program_array);
                glUniform1f(present_array_linear_out, frame[2] ? 1.0f : 0.0f);
                glUniform1f(present_array_keep_alpha, 0.0f);
                glUniform1f(present_layer, (float)eye);
                glBindTexture(GL_TEXTURE_2D_ARRAY, image);
            } else {
                glUseProgram(present_program);
                glUniform1f(present_linear_out, frame[2] ? 1.0f : 0.0f);
                glUniform1f(present_keep_alpha, 0.0f);
                glBindTexture(GL_TEXTURE_2D, image);
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
            /* the left eye's frame hands its 2D to the headset's panel */
            if ((g_bo1_render_eye == 0 || (both && eye == 0)) && ui_used && ui_surface && ui_surface->texture_name) {
                unsigned int panel[3] = {0, 0, 0};
                unsigned int panel_fbo = host_xr_panel_begin(panel);
                if (panel_fbo != 0xFFFFFFFFu) {
                    glBindFramebuffer(GL_FRAMEBUFFER, panel_fbo);
                    glViewport(0, 0, (GLsizei)panel[0], (GLsizei)panel[1]);
                    glUseProgram(present_program);
                    glUniform1f(present_linear_out, panel[2] ? 1.0f : 0.0f);
                    glUniform1f(present_keep_alpha, 1.0f);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, ui_surface->texture_name);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    host_xr_panel_end();
                    gl_cache_forget(); /* the host drew the pointer's dot */
                    glBindFramebuffer(GL_FRAMEBUFFER, target); /* the eye's image, for its own dot */
                }
            }
            if (eye == (both ? 1 : 0)) gpu_collect();
            glActiveTexture(GL_TEXTURE0);
            host_gl_present();
            gl_cache_forget(); /* the host drew too (the pointer's dot) */
        }
        ui_used = false;
        split_post = false;
        sampler_dirty[1] = true; /* unit 1 is the game's again next draw */
        sampler_dirty[0] = true; /* unit 0's texture and sampler are the game's again next draw */
        {
            uint64_t end = now_us();
            t_present += end - present_start;
            if (t_last_present) {
                t_total += end - t_last_present;
                /* hitch hunting: a frame far over the headset's budget, and whether the render thread was
                   busy (work) or waiting for the game (idle) */
                uint64_t gap = end - t_last_present;
                if (gap > 40000) {
                    uint64_t idle = g_bo1_backend_start_us > t_last_present ? g_bo1_backend_start_us - t_last_present : 0;
                    bo1_log("d3d9: slow frame %.1f ms (waiting for the game %.1f ms, drawing %.1f ms, %u draws so far)",
                            gap / 1000.0, idle / 1000.0, (gap - idle) / 1000.0, stats_draws);
                }
                if (g_bo1_backend_start_us > t_last_present) t_idle += g_bo1_backend_start_us - t_last_present;
            }
            t_last_present = end;
        }
        next_stream_part(); /* the next frame streams into the next part of the ring */
        if (g_debug_flags & 2) bo1_log("d3d9: %d framebuffer changes this frame", binds_this_frame);
        binds_this_frame = 0;
        memset(&last_bind_key, 0, sizeof last_bind_key);
        save_shot_if_asked();
        if (tracing) {
            bo1_log("d3d9 trace: end of frame, %u draws, eye %d", trace_draws, g_bo1_render_eye);
            trace_draws = 0;
            tracing = ++traced < 2; /* two frames in a row: both eyes */
        } else if ((g_debug_flags & 4) && !traced) {
            tracing = true; /* the next frames, once per run */
            trace_draws = 0;
        }
        /* frames per second, every 5 s */
        static unsigned frames, frames_then;
        static DWORD then;
        DWORD now = GetTickCount();
        if (++frames == 1) then = now;
        if (now - then >= 5000) {
            unsigned n = frames - frames_then;
            gpu_report(n);
            bo1_log("d3d9: per frame %u texture uploads, %u KB", g_upload_count / n, g_upload_bytes / n / 1024);
            g_upload_count = g_upload_bytes = 0;
            bo1_log("d3d9: per frame %u KB of streamed data reused; in 5 s %u vertex / %u pixel shaders compiled, %u programs linked (%u total; %u from the cache, %u compiled)",
                    stats_reused_bytes / n / 1024, g_vs_compiles, g_ps_compiles, g_links, (unsigned)programs.size(),
                    g_cache_hits, g_cache_misses);
            g_vs_compiles = g_ps_compiles = g_links = g_cache_hits = g_cache_misses = 0;
            stats_reused_bytes = 0;
            bo1_log("d3d9: frame %u, %.1f fps; per frame %u draws (%u streamed, %u KB); per frame us: total %u, "
                    "present %u (xr wait %u), idle %u, work %u",
                    frames, n * 1000.0 / (now - then), stats_draws / n, stats_streamed_draws / n, stats_stream_bytes / n / 1024,
                    (unsigned)(t_total / n), (unsigned)(t_present / n), (unsigned)(t_wait / n),
                    (unsigned)(t_idle / n), (unsigned)((t_total - t_present - t_idle) / n));
            stats_draws = stats_streamed_draws = stats_stream_bytes = 0;
            t_idle = t_draw = t_wait = t_present = t_total = 0;
            then = now;
            frames_then = frames;
        }
        return S_OK;
    }
    /* For testing: when <data folder>/shot_request exists, the back buffer is written to
       <data folder>/shot.tga (checked twice a second) */
    void save_shot_if_asked()
    {
        static unsigned count;
        if (++count % 36) return;
        char request[MAX_PATH], path[MAX_PATH];
        snprintf(request, sizeof request, "%s/d3d9_debug", bo1_data_root());
        if (FILE *d = fopen(request, "r")) {
            unsigned flags = 0;
            if (fscanf(d, "%u", &flags) != 1) flags = 0;
            fclose(d);
            if (flags != g_debug_flags) bo1_log("d3d9: debug flags %u", flags);
            g_debug_flags = flags;
        } else {
            g_debug_flags = 0;
        }
        snprintf(request, sizeof request, "%s/shot_request", bo1_data_root());
        if (access(request, F_OK)) return;
        unlink(request);
        if (g_debug_flags & (1u << 17)) {
            mid_shot_pending = true; /* debug: taken when the bloom starts (bind_current) instead */
            /* or at the framebuffer change numbered in <data>/shot_at, counting from the frame's first */
            mid_shot_at = -1;
            snprintf(request, sizeof request, "%s/shot_at", bo1_data_root());
            if (FILE *f = fopen(request, "r")) {
                if (fscanf(f, "%d", &mid_shot_at) != 1) mid_shot_at = -1;
                fclose(f);
            }
            return;
        }
        write_shot();
    }
    bool mid_shot_pending = false;
    int mid_shot_at = -1, binds_this_frame = 0;
    FramebufferKey last_bind_key = {};
    void write_shot()
    {
        /* a layered target (multiview storage): both layers, the same frame - shot.tga and shot1.tga */
        FramebufferKey probe = {};
        probe.colour[0] = (g_debug_flags & 32) && scene_target ? scene_target : back_buffer;
        int layers = probe.colour[0]->layered() ? 2 : 1;
        for (int layer = 0; layer < layers; layer++) write_shot_layer(layer);
    }
    void write_shot_layer(int layer)
    {
        char path[MAX_PATH];
        UINT w = params.BackBufferWidth, h = params.BackBufferHeight;
        std::vector<uint8_t> pixels((size_t)w * h * 4);
        FramebufferKey key = {};
        key.colour[0] = (g_debug_flags & 32) && scene_target ? scene_target : back_buffer;
        if (key.colour[0] != back_buffer) bo1_log("d3d9: shot of the scene target %p", (void *)scene_target);
        if (layer) key.mode = FB_LAYER1;
        bind_framebuffer(key, GL_READ_FRAMEBUFFER);
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        snprintf(path, sizeof path, "%s/%s", bo1_data_root(), layer ? "shot1.tga" : "shot.tga");
        FILE *f = fopen(path, "wb");
        if (!f) return;
        /* TGA, 32-bit, rows top first (descriptor bit 5): the back buffer's row 0 is the top */
        uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), 32, 0x28};
        fwrite(header, 1, sizeof header, f);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            /* through the gamma ramp, as on screen */
            uint8_t bgra[4] = {gamma[pixels[i + 2]][2], gamma[pixels[i + 1]][1], gamma[pixels[i]][0], 255};
            fwrite(bgra, 1, 4, f);
        }
        fclose(f);
        bo1_log("d3d9: back buffer saved to %s", path);
    }

    HRESULT GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **surface) override
    {
        back_buffer->AddRef();
        *surface = back_buffer;
        return S_OK;
    }
    void SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *ramp) override
    {
        for (int i = 0; i < 256; i++) {
            gamma[i][0] = (uint8_t)(ramp->red[i] >> 8);
            gamma[i][1] = (uint8_t)(ramp->green[i] >> 8);
            gamma[i][2] = (uint8_t)(ramp->blue[i] >> 8);
        }
        gamma_dirty = true;
    }

    HRESULT CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DTexture9 **texture,
                          HANDLE *) override
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
    HRESULT CreateVertexBuffer(UINT size, DWORD usage, DWORD fvf_, D3DPOOL pool, IDirect3DVertexBuffer9 **buffer,
                               HANDLE *) override
    {
        D3DVERTEXBUFFER_DESC d = {};
        d.Format = (D3DFORMAT)100;
        d.Type = D3DRTYPE_VERTEXBUFFER;
        d.Usage = usage;
        d.Pool = pool;
        d.Size = size;
        d.FVF = fvf_;
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
    HRESULT CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **surface,
                               HANDLE *) override
    {
        *surface = new Surface(Surface::RENDERBUFFER, w, h, f, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT);
        return S_OK;
    }
    HRESULT CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                      IDirect3DSurface9 **surface, HANDLE *) override
    {
        *surface = new Surface(Surface::RENDERBUFFER, w, h, f, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT);
        return S_OK;
    }
    HRESULT CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL pool, IDirect3DSurface9 **surface,
                                        HANDLE *) override
    {
        *surface = new Surface(Surface::MEMORY, w, h, f, 0, pool);
        return S_OK;
    }
    HRESULT CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **decl) override
    {
        *decl = new VertexDeclaration(elements);
        return S_OK;
    }
    HRESULT CreateVertexShader(const DWORD *code, IDirect3DVertexShader9 **shader) override
    {
        *shader = new VertexShader(code);
        return S_OK;
    }
    HRESULT CreatePixelShader(const DWORD *code, IDirect3DPixelShader9 **shader) override
    {
        *shader = new PixelShader(code);
        return S_OK;
    }
    HRESULT CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **query) override
    {
        if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION) return D3DERR_NOTAVAILABLE;
        if (query) *query = new Query(type);
        return S_OK;
    }

    HRESULT UpdateSurface(IDirect3DSurface9 *source, const RECT *, IDirect3DSurface9 *dest, const POINT *) override
    {
        /* whole-surface copies from memory into a texture level */
        Surface *s = (Surface *)source, *d = (Surface *)dest;
        D3DLOCKED_RECT from, to;
        if (FAILED(s->LockRect(&from, NULL, D3DLOCK_READONLY)) || FAILED(d->LockRect(&to, NULL, 0))) return D3DERR_INVALIDCALL;
        UINT rows = rows_of(d->format, d->desc.Height), bytes = (UINT)(from.Pitch < to.Pitch ? from.Pitch : to.Pitch);
        for (UINT y = 0; y < rows; y++) memcpy((uint8_t *)to.pBits + y * to.Pitch, (uint8_t *)from.pBits + y * from.Pitch, bytes);
        return S_OK;
    }
    HRESULT UpdateTexture(IDirect3DBaseTexture9 *source, IDirect3DBaseTexture9 *dest) override
    {
        TextureData *s = texture_data(source), *d = texture_data(dest);
        if (!s || !d) return D3DERR_INVALIDCALL;
        UINT skip = s->levels > d->levels ? s->levels - d->levels : 0;
        for (UINT face = 0; face < d->faces && face < s->faces; face++)
            for (UINT level = 0; level < d->levels && level + skip < s->levels; level++) {
                uint8_t *from = s->images[face * s->levels + level + skip].bits;
                if (!from) continue;
                uint8_t *to = d->memory(face, level);
                if (to) memcpy(to, from, d->image_bytes(level));
                d->mark_dirty(face, level);
            }
        return S_OK;
    }
    HRESULT GetRenderTargetData(IDirect3DSurface9 *render_target, IDirect3DSurface9 *dest) override
    {
        Surface *rt = (Surface *)render_target, *d = (Surface *)dest;
        if (!ensure_gl()) return S_OK;
        FramebufferKey key = {};
        key.colour[0] = rt;
        bind_framebuffer(key, GL_READ_FRAMEBUFFER);
        D3DLOCKED_RECT to;
        if (FAILED(d->LockRect(&to, NULL, 0))) return D3DERR_INVALIDCALL;
        UINT w = d->desc.Width, h = d->desc.Height;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (UINT y = 0; y < h; y++)
            for (UINT x = 0; x < w; x++) {
                const uint8_t *p = &rgba[((size_t)y * w + x) * 4];
                uint8_t *o = (uint8_t *)to.pBits + (size_t)y * to.Pitch + x * 4;
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
                o[3] = p[3];
            }
        return S_OK;
    }
    HRESULT StretchRect(IDirect3DSurface9 *source, const RECT *source_rect, IDirect3DSurface9 *dest, const RECT *dest_rect,
                        D3DTEXTUREFILTERTYPE filter) override
    {
        if (!ensure_gl()) return S_OK;
        Surface *s = (Surface *)source, *d = (Surface *)dest;
        bool is_depth = s->is_depth();
        FramebufferKey from = {}, to = {};
        if (is_depth) {
            from.depth = s;
            to.depth = d;
        } else {
            from.colour[0] = s;
            to.colour[0] = d;
        }
        RECT sr = source_rect ? *source_rect : RECT{0, 0, (LONG)s->desc.Width, (LONG)s->desc.Height};
        RECT dr = dest_rect ? *dest_rect : RECT{0, 0, (LONG)d->desc.Width, (LONG)d->desc.Height};
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        glDepthMask(GL_TRUE);
        GLenum earlier = tracing ? glGetError() : 0; /* debug: errors from before the blit */
        if (tracing && earlier) bo1_log("d3d9 trace   GL error %#x pending before StretchRect", earlier);
        /* multiview: eye targets copy each eye's layer. Whole, same-size, same-format colour copies
           go texture to texture (glCopyImageSubData), layer by layer: through framebuffers both layers
           landed in the destination's first (the right eye's picture in the left eye's layer, nothing
           in the right's - a ghost through glass on the left, black flashes on the right). */
        if (g_bo1_render_eye == 2 && s->layered() && d->layered() && !is_depth && s->desc.Format == d->desc.Format &&
            sr.left == 0 && sr.top == 0 && dr.left == 0 && dr.top == 0 && sr.right == dr.right && sr.bottom == dr.bottom &&
            (UINT)sr.right <= s->desc.Width && (UINT)sr.bottom <= s->desc.Height && (UINT)dr.right <= d->desc.Width &&
            (UINT)dr.bottom <= d->desc.Height) {
            GLuint from_name = s->layered_texture(), to_name = d->layered_texture();
            GLint from_level = s->texture ? (GLint)s->level : 0, to_level = d->texture ? (GLint)d->level : 0;
            for (int layer = 0; layer < 2; layer++)
                glCopyImageSubData(from_name, GL_TEXTURE_2D_ARRAY, from_level, 0, 0, layer, to_name, GL_TEXTURE_2D_ARRAY,
                                   to_level, 0, 0, layer, sr.right, sr.bottom, 1);
            return S_OK;
        }
        int layers = g_bo1_render_eye == 2 && s->layered() && d->layered() ? 2 : 1;
        for (int layer = 0; layer < layers; layer++) {
            from.mode = to.mode = layer ? FB_LAYER1 : FB_LAYER0;
            bind_framebuffer(from, GL_READ_FRAMEBUFFER);
            bind_framebuffer(to, GL_DRAW_FRAMEBUFFER);
            glBlitFramebuffer(sr.left, sr.top, sr.right, sr.bottom, dr.left, dr.top, dr.right, dr.bottom,
                              is_depth ? GL_DEPTH_BUFFER_BIT | (s->format.stencil ? GL_STENCIL_BUFFER_BIT : 0) : GL_COLOR_BUFFER_BIT,
                              is_depth || filter == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR);
            static int logged;
            if (layers == 2 && logged < 8) {
                logged++;
                bo1_log("d3d9: multiview StretchRect layer %d: read %#x draw %#x error %#x (%ux%u f%u -> %ux%u f%u)", layer,
                        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER), glGetError(),
                        s->desc.Width, s->desc.Height, (unsigned)s->desc.Format, d->desc.Width, d->desc.Height,
                        (unsigned)d->desc.Format);
            }
        }
        if (tracing) {
            GLenum error = glGetError();
            bo1_log("d3d9 trace   StretchRect %p %ux%u f%u kind %d -> %p %ux%u f%u kind %d (%ld,%ld,%ld,%ld -> %ld,%ld,%ld,%ld) "
                    "read %#x draw %#x error %#x",
                    (void *)s, s->desc.Width, s->desc.Height, (unsigned)s->desc.Format, (int)s->kind, (void *)d, d->desc.Width,
                    d->desc.Height, (unsigned)d->desc.Format, (int)d->kind, (long)sr.left, (long)sr.top, (long)sr.right,
                    (long)sr.bottom, (long)dr.left, (long)dr.top, (long)dr.right, (long)dr.bottom,
                    glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER), error);
        }
        return S_OK;
    }
    HRESULT SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) override
    {
        if (index >= 4) return D3DERR_INVALIDCALL;
        set_target((int)index, (Surface *)surface);
        if (index == 0 && surface) {
            /* D3D9 resets the viewport (and scissor) to the new target */
            viewport = {0, 0, ((Surface *)surface)->desc.Width, ((Surface *)surface)->desc.Height, 0.0f, 1.0f};
            scissor = {0, 0, (LONG)viewport.Width, (LONG)viewport.Height};
        }
        return S_OK;
    }
    HRESULT SetDepthStencilSurface(IDirect3DSurface9 *surface) override
    {
        set_depth((Surface *)surface);
        return S_OK;
    }
    HRESULT BeginScene() override { return S_OK; }
    HRESULT EndScene() override { return S_OK; }
    HRESULT Clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour, float z, DWORD stencil) override
    {
        if (!ensure_gl()) return S_OK;
        bind_current();
        GLbitfield mask = 0;
        if (flags & D3DCLEAR_TARGET) {
            glColorMask(1, 1, 1, 1);
            glClearColor((colour >> 16 & 255) / 255.0f, (colour >> 8 & 255) / 255.0f, (colour & 255) / 255.0f,
                         (colour >> 24) / 255.0f);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if ((flags & D3DCLEAR_ZBUFFER) && depth) {
            glDepthMask(GL_TRUE);
            glClearDepthf(z);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        if ((flags & D3DCLEAR_STENCIL) && depth && depth->format.stencil) {
            glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
            glClearStencil((GLint)stencil);
            mask |= GL_STENCIL_BUFFER_BIT;
        }
        if (!mask) return S_OK;
        /* D3D9 clears the viewport (and the scissor rectangle when the test is on) */
        glEnable(GL_SCISSOR_TEST);
        RECT area = {(LONG)viewport.X, (LONG)viewport.Y, (LONG)(viewport.X + viewport.Width), (LONG)(viewport.Y + viewport.Height)};
        if (render_states[D3DRS_SCISSORTESTENABLE]) {
            if (scissor.left > area.left) area.left = scissor.left;
            if (scissor.top > area.top) area.top = scissor.top;
            if (scissor.right < area.right) area.right = scissor.right;
            if (scissor.bottom < area.bottom) area.bottom = scissor.bottom;
        }
        /* multiview: the Adreno driver clears only the first view of a multiview framebuffer, so each
           layer is cleared through its own */
        FramebufferKey key = current_key();
        int layers = key.mode == FB_MULTIVIEW ? 2 : 1;
        for (int layer = 0; layer < layers; layer++) {
            if (layers == 2) {
                key.mode = layer ? FB_LAYER1 : FB_LAYER0;
                bind_framebuffer(key);
            }
            if (!count || !rects) {
                glScissor(area.left, area.top, area.right - area.left, area.bottom - area.top);
                glClear(mask);
            } else {
                for (DWORD i = 0; i < count; i++) {
                    LONG l = rects[i].x1 > area.left ? rects[i].x1 : area.left, t = rects[i].y1 > area.top ? rects[i].y1 : area.top;
                    LONG r = rects[i].x2 < area.right ? rects[i].x2 : area.right, b = rects[i].y2 < area.bottom ? rects[i].y2 : area.bottom;
                    if (r <= l || b <= t) continue;
                    glScissor(l, t, r - l, b - t);
                    glClear(mask);
                }
            }
        }
        if (layers == 2) bind_current(); /* the multiview framebuffer again for the draws */
        return S_OK;
    }
    HRESULT SetViewport(const D3DVIEWPORT9 *v) override
    {
        viewport = *v;
        return S_OK;
    }
    HRESULT SetScissorRect(const RECT *rect) override
    {
        scissor = *rect;
        return S_OK;
    }
    HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override
    {
        if ((UINT)state < 256) render_states[state] = value;
        return S_OK;
    }
    static int unit_of(DWORD stage)
    {
        if (stage < 16) return (int)stage;
        if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage < D3DVERTEXTEXTURESAMPLER0 + 4) return 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
        return -1;
    }
    HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture) override
    {
        int unit = unit_of(stage);
        if (unit >= 0) {
            hold(textures[unit], texture);
            unit_data[unit] = texture_data(texture);
        }
        return S_OK;
    }
    HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override
    {
        if (stage < 8 && (UINT)type < 8) stage_states[stage][type] = value;
        return S_OK;
    }
    HRESULT SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override
    {
        int unit = unit_of(sampler);
        if (unit >= 0 && (UINT)type < 14 && sampler_states[unit][type] != value) {
            sampler_states[unit][type] = value;
            sampler_dirty[unit] = true;
        }
        return S_OK;
    }
    HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl) override
    {
        hold(declaration, (VertexDeclaration *)decl);
        variant_last = NULL; /* the cached variant was for what was set before */
        return S_OK;
    }
    HRESULT SetFVF(DWORD value) override
    {
        fvf = value;
        return S_OK;
    }
    HRESULT SetVertexShader(IDirect3DVertexShader9 *shader) override
    {
        hold(vertex_shader, (VertexShader *)shader);
        variant_last = NULL; /* the cached variant was for what was set before */
        return S_OK;
    }
    HRESULT SetVertexShaderConstantF(UINT start, const float *data, UINT count) override
    {
        if (start >= 256) return D3DERR_INVALIDCALL;
        if (start + count > 256) count = 256 - start;
        memcpy(vs_constants[start], data, (size_t)count * 16);
        memcpy(vs_constants_eye1[start], data, (size_t)count * 16);
        if (start < vs_dirty_from) vs_dirty_from = start;
        if (start + count > vs_dirty_to) vs_dirty_to = start + count;
        return S_OK;
    }
    HRESULT SetPixelShader(IDirect3DPixelShader9 *shader) override
    {
        hold(pixel_shader, (PixelShader *)shader);
        return S_OK;
    }
    HRESULT SetPixelShaderConstantF(UINT start, const float *data, UINT count) override
    {
        if (start >= 224) return D3DERR_INVALIDCALL;
        if (start + count > 224) count = 224 - start;
        memcpy(ps_constants[start], data, (size_t)count * 16);
        memcpy(ps_constants_eye1[start], data, (size_t)count * 16);
        if (start < ps_dirty_from) ps_dirty_from = start;
        if (start + count > ps_dirty_to) ps_dirty_to = start + count;
        return S_OK;
    }
    HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) override
    {
        if (stream >= 16) return D3DERR_INVALIDCALL;
        hold(streams[stream].buffer, (VertexBuffer *)buffer);
        streams[stream].offset = offset;
        streams[stream].stride = stride;
        return S_OK;
    }
    HRESULT SetIndices(IDirect3DIndexBuffer9 *buffer) override
    {
        hold(indices, (IndexBuffer *)buffer);
        return S_OK;
    }

    /* debug flag 4: log one frame's draws (render target, shader, blend, depth, textures) */
    unsigned trace_draws = 0;
    bool tracing = false;
    int traced = 0;
    void trace_draw(bool fixed)
    {
        if (trace_draws >= 12000) return;
        const DWORD *rs = render_states;
        char line[512];
        int n = snprintf(line, sizeof line, "d3d9 trace %u: %s vs %p ps %p rt %ux%u f%u depth %s blend %lu %lu/%lu cw %lx z %lu/%lu/%lu",
                         trace_draws++, fixed ? "fixed" : "shader", (void *)vertex_shader, (void *)pixel_shader,
                         targets[0] ? targets[0]->desc.Width : 0, targets[0] ? targets[0]->desc.Height : 0,
                         targets[0] ? (unsigned)targets[0]->desc.Format : 0, depth ? "yes" : "no",
                         (unsigned long)rs[D3DRS_ALPHABLENDENABLE], (unsigned long)rs[D3DRS_SRCBLEND],
                         (unsigned long)rs[D3DRS_DESTBLEND], (unsigned long)rs[D3DRS_COLORWRITEENABLE],
                         (unsigned long)rs[D3DRS_ZENABLE], (unsigned long)rs[D3DRS_ZFUNC], (unsigned long)rs[D3DRS_ZWRITEENABLE]);
        n += snprintf(line + n, sizeof line - n, " mode %u rtL %d dL %d ui %d", current_key().mode,
                      targets[0] ? (int)targets[0]->layered() : -1, depth ? (int)depth->layered() : -1, ui_target() ? 1 : 0);
        n += snprintf(line + n, sizeof line - n, " vp %lu,%lu %lux%lu %.2f-%.2f sc %lu %ld,%ld,%ld,%ld cull %lu st %lu %lu ref %lu clip %lx",
                      (unsigned long)viewport.X, (unsigned long)viewport.Y, (unsigned long)viewport.Width, (unsigned long)viewport.Height,
                      viewport.MinZ, viewport.MaxZ, (unsigned long)rs[D3DRS_SCISSORTESTENABLE], (long)scissor.left, (long)scissor.top,
                      (long)scissor.right, (long)scissor.bottom, (unsigned long)rs[D3DRS_CULLMODE], (unsigned long)rs[D3DRS_STENCILENABLE],
                      (unsigned long)rs[D3DRS_STENCILFUNC], (unsigned long)rs[D3DRS_STENCILREF], (unsigned long)rs[152 /* CLIPPLANEENABLE */]);
        for (int unit = 0; unit < MAX_SAMPLERS && n < (int)sizeof line - 40; unit++) {
            TextureData *t = texture_data(textures[unit]);
            if (t) n += snprintf(line + n, sizeof line - n, " t%d:%ux%u f%u", unit, t->width, t->height, (unsigned)t->d3d_format);
        }
        bo1_log("%s", line);
        if (!fixed && pixel_shader && pixel_shader->compiled) dump_shaders();
    }

    /* while tracing: each shader's GLSL once, to <data>/shaders/, and the constants it was drawn with */
    std::set<const void *> dumped;
    void dump_glsl(const void *key, MOJOSHADER_glShader *shader, const char *kind)
    {
        if (!shader || !dumped.insert(key).second) return;
        const MOJOSHADER_parseData *pd = MOJOSHADER_glGetShaderParseData(shader);
        if (!pd || !pd->output) return;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s/shaders", bo1_data_root());
        mkdir(path, 0777);
        snprintf(path, sizeof path, "%s/shaders/%s_%p.glsl", bo1_data_root(), kind, key);
        if (FILE *f = fopen(path, "wb")) {
            fwrite(pd->output, 1, (size_t)pd->output_len, f);
            fclose(f);
        }
    }
    void dump_shaders()
    {
        bool fresh = !dumped.count(pixel_shader);
        dump_glsl(pixel_shader, pixel_shader->compiled, "ps");
        dump_glsl(vertex_shader, vertex_variant(), "vs");
        if (!fresh) return;
        for (int r = 0; r < 224; r++) {
            const float *c = ps_constants[r];
            if (c[0] || c[1] || c[2] || c[3]) bo1_log("d3d9 trace   ps %p c%d = %g %g %g %g", (void *)pixel_shader, r, c[0], c[1], c[2], c[3]);
        }
        for (int r = 0; r < 256; r++) {
            const float *c = vs_constants[r];
            if (c[0] || c[1] || c[2] || c[3]) bo1_log("d3d9 trace   vs %p c%d = %g %g %g %g", (void *)vertex_shader, r, c[0], c[1], c[2], c[3]);
        }
    }

    /* Device::pick: the draw just prepared samples the pointed-at texture. It is drawn again for the
       one pointed-at pixel only, into a 1x1 float target, three times with that texture swapped for
       (a) one whose texels hold their own coordinates, (b) white and (c) black. A material's output
       is (nearly always) affine in what it samples, out = k * t + s per channel, so (a - c) / (b - c)
       is the texture coordinate the pixel sampled, whatever lighting, tint or glow the material adds. */
    GLuint pick_fbo = 0, pick_target = 0, pick_textures[3] = {};
    bool pick_found = false;
    float pick_u = 0, pick_v = 0;
    void make_pick_resources()
    {
        glGenTextures(1, &pick_target);
        glBindTexture(GL_TEXTURE_2D, pick_target);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 1, 1);
        glGenFramebuffers(1, &pick_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pick_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pick_target, 0);
        GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) bo1_log("d3d9: pick framebuffer incomplete (%#x)", status);
        glGenTextures(3, pick_textures);
        const int N = 256;
        std::vector<uint16_t> coords((size_t)N * N * 4);
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                uint16_t *t = &coords[((size_t)y * N + x) * 4];
                t[0] = to_half((x + 0.5f) / N);
                t[1] = to_half((y + 0.5f) / N);
                t[2] = t[3] = to_half(1.0f);
            }
        const uint16_t white[4] = {to_half(1.0f), to_half(1.0f), to_half(1.0f), to_half(1.0f)};
        const uint16_t black[4] = {0, 0, 0, to_half(1.0f)};
        for (int i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, pick_textures[i]);
            int n = i == 0 ? N : 1;
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, n, n);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGBA, GL_HALF_FLOAT, i == 0 ? coords.data() : i == 1 ? white : black);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
    }
    template <class Draw> void pick(Draw &&draw)
    {
        /* in stereo the pointer is a pixel of the left eye's picture (quest_vr.cpp: g_bo1_render_eye) */
        if (g_pick_x < 0 || !g_pick_texture || !targets[0] || fixed_function() || g_bo1_render_eye > 0) return;
        int unit = -1;
        for (int i = 0; i < MAX_SAMPLERS && unit < 0; i++)
            if ((void *)textures[i] == g_pick_texture) unit = i;
        if (unit < 0) return;
        /* the request is in back-buffer pixels; the draw's target may be another full-screen size */
        int x = (int)((long long)g_pick_x * targets[0]->desc.Width / params.BackBufferWidth);
        int y = (int)((long long)g_pick_y * targets[0]->desc.Height / params.BackBufferHeight);
        if (x < (int)viewport.X || y < (int)viewport.Y || x >= (int)(viewport.X + viewport.Width) ||
            y >= (int)(viewport.Y + viewport.Height))
            return;
        if (!pick_fbo) make_pick_resources();
        /* rows run top-down in every render target here, so (x, y) is the pixel; the viewport moves
           so that it lands on the 1x1 target's only pixel */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pick_fbo);
        glViewport((GLint)viewport.X - x, (GLint)viewport.Y - y, (GLsizei)viewport.Width, (GLsizei)viewport.Height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glColorMask(1, 1, 1, 1);
        float out[3][4];
        for (int i = 0; i < 3; i++) {
            const float clear[4] = {-1000.0f, -1000.0f, -1000.0f, -1000.0f};
            glClearBufferfv(GL_COLOR, 0, clear);
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, pick_textures[i]);
            draw();
            glBindFramebuffer(GL_READ_FRAMEBUFFER, pick_fbo);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, out[i]);
        }
        apply_sampler(unit);               /* the real texture back */
        apply_render_states(false);        /* and the draw's own target and states */
        if (out[2][0] <= -999.0f) return;  /* the draw doesn't cover the pixel */
        float dr = out[1][0] - out[2][0], dg = out[1][1] - out[2][1];
        if (fabsf(dr) < 1e-3f || fabsf(dg) < 1e-3f) return;
        pick_u = (out[0][0] - out[2][0]) / dr;
        pick_v = (out[0][1] - out[2][1]) / dg;
        pick_found = true;
    }
    void publish_pick()
    {
        /* the pick is made in the left eye's (or a flat) frame: a request that arrives after that eye was
           drawn waits through the right eye's frame instead of being answered "nothing" there */
        if (g_pick_x < 0 || g_bo1_render_eye == 1) return;
        g_pick_u = pick_u;
        g_pick_v = pick_v;
        g_pick_valid = pick_found;
        g_pick_serial = g_pick_serial + 1;
        pick_found = false;
        g_pick_x = -1; /* asked again for the next frame if still wanted */
    }

    Surface *scene_target = NULL; /* debug flag 32: the shot shows the last depth-tested full-size target */
    /* debug (d3d9_debug bit 3): additive draws (dest blend ONE) left out, each shader pair logged once */
    bool debug_skip()
    {
        if ((g_debug_flags & 128) && last_layered && mv_draw && !fixed_function()) {
            /* debug (bit 7): draws reading a layered (per-eye) texture left out */
            static std::set<std::pair<const void *, const void *>> seen;
            if (seen.insert({vertex_shader, pixel_shader}).second)
                bo1_log("DEBUG layered-reading draw left out: vs %p ps %p units %#x blend %lu %lu/%lu", (void *)vertex_shader,
                        (void *)pixel_shader, last_layered, (unsigned long)render_states[D3DRS_ALPHABLENDENABLE],
                        (unsigned long)render_states[D3DRS_SRCBLEND], (unsigned long)render_states[D3DRS_DESTBLEND]);
            return true;
        }
        if (!(g_debug_flags & 8) || !render_states[D3DRS_ALPHABLENDENABLE] || render_states[D3DRS_DESTBLEND] != D3DBLEND_ONE)
            return false;
        static std::set<std::pair<const void *, const void *>> seen;
        if (seen.insert({vertex_shader, pixel_shader}).second)
            bo1_log("DEBUG additive draw left out: vs %p ps %p src %lu z %lu/%lu mv %d", (void *)vertex_shader, (void *)pixel_shader,
                    (unsigned long)render_states[D3DRS_SRCBLEND], (unsigned long)render_states[D3DRS_ZENABLE],
                    (unsigned long)render_states[D3DRS_ZWRITEENABLE], mv_draw ? 1 : 0);
        return true;
    }

    /* a draw, prepared and issued; in a split post-processing frame once per eye layer */
    template <class Issue> void draw_split(VertexSource &source, Issue &&issue)
    {
        bool split = split_post && g_bo1_render_eye == 2 && current_key().mode == FB_MULTIVIEW;
        if (!split) {
            if (prepare(source)) issue();
            return;
        }
        for (int layer = 0; layer < 2; layer++) {
            force_layer = layer;
            if (prepare(source)) issue();
        }
        force_layer = -1;
        /* the left eye's vertex constants back for what follows */
        MOJOSHADER_glSetVertexShaderUniformF(0, vs_constants[0], 256);
    }

    bool prepare(VertexSource &source)
    {
        if (tracing) trace_draw(fixed_function());
        if (render_states[D3DRS_ZENABLE] && targets[0] && targets[0] != back_buffer &&
            targets[0]->desc.Width == params.BackBufferWidth && targets[0]->desc.Format == D3DFMT_A8R8G8B8)
            scene_target = targets[0];
        stats_draws++;
        if (g_gpu_open && g_pass_draw_counter) ++*g_pass_draw_counter;
        if (source.streaming || source.client) stats_streamed_draws++;
        bool fixed = fixed_function();
        apply_render_states(fixed);
        bool ok = fixed ? prepare_fixed_draw(source) : prepare_shader_draw(source);
        if (g_bo1_render_eye == 2 && (g_debug_flags & (1u << 28))) {
            /* debug: GL errors in multiview frames */
            static int logged;
            GLenum e = glGetError();
            if (e && logged++ < 30)
                bo1_log("DEBUG GL error %#x at a %s draw, target %ux%u fmt %u, mv %d", e, fixed ? "fixed" : "shader",
                        targets[0] ? targets[0]->desc.Width : 0, targets[0] ? targets[0]->desc.Height : 0,
                        targets[0] ? (unsigned)targets[0]->desc.Format : 0, mv_draw ? 1 : 0);
        }
        if (tracing && !ok) bo1_log("d3d9 trace   skipped (vs %p ps %p decl %p)", (void *)vertex_shader, (void *)pixel_shader, (void *)declaration);
        return ok;
    }

    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override
    {
        if (!ensure_gl()) return S_OK;
        GLsizei n;
        GLenum mode = gl_primitive(type, count, &n);
        VertexSource source;
        unsigned used = streams_used();
        if (any_streamed(used)) {
            source.streaming = true;
            source.first = start;
            source.count = (UINT)n;
            if (!reserve(streamed_bytes(source, used))) return S_OK;
        }
        GLint first = source.streaming ? 0 : (GLint)start;
        auto draw = [&] { if (!(g_debug_flags & (1u << 24)) && !debug_skip()) glDrawArrays(mode, first, n); };
        draw_split(source, [&] {
            pick(draw);
            draw();
        });
        return S_OK;
    }
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base_vertex, UINT min_index, UINT vertex_count, UINT start_index,
                                 UINT count) override
    {
        if (!indices || !ensure_gl()) return S_OK;
        GLsizei n;
        GLenum mode = gl_primitive(type, count, &n);
        bool wide = indices->desc.Format == D3DFMT_INDEX32;
        UINT index_size = wide ? 4 : 2, index_from = start_index * index_size, index_bytes = (UINT)n * index_size;
        if (index_from + index_bytes > indices->desc.Size) return S_OK;

        /* streamed vertices: D3D says the indices reach [min_index, min_index + vertex_count) past
           base_vertex; those vertices are copied as 0.., so GL's base vertex becomes -min_index */
        VertexSource source;
        unsigned used = streams_used();
        GLint gl_base = base_vertex;
        if (any_streamed(used) && vertex_count) {
            source.streaming = true;
            source.first = (UINT)(base_vertex + (INT)min_index);
            source.count = vertex_count;
            gl_base = -(GLint)min_index;
        } else if ((g_debug_flags & (1u << 14)) && gl_base > 0) {
            /* debug: the base vertex on the attribute pointers, not the draw */
            source.base = gl_base;
            gl_base = 0;
        }
        if (!reserve((source.streaming ? streamed_bytes(source, used) : 0) + (indices->streamed ? index_bytes + 16 : 0)))
            return S_OK;
        draw_split(source, [&] {
            const void *offset;
            if (indices->streamed) {
                UINT at = stream_cached(indices, index_from, index_bytes);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_buffer);
                offset = (const void *)(uintptr_t)at;
            } else {
                indices->bind(GL_ELEMENT_ARRAY_BUFFER);
                offset = (const void *)(uintptr_t)index_from;
            }
            auto draw = [&] {
                if ((g_debug_flags & (1u << 24)) || debug_skip()) return;
                glDrawElementsBaseVertex(mode, n, wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, offset, gl_base);
            };
            pick(draw);
            draw();
        });
        return S_OK;
    }
    /* RESZ: the scene's depth buffer copied into the INTZ texture */
    void resolve_depth_into(TextureData *t)
    {
        if (!depth) return;
        if (depth->samples > 1 || depth->desc.Width != t->width || depth->desc.Height != t->height) {
            static int warned;
            if (warned++ < 3)
                bo1_log("d3d9: RESZ from a %ux%u depth buffer (%u samples) into a %ux%u INTZ texture: skipped",
                        depth->desc.Width, depth->desc.Height, depth->samples, t->width, t->height);
            return;
        }
        t->realize();
        static GLuint to;
        static GLuint to_texture;
        static int to_layer = -1;
        if (!to) glGenFramebuffers(1, &to);
        glDisable(GL_SCISSOR_TEST);
        /* multiview: each eye's layer (a layered INTZ texture takes a layered depth buffer's) */
        if (g_bo1_render_eye == 2 && t->layers == 2 && depth->layered() && !depth->depth_only_array) {
            /* each eye's depth straight into its layer (through framebuffers both landed in the first:
               the right eye's soft effects - muzzle flashes, explosions - faded out against no depth) */
            GLuint from = depth->layered_texture();
            for (int layer = 0; layer < 2; layer++)
                glCopyImageSubData(from, GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, t->name, GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer,
                                   (GLsizei)t->width, (GLsizei)t->height, 1);
            return;
        }
        int layers = g_bo1_render_eye == 2 && t->layers == 2 && depth->layered() ? 2 : 1;
        for (int layer = 0; layer < layers; layer++) {
            FramebufferKey from = {};
            from.depth = depth;
            from.mode = layer ? FB_LAYER1 : FB_LAYER0;
            bind_framebuffer(from, GL_READ_FRAMEBUFFER);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
            if (to_texture != t->name || to_layer != layer) {
                if (t->layers == 2) glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, t->name, 0, layer);
                else glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, t->name, 0);
                to_texture = t->name;
                to_layer = layer;
            }
            glBlitFramebuffer(0, 0, (GLint)t->width, (GLint)t->height, 0, 0, (GLint)t->width, (GLint)t->height,
                              GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
        static int checked;
        if (checked++ < 2) {
            GLenum e = glGetError();
            bo1_log("d3d9: RESZ depth copy %ux%u, GL error 0x%x", t->width, t->height, e);
        }
    }

    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void *vertices, UINT stride) override
    {
        if (!ensure_gl()) return S_OK;
        if (type == D3DPT_POINTLIST) {
            TextureData *t0 = texture_data(textures[0]);
            if (t0 && (DWORD)t0->d3d_format == FMT_INTZ) {
                if (render_states[D3DRS_POINTSIZE] == RESZ_CODE) resolve_depth_into(t0);
                return S_OK; /* the RESZ points themselves draw nothing */
            }
        }
        GLsizei n;
        GLenum mode = gl_primitive(type, count, &n);
        UINT bytes = (UINT)n * stride;
        if (!reserve(bytes + 16)) return S_OK;
        VertexSource source;
        source.client = (const uint8_t *)vertices;
        source.client_stride = stride;
        source.client_at = stream(vertices, bytes);
        {
            auto draw = [&] { if (!debug_skip()) glDrawArrays(mode, 0, n); };
            draw_split(source, [&] {
                pick(draw);
                draw();
            });
        }
        return S_OK;
    }
};

struct SwapChain final : Counted<IDirect3DSwapChain9> {
    Device *device;
    explicit SwapChain(Device *d) : device(d) {}
    HRESULT Present(const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD) override { return device->Present(a, b, w, r); }
    HRESULT GetFrontBufferData(IDirect3DSurface9 *) override { return S_OK; }
    HRESULT GetBackBuffer(UINT, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surface) override
    {
        return device->GetBackBuffer(0, 0, type, surface);
    }
};

/* the back end, around its 2D commands in a stereo frame (rb_backend.cpp): 1 sends them to the panel */
extern "C" void bo1_d3d9_ui(int on)
{
    if (g_device_for_ui) g_device_for_ui->ui_redirect(on != 0);
}

/* within the 2D: 1 sends what follows to the eye's picture instead (the 3D crosshair), 0 back to the panel */
extern "C" void bo1_d3d9_ui_pause(int paused)
{
    Device *d = g_device_for_ui;
    if (!d) return;
    d->ui_paused = paused != 0;
    g_bo1_ui_redirect = d->ui_on && !d->ui_paused;
}

/* multiview: post-processing starts (rb_postfx.cpp); its draws go a layer at a time to the frame's end */
extern "C" void bo1_d3d9_split_post(void)
{
    if (g_device_for_ui && g_bo1_render_eye == 2) g_device_for_ui->split_post = true;
}

/* multiview (quest_multiview.cpp): the right eye's values of constants just set for both */
extern "C" void bo1_d3d9_eye1_constants(int pixel, unsigned int dest, const float *data, unsigned int rows)
{
    Device *d = g_device_for_ui;
    if (!d) return;
    unsigned limit = pixel ? 224 : 256;
    if (dest >= limit) return;
    if (dest + rows > limit) rows = limit - dest;
    memcpy(pixel ? d->ps_constants_eye1[dest] : d->vs_constants_eye1[dest], data, (size_t)rows * 16);
}

/* multiview storage: a render texture each eye fills with its own picture (by texture: the INTZ
   depth copy has no surface the engine keeps) */
extern "C" void bo1_d3d9_stereo_surface(IDirect3DSurface9 *surface);
extern "C" void bo1_d3d9_stereo_texture(IDirect3DBaseTexture9 *texture)
{
    TextureData *t = texture_data(texture);
    if (!t || t->layers == 2 || !t->render_target()) return;
    if (t->target != GL_TEXTURE_2D || t->name || t->faces != 1 || t->levels != 1) {
        bo1_log("d3d9: multiview: can't make a %ux%u texture (fmt %u) layered", t->width, t->height, (unsigned)t->d3d_format);
        return;
    }
    t->layers = 2;
    t->target = GL_TEXTURE_2D_ARRAY;
    bo1_log("d3d9: multiview: %ux%u texture fmt %u is layered", t->width, t->height, (unsigned)t->d3d_format);
}

/* multiview storage (r_rendertarget.cpp, before anything is drawn): a target each eye draws its own
   picture into becomes a two-layer texture array, one layer an eye */
extern "C" void bo1_d3d9_stereo_surface(IDirect3DSurface9 *surface)
{
    Surface *s = (Surface *)surface;
    if (!s) return;
    if (s->texture) {
        TextureData *t = s->texture;
        if (t->layers == 2) return;
        if (t->target != GL_TEXTURE_2D || t->name || t->faces != 1 || t->levels != 1) {
            bo1_log("d3d9: multiview: can't make a %ux%u texture (fmt %u) layered", t->width, t->height, (unsigned)t->d3d_format);
            return;
        }
        t->layers = 2;
        t->target = GL_TEXTURE_2D_ARRAY;
        bo1_log("d3d9: multiview: %ux%u texture fmt %u is layered", t->width, t->height, (unsigned)t->d3d_format);
        return;
    }
    if (s->layers == 2) return;
    if (s->kind != Surface::RENDERBUFFER || s->renderbuffer || s->texture_name || s->samples > 1) {
        bo1_log("d3d9: multiview: can't make a %ux%u surface (fmt %u) layered", s->desc.Width, s->desc.Height,
                (unsigned)s->desc.Format);
        return;
    }
    s->layers = 2;
    s->sampled = true;
    bo1_log("d3d9: multiview: %ux%u surface fmt %u is layered", s->desc.Width, s->desc.Height, (unsigned)s->desc.Format);
}

HRESULT Device::GetSwapChain(UINT, IDirect3DSwapChain9 **swap_chain)
{
    *swap_chain = new SwapChain(this);
    return S_OK;
}

/* ---- the adapter ----------------------------------------------------------------------------- */

struct Direct3D final : Counted<IDirect3D9Ex> {
    UINT GetAdapterCount() override { return 1; }
    HRESULT GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *id) override
    {
        memset(id, 0, sizeof *id);
        strcpy(id->Driver, "bo1quest");
        strcpy(id->Description, "Quest (OpenGL ES 3.2)");
        strcpy(id->DeviceName, "\\\\.\\DISPLAY1");
        id->VendorId = 0x5143; /* Qualcomm */
        return S_OK;
    }
    UINT GetAdapterModeCount(UINT, D3DFORMAT) override { return 1; }
    HRESULT EnumAdapterModes(UINT, D3DFORMAT f, UINT mode, D3DDISPLAYMODE *m) override
    {
        if (mode) return D3DERR_INVALIDCALL;
        m->Width = 1920;
        m->Height = 1080;
        m->RefreshRate = 72;
        m->Format = f == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : f;
        return S_OK;
    }
    HRESULT GetAdapterDisplayMode(UINT, D3DDISPLAYMODE *m) override { return EnumAdapterModes(0, D3DFMT_X8R8G8B8, 0, m); }
    /* vendor formats (INTZ, ATOC, NULL...) are refused, so the renderer takes its plain D3D9 paths */
    HRESULT CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT check) override
    {
        if ((DWORD)check == FMT_RESZ) return S_OK;
        return format_of(check).internal && (DWORD)check != FMT_NULL ? S_OK : D3DERR_NOTAVAILABLE;
    }
    HRESULT CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE samples, DWORD *quality) override
    {
        if (quality) *quality = 1;
        return samples <= D3DMULTISAMPLE_NONMASKABLE ? S_OK : D3DERR_NOTAVAILABLE;
    }
    HRESULT CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return S_OK; }
    HRESULT GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9 *caps) override
    {
        bo1_d3d9_fill_caps(caps);
        return S_OK;
    }
    HMONITOR GetAdapterMonitor(UINT) override { return MonitorFromWindow(NULL, 0); }
    HRESULT CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **device) override
    {
        *device = new Device(*params);
        bo1_log("d3d9: OpenGL ES device, back buffer %ux%u format %u", params->BackBufferWidth, params->BackBufferHeight,
                (unsigned)params->BackBufferFormat);
        return S_OK;
    }
    HRESULT CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior, D3DPRESENT_PARAMETERS *params,
                           D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **device) override
    {
        return CreateDevice(adapter, type, window, behavior, params, (IDirect3DDevice9 **)device);
    }
};

} // namespace

extern "C" void bo1_d3d9_pick_request(int x, int y, void *texture)
{
    g_pick_texture = texture;
    g_pick_y = y;
    g_pick_x = x;
}

/* the newest answer: 1 with the texture coordinate when the pixel showed the texture; *serial
   changes with every answer */
extern "C" int bo1_d3d9_pick_result(float *u, float *v, unsigned *serial)
{
    *u = g_pick_u;
    *v = g_pick_v;
    *serial = g_pick_serial;
    return g_pick_valid;
}

extern "C" IDirect3D9 *Direct3DCreate9(UINT) { return new Direct3D(); }

extern "C" HRESULT Direct3DCreate9Ex(UINT, IDirect3D9Ex **d3d)
{
    *d3d = new Direct3D();
    return S_OK;
}
