/* A shadow of the GL state d3d9_gles.cpp sets per draw, so calls that would change nothing are
 * skipped. On the Quest the render thread is bound by the driver's per-call cost (each draw used
 * to re-send every state), and stereo doubles the draws, so this matters.
 *
 * Included by d3d9_gles.cpp after the GL headers: from then on its calls to these functions go
 * through the shadow (the macros below). Code that changes this state without going through it
 * (the host between frames: host_gl_present, the cursor) is followed by gl_cache_forget(). Deleted
 * textures and framebuffers drop out of the shadow, since GL unbinds them. */
#pragma once

#include <GLES3/gl32.h>
#include <string.h>

namespace gl_cache {

const int UNITS = 32;

struct State {
    bool valid;
    /* capabilities: scissor, depth, blend, cull, stencil, polygon offset (0/1, or -1 unknown) */
    signed char caps[6];
    GLint viewport[4], scissor[4];
    GLfloat depth_range[2];
    GLenum depth_func;
    GLboolean depth_mask;
    GLenum blend_func[4], blend_equation[2];
    GLfloat blend_colour[4];
    GLboolean colour_mask[4];
    GLenum cull_face;
    GLenum stencil_func[2][1];
    GLint stencil_ref[2];
    GLuint stencil_read[2];
    GLenum stencil_op[2][3];
    GLuint stencil_write[2];
    GLfloat polygon_offset[2];
    GLenum active_unit;
    GLuint textures[UNITS][4]; /* 2D, 3D, cube, 2D array */
    GLuint samplers[UNITS];
    GLuint draw_framebuffer, read_framebuffer, vertex_array;
    GLuint array_buffer;
    /* the default vertex array's attribute pointers (MojoShader sets them every draw) */
    struct Attribute {
        GLint size;
        GLenum type;
        GLboolean normalized;
        GLsizei stride;
        const void *pointer;
        GLuint buffer;
    } attributes[16];
};

inline State &state()
{
    static State s;
    return s;
}

inline void forget() { state().valid = false; }

/* true when the shadow is unknown (and makes it known) */
inline bool reset_if_unknown()
{
    State &s = state();
    if (s.valid) return false;
    memset(&s, 0xFF, sizeof s); /* nothing matches */
    s.valid = true;
    return true;
}

inline int cap_index(GLenum cap)
{
    switch (cap) {
    case GL_SCISSOR_TEST: return 0;
    case GL_DEPTH_TEST: return 1;
    case GL_BLEND: return 2;
    case GL_CULL_FACE: return 3;
    case GL_STENCIL_TEST: return 4;
    case GL_POLYGON_OFFSET_FILL: return 5;
    default: return -1;
    }
}

inline void set_cap(GLenum cap, bool on)
{
    reset_if_unknown();
    int i = cap_index(cap);
    if (i >= 0 && state().caps[i] == (on ? 1 : 0)) return;
    if (on) glEnable(cap);
    else glDisable(cap);
    if (i >= 0) state().caps[i] = on ? 1 : 0;
}

template <class T, size_t N> inline bool same(T (&slot)[N], const T (&value)[N])
{
    if (!memcmp(slot, value, sizeof slot)) return true;
    memcpy(slot, value, sizeof slot);
    return false;
}

inline void viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    reset_if_unknown();
    const GLint v[4] = {x, y, w, h};
    if (!same(state().viewport, v)) glViewport(x, y, w, h);
}
inline void scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    reset_if_unknown();
    const GLint v[4] = {x, y, w, h};
    if (!same(state().scissor, v)) glScissor(x, y, w, h);
}
inline void depth_range(GLfloat n, GLfloat f)
{
    reset_if_unknown();
    const GLfloat v[2] = {n, f};
    if (!same(state().depth_range, v)) glDepthRangef(n, f);
}
inline void depth_func(GLenum f)
{
    reset_if_unknown();
    if (state().depth_func != f) glDepthFunc(state().depth_func = f);
}
inline void depth_mask(GLboolean m)
{
    reset_if_unknown();
    if (state().depth_mask != m) glDepthMask(state().depth_mask = m);
}
inline void blend_func(GLenum a, GLenum b, GLenum c, GLenum d)
{
    reset_if_unknown();
    const GLenum v[4] = {a, b, c, d};
    if (!same(state().blend_func, v)) glBlendFuncSeparate(a, b, c, d);
}
inline void blend_equation(GLenum a, GLenum b)
{
    reset_if_unknown();
    const GLenum v[2] = {a, b};
    if (!same(state().blend_equation, v)) glBlendEquationSeparate(a, b);
}
inline void blend_colour(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    reset_if_unknown();
    const GLfloat v[4] = {r, g, b, a};
    if (!same(state().blend_colour, v)) glBlendColor(r, g, b, a);
}
inline void colour_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    reset_if_unknown();
    const GLboolean v[4] = {r, g, b, a};
    if (!same(state().colour_mask, v)) glColorMask(r, g, b, a);
}
inline void cull_face(GLenum f)
{
    reset_if_unknown();
    if (state().cull_face != f) glCullFace(state().cull_face = f);
}
inline int face_index(GLenum face) { return face == GL_BACK ? 1 : 0; }
inline void stencil_func(GLenum face, GLenum func, GLint ref, GLuint mask)
{
    reset_if_unknown();
    State &s = state();
    if (face == GL_FRONT_AND_BACK) {
        s.stencil_func[0][0] = s.stencil_func[1][0] = 0xFFFFFFFF; /* not tracked together */
        glStencilFuncSeparate(face, func, ref, mask);
        return;
    }
    int i = face_index(face);
    if (s.stencil_func[i][0] == func && s.stencil_ref[i] == ref && s.stencil_read[i] == mask) return;
    s.stencil_func[i][0] = func;
    s.stencil_ref[i] = ref;
    s.stencil_read[i] = mask;
    glStencilFuncSeparate(face, func, ref, mask);
}
inline void stencil_op(GLenum face, GLenum fail, GLenum zfail, GLenum pass)
{
    reset_if_unknown();
    State &s = state();
    if (face == GL_FRONT_AND_BACK) {
        s.stencil_op[0][0] = s.stencil_op[1][0] = 0xFFFFFFFF;
        glStencilOpSeparate(face, fail, zfail, pass);
        return;
    }
    const GLenum v[3] = {fail, zfail, pass};
    if (!same(s.stencil_op[face_index(face)], v)) glStencilOpSeparate(face, fail, zfail, pass);
}
inline void stencil_mask(GLenum face, GLuint mask)
{
    reset_if_unknown();
    State &s = state();
    if (face == GL_FRONT_AND_BACK) {
        if (s.stencil_write[0] == mask && s.stencil_write[1] == mask) return;
        s.stencil_write[0] = s.stencil_write[1] = mask;
    } else {
        GLuint &slot = s.stencil_write[face_index(face)];
        if (slot == mask) return;
        slot = mask;
    }
    glStencilMaskSeparate(face, mask);
}
inline void polygon_offset(GLfloat factor, GLfloat units)
{
    reset_if_unknown();
    const GLfloat v[2] = {factor, units};
    if (!same(state().polygon_offset, v)) glPolygonOffset(factor, units);
}

