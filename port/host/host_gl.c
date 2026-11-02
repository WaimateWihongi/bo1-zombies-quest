/* OpenGL ES for the guest's D3D9 device: the EGL context on the activity's window, and the GL
 * calls whose arguments can't simply be widened (port/tools/gen_gl.py writes the others).
 *
 * The context is made current on whichever guest thread calls host_gl_begin (the engine's render
 * thread), and stays there. With a headset (host_xr.c) the context sits on a small pbuffer and
 * every frame goes to OpenXR; without one it draws into the activity's window, a flat panel. */
#include "host.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <android/native_window.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_changed = PTHREAD_COND_INITIALIZER;
static ANativeWindow *g_window;
static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLConfig g_config;
static int g_xr; /* frames go to OpenXR */

/* the main thread's view of the window (APP_CMD_INIT_WINDOW / APP_CMD_TERM_WINDOW) */
void host_gl_set_window(ANativeWindow *window)
{
    host_info("gl: window %p (%dx%d)", (void *)window, window ? ANativeWindow_getWidth(window) : 0,
              window ? ANativeWindow_getHeight(window) : 0);
    pthread_mutex_lock(&g_lock);
    g_window = window;
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
}

static int create_context(int pbuffer)
{
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_display, NULL, NULL)) return 0;
    const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                        EGL_SURFACE_TYPE, pbuffer ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
                                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                        EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(g_display, config_attributes, &g_config, 1, &count) || !count) return 0;
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, context_attributes);
    if (g_context == EGL_NO_CONTEXT) return 0;
    if (pbuffer) {
        const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        g_surface = eglCreatePbufferSurface(g_display, g_config, size);
    } else {
        g_surface = eglCreateWindowSurface(g_display, g_config, g_window, NULL);
    }
    return g_surface != EGL_NO_SURFACE;
}

/* Waits for the window, makes the context current on the calling thread and writes the window's
   size to the guest's uint32_t[2]. 1 on success. */
uint32_t host_gl_begin(uint32_t size_out)
{
    uint32_t *size = GUEST_PTR(uint32_t *, size_out);
    /* with a headset: a pbuffer, and the session on this context */
    if (host_xr_available() && g_context == EGL_NO_CONTEXT) {
        if (create_context(1) && eglMakeCurrent(g_display, g_surface, g_surface, g_context) &&
            host_xr_start(g_display, g_config, g_context) == 0) {
            g_xr = 1;
            size[0] = 1920;
            size[1] = 1080;
            host_info("gl: %s on %s, frames to OpenXR", glGetString(GL_VERSION), glGetString(GL_RENDERER));
            return 1;
        }
        host_warn("gl: no OpenXR session; the flat panel instead");
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
        if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
        g_surface = EGL_NO_SURFACE;
        g_context = EGL_NO_CONTEXT;
    }
    pthread_mutex_lock(&g_lock);
    while (!g_window) pthread_cond_wait(&g_changed, &g_lock);
    int ok = g_context != EGL_NO_CONTEXT || create_context(0);
    pthread_mutex_unlock(&g_lock);
    if (!ok || !eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        host_error("gl: no OpenGL ES 3.2 context (EGL error %#x)", eglGetError());
        return 0;
    }
    eglSwapInterval(g_display, 1);
    size[0] = (uint32_t)ANativeWindow_getWidth(g_window);
    size[1] = (uint32_t)ANativeWindow_getHeight(g_window);
    host_info("gl: %s on %s, window %ux%u", glGetString(GL_VERSION), glGetString(GL_RENDERER), size[0], size[1]);
    return 1;
}

/* Where this frame's finished picture goes: a framebuffer name and size[0..2] = width, height,
   sRGB (the picture must be linear on the way in). 0 is the window; 0xFFFFFFFF is nowhere (the
   headset isn't showing us). */
uint32_t host_gl_frame_begin(uint32_t size_out)
{
    uint32_t *size = GUEST_PTR(uint32_t *, size_out);
    if (g_xr) {
        uint32_t fbo = host_xr_frame_begin(size);
        if (!fbo) usleep(20000); /* not shown: no frame pacing from the headset, so keep the game slow */
        return fbo ? fbo : 0xFFFFFFFFu;
    }
    size[0] = g_window ? (uint32_t)ANativeWindow_getWidth(g_window) : 0;
    size[1] = g_window ? (uint32_t)ANativeWindow_getHeight(g_window) : 0;
    size[2] = 0;
    return 0;
}

void host_gl_present(void)
{
    if (g_xr) {
        host_xr_frame_end();
        return;
    }
    if (!eglSwapBuffers(g_display, g_surface)) {
        static int reported;
        if (reported++ < 5) host_error("gl: eglSwapBuffers failed (EGL error %#x)", eglGetError());
    }
}

/* ---- the hand-written wrappers --------------------------------------------------------------- */

/* GL's strings live in the driver, above 4 GB: the guest gets copies in low memory, made once */
static uint32_t guest_copy(const GLubyte *text)
{
    if (!text) return 0;
    size_t n = strlen((const char *)text) + 1;
    char *copy = host_low_map((n + 4095) & ~(size_t)4095, PROT_READ | PROT_WRITE);
    if (!copy) return 0;
    memcpy(copy, text, n);
    return (uint32_t)(uintptr_t)copy;
}

