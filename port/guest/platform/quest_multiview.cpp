/* Multiview, the engine side: both eyes drawn in one pass (OVR_multiview2).
 *
 * Off unless the dvar bo1_vr_multiview is set (it takes effect at the next start: the eyes' render
 * targets are made as two-layer texture arrays then). With it, while playing (not in menus, which
 * keep drawing each eye on its own) a game frame is drawn once: the front end culls for a view that
 * covers both eyes and draws for the left eye (quest_vr.cpp, bo1_vr_view with eye 2); the back end
 * runs once, and every shader constant that depends on the eye is sent twice - the left eye's as the
 * engine sets it, the right eye's through bo1_d3d9_eye1_constants - and the device's shaders pick
 * theirs by gl_ViewID_OVR (d3d9_gles.cpp).
 *
 * What depends on the eye: the view, projection, view-projection, world-view, world-view-projection
 * and outdoor-lookup code matrices (and their inverses and transposes), and the near-plane and
 * pixel-to-world code constants, all of the main scene view only (shadow views are the same for both
 * eyes). The right eye's are the engine's own derivations with that eye's view parameters swapped in
 * (Eye1Scope); the camera-relative origin (eyeOffset) stays the left eye's for both, so the world
 * matrix is shared. The 2D drawn into the eyes while the crosshair is up gets the right eye's
 * on-screen offset added to its projection (bo1_mv_set_2d_offset, from the crosshair's marker). */
#include <math.h>
#include <string.h>

#include "gfx_d3d/rb_state.h"
#include "gfx_d3d/r_state.h"
#include "gfx_d3d/r_state_utils.h"
#include "gfx_d3d/r_gfx.h"
#include "gfx_d3d/r_material.h"
#include "universal/com_math.h"
#include "win32_internal.h"
#include "win32/win_shared.h"

extern "C" void bo1_d3d9_eye1_constants(int pixel, unsigned int dest, const float *data, unsigned int rows); /* d3d9_gles.cpp */
extern "C" void bo1_d3d9_clip1(const float *m); /* d3d9_gles.cpp */

/* render thread, set by quest_vr.cpp's bo1_vr_backend_frame for each back-end frame */
bool g_bo1_mv_frame;               /* this frame draws both eyes in one pass */
GfxViewParms g_bo1_mv_eye1;        /* the right eye's view of the main scene */
float g_bo1_mv_eye0_view[4][4];    /* the left eye's view matrix: how the main scene's view is known */
GfxViewParms g_bo1_mv_eye0;        /* the left eye's view of the main scene */