inline void active_texture(GLenum unit)
{
    reset_if_unknown();
    if (state().active_unit != unit) glActiveTexture(state().active_unit = unit);
}
inline int target_index(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_2D: return 0;
    case GL_TEXTURE_3D: return 1;
    case GL_TEXTURE_CUBE_MAP: return 2;
    case GL_TEXTURE_2D_ARRAY: return 3;
    default: return -1;
    }
}
inline void bind_texture(GLenum target, GLuint name)
{
    reset_if_unknown();
    State &s = state();
    unsigned unit = s.active_unit - GL_TEXTURE0;
    int t = target_index(target);
    if (unit < (unsigned)UNITS && t >= 0) {
        if (s.textures[unit][t] == name) return;
        s.textures[unit][t] = name;
    }
    glBindTexture(target, name);
}
/* true when this unit already has this texture and sampler bound (known state only) */
inline bool unit_has(unsigned unit, GLenum target, GLuint name, GLuint sampler)
{
    const State &s = state();
    int t = target_index(target);
    return s.valid && unit < (unsigned)UNITS && t >= 0 && s.textures[unit][t] == name && s.samplers[unit] == sampler;
}
inline void bind_sampler(GLuint unit, GLuint sampler)
{
    reset_if_unknown();
    if (unit < (unsigned)UNITS) {
        if (state().samplers[unit] == sampler) return;
        state().samplers[unit] = sampler;
    }
    glBindSampler(unit, sampler);
}
inline void bind_framebuffer(GLenum target, GLuint fbo)
{
    reset_if_unknown();
    State &s = state();
    bool draw = target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER;
    bool read = target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER;
    if ((!draw || s.draw_framebuffer == fbo) && (!read || s.read_framebuffer == fbo)) return;
    if (draw) s.draw_framebuffer = fbo;
    if (read) s.read_framebuffer = fbo;
    glBindFramebuffer(target, fbo);
}
inline void bind_vertex_array(GLuint vao)
{
    reset_if_unknown();
    if (state().vertex_array != vao) glBindVertexArray(state().vertex_array = vao);
}
inline void bind_buffer(GLenum target, GLuint name)
{
    reset_if_unknown();
    if (target == GL_ARRAY_BUFFER) {
        if (state().array_buffer == name) return;
        state().array_buffer = name;
    }
    glBindBuffer(target, name);
}
inline void delete_buffers(GLsizei n, const GLuint *names)
{
    State &s = state();
    for (GLsizei i = 0; i < n; i++) {
        if (s.array_buffer == names[i]) s.array_buffer = 0;
        for (State::Attribute &a : s.attributes)
            if (a.buffer == names[i]) a.buffer = 0xFFFFFFFF; /* a new buffer may take the name */
    }
    glDeleteBuffers(n, names);
}
/* given to MojoShader in place of glVertexAttribPointer (d3d9_gles.cpp's gl_lookup) */
inline void GL_APIENTRY vertex_attrib_pointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                              const void *pointer)
{
    reset_if_unknown();
    State &s = state();
    if (s.vertex_array != 0 || index >= 16) {
        glVertexAttribPointer(index, size, type, normalized, stride, pointer);
        return;
    }
    State::Attribute &a = s.attributes[index];
    if (a.size == size && a.type == type && a.normalized == normalized && a.stride == stride && a.pointer == pointer &&
        a.buffer == s.array_buffer)
        return;
    a = {size, type, normalized, stride, pointer, s.array_buffer};
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

inline void delete_textures(GLsizei n, const GLuint *names)
{
    State &s = state();
    for (GLsizei i = 0; i < n; i++)
        for (int u = 0; u < UNITS; u++)
            for (int t = 0; t < 4; t++)
                if (s.textures[u][t] == names[i]) s.textures[u][t] = 0;
    glDeleteTextures(n, names);
}
inline void delete_framebuffers(GLsizei n, const GLuint *names)
{
    State &s = state();
    for (GLsizei i = 0; i < n; i++) {
        if (s.draw_framebuffer == names[i]) s.draw_framebuffer = 0;
        if (s.read_framebuffer == names[i]) s.read_framebuffer = 0;
    }
    glDeleteFramebuffers(n, names);
}

} // namespace gl_cache