uint32_t hgl_glGetString(GLenum name)
{
    static struct { GLenum name; uint32_t copy; } cache[8];
    pthread_mutex_lock(&g_lock);
    uint32_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (cache[i].name == name && cache[i].copy) {
            r = cache[i].copy;
            break;
        }
        if (!cache[i].copy) {
            cache[i].name = name;
            r = cache[i].copy = guest_copy(glGetString(name));
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

uint32_t hgl_glGetStringi(GLenum name, GLuint index)
{
    /* extension names: few, read once at start-up */
    static uint32_t copies[512];
    if (name != GL_EXTENSIONS || index >= 512) return guest_copy(glGetStringi(name, index));
    pthread_mutex_lock(&g_lock);
    if (!copies[index]) copies[index] = guest_copy(glGetStringi(name, index));
    uint32_t r = copies[index];
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* the guest's array of sources holds 32-bit pointers */
void hgl_glShaderSource(GLuint shader, GLsizei count, uint32_t strings, uint32_t lengths)
{
    const uint32_t *guest_strings = GUEST_PTR(const uint32_t *, strings);
    const GLchar *host_strings[64];
    if (count > 64) count = 64;
    for (GLsizei i = 0; i < count; i++) host_strings[i] = GUEST_PTR(const GLchar *, guest_strings[i]);
    glShaderSource(shader, count, host_strings, GUEST_PTR(const GLint *, lengths));
}

/* Streaming ring for the guest's changing vertex and index data: one GL buffer, mapped once and
 * for good (EXT_buffer_storage), split into STREAM_PARTS parts used one frame each. Writes are
 * plain copies into the mapping; each part is fenced when its frame ends and waited on only when
 * it comes round again, frames later. The mapping and the fences live above 4 GB, out of the
 * guest's reach, hence the host side. Without the extension it falls back to glBufferSubData. */
#define STREAM_PARTS 3
typedef void(GL_APIENTRY *buffer_storage_fn)(GLenum, GLsizeiptr, const void *, GLbitfield);
static uint8_t *g_stream_map;
static GLsync g_stream_fences[STREAM_PARTS];

/* makes the ring (size bytes, bound to GL_COPY_WRITE_BUFFER) and returns its GL name */
uint32_t host_gl_stream_create(uint32_t size)
{
    GLuint name;
    glGenBuffers(1, &name);
    glBindBuffer(GL_COPY_WRITE_BUFFER, name);
    buffer_storage_fn storage = (buffer_storage_fn)eglGetProcAddress("glBufferStorageEXT");
    const GLbitfield flags = GL_MAP_WRITE_BIT | 0x0040 /* PERSISTENT */ | 0x0080 /* COHERENT */;
    if (storage) {
        storage(GL_COPY_WRITE_BUFFER, size, NULL, flags);
        g_stream_map = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, size, flags);
    }
    if (!g_stream_map) {
        glBufferData(GL_COPY_WRITE_BUFFER, size, NULL, GL_STREAM_DRAW);
        host_warn("gl: no persistent mapping for the stream ring; glBufferSubData instead");
    } else {
        host_info("gl: stream ring %u MB, persistently mapped", size >> 20);
    }
    return name;
}

/* copies guest memory to the ring at offset (the ring is bound to GL_COPY_WRITE_BUFFER) */
void host_gl_stream_write(uint32_t offset, uint32_t size, uint32_t data)
{
    if (g_stream_map) memcpy(g_stream_map + offset, GUEST_PTR(const void *, data), size);
    else glBufferSubData(GL_COPY_WRITE_BUFFER, offset, size, GUEST_PTR(const void *, data));
}

/* the frame that used `part` is submitted: fence it */
void host_gl_stream_fence(uint32_t part)
{
    if (part >= STREAM_PARTS) return;
    if (g_stream_fences[part]) glDeleteSync(g_stream_fences[part]);
    g_stream_fences[part] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

/* about to write `part` again: wait until the GPU is done with what it held */
void host_gl_stream_wait(uint32_t part)
{
    if (part >= STREAM_PARTS || !g_stream_fences[part]) return;
    glClientWaitSync(g_stream_fences[part], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    glDeleteSync(g_stream_fences[part]);
    g_stream_fences[part] = 0;
}

/* Multiview (OVR_multiview2): both eyes drawn in one pass into a two-layer texture array. The
 * guest's GL bindings come from gl32.h, which doesn't have the extension's entry point. */
typedef void(GL_APIENTRY *multiview_fn)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
static multiview_fn g_multiview;

/* the guest: 1 when the driver has OVR_multiview2 */
int32_t host_gl_has_multiview(void)
{
    static int known = -1;
    if (known < 0) {
        g_multiview = (multiview_fn)eglGetProcAddress("glFramebufferTextureMultiviewOVR");
        GLint count = 0, found = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; i++) {
            const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (e && !strcmp(e, "GL_OVR_multiview2")) found = 1;
        }
        GLint views = 0;
        if (found) glGetIntegerv(0x9631 /* GL_MAX_VIEWS_OVR */, &views);
        known = found && g_multiview && views >= 2;
        host_info("gl: OVR_multiview2 %s (max views %d)", known ? "available" : "missing", views);
    }
    return known;
}

/* the guest: glFramebufferTextureMultiviewOVR */
void host_gl_multiview_attach(uint32_t target, uint32_t attachment, uint32_t texture, int32_t level, int32_t base_view,
                              int32_t views)
{
    if (!g_multiview) host_gl_has_multiview();
    if (g_multiview) g_multiview(target, attachment, texture, level, base_view, views);
}