namespace {

const unsigned int FIRST_MATRIX = CONST_SRC_FIRST_CODE_MATRIX;
const GfxViewParms *g_main;         /* the main scene view's viewParms3D while it's being drawn */
unsigned int g_serial = 1;          /* bumped when what the right eye's values depend on changes */
float g_consts[CONST_SRC_VPOS1_TO_WORLD + 1][4]; /* the right eye's near-plane and pixel-to-world constants */
bool g_2d_on;
bool g_reproject;                   /* the sun and coronas: placed in the left eye's clip space on the CPU */
float g_2d_offset[2];               /* the right eye's 2D offset, clip space */

bool eye_constant(unsigned int index)
{
    return (index >= CONST_SRC_CODE_NEARPLANE_ORG && index <= CONST_SRC_CODE_NEARPLANE_DY) ||
           (index >= CONST_SRC_VPOSX_TO_WORLD && index <= CONST_SRC_VPOS1_TO_WORLD);
}

/* the code matrices that depend on the view: groups 1-5 (view .. world-view-projection) and 7
   (outdoor lookup, which goes through the inverse view); 0 (world) and 6 (shadow lookup) don't */
bool eye_matrix_group(unsigned int group) { return (group >= 1 && group <= 5) || group == 7; }

/* the engine's state with the right eye's view swapped in, and everything it derives from it
   forgotten; all put back as it was when the scope ends */
struct Eye1Scope {
    GfxCmdBufSourceState *s;
    GfxViewParms view;
    GfxMatrix matrices[28];
    unsigned short versions[28];
    float consts[9][4];
    unsigned short const_versions[9];
    Eye1Scope(GfxCmdBufSourceState *source, const GfxViewParms &eye1) : s(source)
    {
        view = s->viewParms;
        memcpy(matrices, &s->matrices.matrix[4], sizeof matrices);
        memcpy(versions, &s->constVersions[FIRST_MATRIX + 4], sizeof versions);
        memcpy(consts, s->input.consts[CONST_SRC_CODE_NEARPLANE_ORG], sizeof consts);
        memcpy(const_versions, &s->constVersions[CONST_SRC_CODE_NEARPLANE_ORG], sizeof const_versions);
        s->viewParms = eye1;
        /* versions that can't match the current ones (which may be any value, 0 too, as they wrap) */
        for (int i = 0; i < 28; i++)
            s->constVersions[FIRST_MATRIX + 4 + i] = (unsigned short)(s->matrixVersions[(4 + i) >> 2] ^ 0x8000);
    }
    ~Eye1Scope()
    {
        s->viewParms = view;
        memcpy(&s->matrices.matrix[4], matrices, sizeof matrices);
        memcpy(&s->constVersions[FIRST_MATRIX + 4], versions, sizeof versions);
        memcpy(s->input.consts[CONST_SRC_CODE_NEARPLANE_ORG], consts, sizeof consts);
        memcpy(&s->constVersions[CONST_SRC_CODE_NEARPLANE_ORG], const_versions, sizeof const_versions);
    }
};

/* the right eye's view parameters for what this source draws now; false if it's the same for both */
bool eye1_parms(const GfxCmdBufSourceState *source, GfxViewParms *out)
{
    if (source->viewMode == VIEW_MODE_3D) {
        /* the main scene (any list of it: they may keep their own copy of its view) */
        const float(*v)[4] = source->viewParms.viewMatrix.m;
        if (!memcmp(v, g_bo1_mv_eye0_view, sizeof g_bo1_mv_eye0_view)) {
            *out = g_bo1_mv_eye1;
            return true;
        }
        /* a variant of it: the same turn, its own place or depth range (the sky and the sun, effects drawn
           from a view without its translation). Its right eye: the eyes' offset added, and the right eye's
           off-centre frustum with the variant's own depth terms. Without this the sun and some effects were
           drawn where the left eye sees them, in both eyes - doubled, or in one eye only. */
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                if (fabsf(v[r][c] - g_bo1_mv_eye0_view[r][c]) > 1e-4f) return false;
        *out = source->viewParms;
        for (int c = 0; c < 3; c++) out->viewMatrix.m[3][c] += g_bo1_mv_eye1.viewMatrix.m[3][c] - g_bo1_mv_eye0_view[3][c];
        const float(*e)[4] = g_bo1_mv_eye1.projectionMatrix.m;
        float(*p)[4] = out->projectionMatrix.m;
        p[0][0] = e[0][0];
        p[1][1] = e[1][1];
        p[2][0] = e[2][0];
        p[2][1] = e[2][1];
        MatrixMultiply44(out->viewMatrix.m, out->projectionMatrix.m, out->viewProjectionMatrix.m);
        MatrixInverse44(*(mat4x4 *)&out->viewProjectionMatrix, *(mat4x4 *)&out->inverseViewProjectionMatrix);
        return true;
    }
    if (source->viewMode == VIEW_MODE_2D && g_2d_on) {
        /* clip.xy += offset * clip.w */
        *out = source->viewParms;
        GfxMatrix *ms[2] = {&out->projectionMatrix, &out->viewProjectionMatrix};
        for (GfxMatrix *m : ms)
            for (int i = 0; i < 4; i++) {
                m->m[i][0] += g_2d_offset[0] * m->m[i][3];
                m->m[i][1] += g_2d_offset[1] * m->m[i][3];
            }
        return true;
    }
    return false;
}

struct Cached {
    unsigned int serial;
    unsigned short version;
    float m[4][4];
};
Cached g_cache[32];

/* the right eye's rows of code matrix `index` (from firstRow), or NULL when it's the left eye's */
const float *eye1_matrix(GfxCmdBufSourceState *source, unsigned int index, unsigned int first_row)
{
    unsigned int mi = index - FIRST_MATRIX, group = mi >> 2;
    if (mi >= 32 || !eye_matrix_group(group)) return NULL;
    GfxViewParms parms;
    if (!eye1_parms(source, &parms)) return NULL;
    Cached &c = g_cache[mi];
    unsigned short version = source->matrixVersions[group];
    if (c.serial == g_serial && c.version == version) return c.m[first_row];
    if (group == 5 && source->depthHackFlags != 2 && !(mi & 1)) {
        /* world-view-projection (or its transpose), the per-object one: directly, as
           R_DeriveWorldViewProjectionMatrix does it */
        GfxMatrix world = source->matrices.matrix[0];
        for (int i = 0; i < 3; i++) world.m[3][i] += source->eyeOffset[i];
        GfxMatrix wvp;
        MatrixMultiply44(world.m, parms.viewProjectionMatrix.m, wvp.m);
        if (mi & 2) MatrixTranspose44(*(mat4x4 *)&wvp, *(mat4x4 *)c.m);
        else memcpy(c.m, wvp.m, sizeof c.m);
    } else {
        Eye1Scope scope(source, parms);
        memcpy(c.m, R_GetCodeMatrix(source, index, 0), sizeof c.m);
    }
    c.serial = g_serial;
    c.version = version;
    return c.m[first_row];
}

} // namespace