inline void gl_cache_forget() { gl_cache::forget(); }

#define glEnable(cap) gl_cache::set_cap(cap, true)
#define glDisable(cap) gl_cache::set_cap(cap, false)
#define glViewport gl_cache::viewport
#define glScissor gl_cache::scissor
#define glDepthRangef gl_cache::depth_range
#define glDepthFunc gl_cache::depth_func
#define glDepthMask gl_cache::depth_mask
#define glBlendFuncSeparate gl_cache::blend_func
#define glBlendEquationSeparate gl_cache::blend_equation
#define glBlendColor gl_cache::blend_colour
#define glColorMask gl_cache::colour_mask
#define glCullFace gl_cache::cull_face
#define glStencilFuncSeparate gl_cache::stencil_func
#define glStencilOpSeparate gl_cache::stencil_op
#define glStencilMaskSeparate gl_cache::stencil_mask
#define glPolygonOffset gl_cache::polygon_offset
#define glActiveTexture gl_cache::active_texture
#define glBindTexture gl_cache::bind_texture
#define glBindSampler gl_cache::bind_sampler
#define glBindFramebuffer gl_cache::bind_framebuffer
#define glBindVertexArray gl_cache::bind_vertex_array
#define glDeleteTextures gl_cache::delete_textures
#define glDeleteFramebuffers gl_cache::delete_framebuffers
#define glBindBuffer gl_cache::bind_buffer
#define glDeleteBuffers gl_cache::delete_buffers