/* end of R_BeginView: is this the main scene's view? Then the right eye's view constants */
void bo1_mv_begin_view(GfxCmdBufSourceState *source)
{
    if (!g_bo1_mv_frame) {
        g_main = NULL;
        return;
    }
    static unsigned matched, unmatched, logged_at;
    unsigned now = (unsigned)Sys_Milliseconds();
    if (now - logged_at > 5000) {
        if (logged_at) bo1_log("multiview: main view found %u times, other views %u (5 s)", matched, unmatched);
        logged_at = now;
        matched = unmatched = 0;
    }
    if (!memcmp(source->viewParms.viewMatrix.m, g_bo1_mv_eye0_view, sizeof g_bo1_mv_eye0_view)) {
        matched++;
        g_main = source->viewParms3D;
        g_serial++;
        Eye1Scope scope(source, g_bo1_mv_eye1);
        R_DeriveNearPlaneConstantsForView(source);
        UpdateVPosToWorld(source);
        for (unsigned int i = 0; i <= CONST_SRC_VPOS1_TO_WORLD; i++)
            if (eye_constant(i)) memcpy(g_consts[i], source->input.consts[i], sizeof g_consts[i]);
    } else {
        unmatched++;
        if (source->viewParms3D == g_main) g_main = NULL;
    }
}

/* around the sun's and the coronas' drawing (rb_sky.cpp, rb_corona.cpp): their identity-mode draws are
   re-projected into the right eye */
void bo1_mv_reproject(bool on)
{
    g_reproject = on && g_bo1_mv_frame;
    /* their vertex shaders pass the position through (no matrix to swap): the device applies this to
       the right eye's position instead - back to the world with the left eye's view-projection's
       inverse, on with the right eye's (row vectors: clip1 = clip0 * m) */
    if (!g_reproject) {
        bo1_d3d9_clip1(NULL);
        return;
    }
    GfxMatrix inverse, m;
    MatrixInverse44(*(const mat4x4 *)&g_bo1_mv_eye0.viewProjectionMatrix, *(mat4x4 *)&inverse);
    MatrixMultiply44(inverse.m, g_bo1_mv_eye1.viewProjectionMatrix.m, m.m);
    bo1_d3d9_clip1(&m.m[0][0]);
}

/* the crosshair's marker: the 2D drawn into the eyes from now on is offset this much (clip space)
   in the right eye, until it's turned off */
void bo1_mv_set_2d_offset(bool on, float dx, float dy)
{
    g_2d_on = on && g_bo1_mv_frame;
    g_2d_offset[0] = dx;
    g_2d_offset[1] = dy;
    g_serial++;
    /* the engine's own "this register is up to date" checks go by these versions: make it send the
       2D matrices again, so the right eye's are sent with them */
    for (int g = 1; g <= 5; g++) ++gfxCmdBufSourceState.matrixVersions[g];
}

/* after a vertex shader code constant is set: the right eye's, where it differs */
void bo1_mv_vertex_code_constant(GfxCmdBufContext context, const MaterialShaderArgument *arg)
{
    if (!g_bo1_mv_frame) return;
    unsigned int index = arg->u.codeConst.index;
    if (index >= FIRST_MATRIX) {
        const float *m = eye1_matrix(context.source, index, arg->u.codeConst.firstRow);
        if (m) bo1_d3d9_eye1_constants(0, arg->dest, m, arg->u.codeConst.rowCount);
        return;
    }
    GfxViewParms parms;
    if (eye_constant(index) && context.source->viewMode == VIEW_MODE_3D && eye1_parms(context.source, &parms))
        bo1_d3d9_eye1_constants(0, arg->dest, g_consts[index], 1);
}

/* after a pixel shader code constant is set (rows of consecutive constants) */
void bo1_mv_pixel_code_constant(GfxCmdBufContext context, const MaterialShaderArgument *arg)
{
    if (!g_bo1_mv_frame || context.source->viewMode != VIEW_MODE_3D) return;
    GfxViewParms parms;
    unsigned int index = arg->u.codeConst.index, rows = arg->u.codeConst.rowCount;
    bool any = false;
    for (unsigned int r = 0; r < rows; r++) any |= index + r < FIRST_MATRIX && eye_constant(index + r);
    if (!any || !eye1_parms(context.source, &parms)) return;
    for (unsigned int r = 0; r < rows; r++)
        if (eye_constant(index + r)) bo1_d3d9_eye1_constants(1, arg->dest + r, g_consts[index + r], 1);
}
