/* Stereo: the game drawn once per eye (milestone 5, step 3).
 *
 * Each of the game's frames becomes two renderer frames, one per eye (cl_scrn_mp.cpp runs
 * SCR_UpdateFrame twice). Before the pair the headset's eye poses are fetched once
 * (bo1_vr_begin_pair, main thread); while an eye's frame is built, R_SetViewParmsForScene asks
 * bo1_vr_view to turn the scene's camera into that eye's: the game camera, levelled to its yaw (the
 * head supplies pitch and roll), moved and turned by the eye's pose in the headset's space, with
 * the eye's own off-centre frustum. Which eye a renderer frame is, and the poses it was drawn with,
 * travel with the frame's back-end data (one of two, R_EndFrame -> RB_BeginFrame) to the render
 * thread, which tells the host before the frame is presented (host_xr_set_eye), so the headset is
 * given exactly the poses the picture was drawn for. */
#include <math.h>
#include <string.h>
#include <time.h>

#include "win32_internal.h"
#include "bo1_abi.h"
#include "gfx_d3d/r_gfx.h"
#include "gfx_d3d/r_scene.h"
#include "cgame_mp/cg_local_mp.h"
#include "universal/com_math.h"
#include "client/client.h"
#include "client/cl_keys.h"
#include "win32/win_shared.h"
#include "xanim/dobj.h"
#include "xanim/dobj_utils.h"
#include "xanim/xmodel.h"
#include "cgame/cg_drawtools.h"
#include "cgame_mp/cg_ents_mp.h"
#include "clientscript/cscr_stringlist.h"
#include "cgame/cg_world.h"
#include "qcommon/cm_trace.h"
#include "client/screen_placement.h"

extern "C" unsigned int host_xr_views(struct bo1_xr_views *views);
extern "C" void host_xr_set_eye(int eye, const struct bo1_xr_views *views);

/* game units per metre (a unit is an inch) */
static const float UNITS_PER_METRE = 39.3701f;

static bo1_xr_views g_views;     /* main thread: this pair's poses */
/* in game: how far (degrees, left positive) the hand's aim is turned from the body's heading. The
   game's view (its yaw) is the aim; the eyes are drawn from the body's heading, so the camera turns
   with the stick and the head, not with the hand. */
static float g_aim_yaw;
static float g_eye_vp[4][4]; /* the eye being built: its view-projection (row vectors), for the crosshair */
static int g_eye_vp_eye = -1;
bool g_bo1_vr_controls;          /* main thread: the controllers play this frame (quest_input.cpp) */
/* both hands on the gun (the Halo port's grip mode): the left grip squeezed with the left hand at the
   gun's support grip - where the game's animation puts that hand - locks it there until let go; the
   gun then points from the right hand to the left */
static bool g_two_hand;
bool g_bo1_mask_lb;              /* the left grip is the support hand's, not the tactical grenade (quest_input.cpp) */
static float g_support_local[3]; /* the support grip's place, LOCAL space (bo1_vr_left_hand, each frame) */
static bool g_support_valid;
/* the right hand's last tracked aim pose (LOCAL space), for the gun in the hand */
static struct {
    bool valid;
    float position[3];
    float orientation[4];
} g_hand, g_left;
static int g_eye = -1;           /* main thread: the eye being built, -1 for a flat frame, 2 both (multiview) */
/* multiview (quest_multiview.cpp): the right eye's view and the view that culls for both, as the
   frame's left-eye view is built (bo1_vr_view with eye 2) */
static GfxViewParms g_mv_eye1, g_mv_dpvs;
static float g_mv_eye0_view[4][4];
static GfxViewParms g_mv_eye0;
static bool g_mv_built;
static struct {
    int eye;
    bo1_xr_views views;
    GfxViewParms eye1;           /* multiview: the right eye's view */
    GfxViewParms eye0;           /* multiview: the left eye's (for re-projecting what's placed in its clip space) */
    float eye0_view[4][4];       /* multiview: the left eye's view matrix */
} g_frames[2];                   /* per back-end data buffer */
int g_bo1_render_eye = -1;       /* render thread: the eye being drawn (d3d9_gles.cpp's pick uses it), 2 both */
static int g_backend_index = -1; /* render thread: the back-end data being drawn */

/* render thread (quest_multiview.cpp) */
extern bool g_bo1_mv_frame;
extern GfxViewParms g_bo1_mv_eye1;
extern float g_bo1_mv_eye0_view[4][4];
extern GfxViewParms g_bo1_mv_eye0;

/* multiview: wanted (the dvar, read once when the render targets are made: they're made for it then)
   and possible (the driver, d3d9_gles.cpp says at GL start) */
static const dvar_s *bo1_vr_multiview;
static bool g_mv_storage;
bool g_bo1_mv_capable;

bool bo1_vr_multiview_storage()
{
    if (!bo1_vr_multiview)
        bo1_vr_multiview = _Dvar_RegisterBool("bo1_vr_multiview", 0, 0u,
                                              "VR: draw both eyes in one pass while playing (from the next start)");
    g_mv_storage = bo1_vr_multiview->current.enabled;
    bo1_log("vr: multiview %s", g_mv_storage ? "on (two-layer eye targets)" : "off");
    return g_mv_storage;
}

bool bo1_vr_begin_pair()
{
    return host_xr_views(&g_views) && g_views.valid;
}

/* this pair as one multiview frame: only while playing (the menus' in-world screens need the pointer
   picking of single-eye frames) */
extern unsigned g_debug_flags; /* d3d9_gles.cpp: bit 26 turns multiview frames off (debug) */
bool bo1_vr_multiview_pair()
{
    /* debug: bit 26 off, bit 27 on even without the controllers (testing with the headset on a desk) */
    return g_mv_storage && g_bo1_mv_capable && (g_bo1_vr_controls || (g_debug_flags & (1u << 27))) && !(g_debug_flags & (1u << 26));
}

/* R_RenderScene: the view to cull with when it differs from the one drawn (multiview: both eyes) */
const GfxViewParms *bo1_vr_dpvs_view() { return g_eye == 2 && g_mv_built ? &g_mv_dpvs : NULL; }

void bo1_vr_set_eye(int eye) { g_eye = eye; }
int bo1_vr_eye() { return g_eye; }

void bo1_vr_record_frame(int index)
{
    static int calls;
    if (calls++ % 500 == 0) bo1_log("vr: front-end frame data %d, eye %d", index, g_eye);
    if (index < 0 || index > 1) return;
    g_frames[index].eye = g_eye;
    g_frames[index].views = g_views;
    /* (a multiview frame with no scene view, g_mv_built false, draws the same picture into both eyes) */
    if (g_frames[index].eye == 2) {
        g_frames[index].eye1 = g_mv_eye1;
        memcpy(g_frames[index].eye0_view, g_mv_eye0_view, sizeof g_mv_eye0_view);
        g_frames[index].eye0 = g_mv_eye0;
    }
    g_mv_built = false;
}

/* d3d9_gles.cpp's Present of a multiview frame: each eye's picture is handed over in turn */
void bo1_vr_present_eye(int eye)
{
    if (g_backend_index < 0) return;
    host_xr_set_eye(eye, &g_frames[g_backend_index].views);
}

extern uint64_t g_bo1_backend_start_us;
void bo1_vr_backend_frame(int index)
{
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        g_bo1_backend_start_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    }
    static int calls;
    if (calls++ % 500 == 0) bo1_log("vr: backend frame data %d, eye %d", index, index >= 0 && index <= 1 ? g_frames[index].eye : -9);
    if (index < 0 || index > 1) return;
    g_backend_index = index;
    g_bo1_render_eye = g_frames[index].eye;
    g_bo1_mv_frame = g_frames[index].eye == 2;
    if (g_bo1_mv_frame) {
        g_bo1_mv_eye1 = g_frames[index].eye1;
        memcpy(g_bo1_mv_eye0_view, g_frames[index].eye0_view, sizeof g_bo1_mv_eye0_view);
        g_bo1_mv_eye0 = g_frames[index].eye0;
    } else {
        host_xr_set_eye(g_frames[index].eye, &g_frames[index].views);
    }
}

namespace {

struct Vec { float x, y, z; };

Vec rotate(const float q[4], Vec v)
{
    Vec u = {q[0], q[1], q[2]};
    Vec t = {2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z), 2.0f * (u.x * v.y - u.y * v.x)};
    return {v.x + q[3] * t.x + (u.y * t.z - u.z * t.y), v.y + q[3] * t.y + (u.z * t.x - u.x * t.z),
            v.z + q[3] * t.z + (u.x * t.y - u.y * t.x)};
}

} // namespace

static void eye_view(const refdef_s *refdef, GfxViewParms *view, int which);

/* R_SetViewParmsForScene: the scene's view as the current eye sees it. false for a flat frame.
   Multiview (eye 2): the left eye's, with the right eye's and a view covering both put aside. */
bool bo1_vr_view(const refdef_s *refdef, GfxViewParms *view)
{
    if (g_eye < 0 || g_eye > 2) return false;
    if (g_eye < 2) {
        eye_view(refdef, view, g_eye);
        return true;
    }
    GfxViewParms flat = *view;
    g_mv_eye1 = flat;
    eye_view(refdef, &g_mv_eye1, 1);
    eye_view(refdef, view, 0);
    if (g_debug_flags & (1u << 19)) g_mv_eye1 = *view; /* debug: the right eye drawn as the left */
    memcpy(g_mv_eye0_view, view->viewMatrix.m, sizeof g_mv_eye0_view);
    g_mv_eye0 = *view;
    /* culling for both: from between the eyes and a little behind (so each eye's frustum is inside
       it near the head too), with the union of their fields of view */
    g_mv_dpvs = flat;
    {
        const bo1_xr_eye &a = g_views.eye[0], &b = g_views.eye[1];
        bo1_xr_eye both = a;
        for (int i = 0; i < 3; i++) both.position[i] = (a.position[i] + b.position[i]) * 0.5f;
        both.fov[0] = fminf(a.fov[0], b.fov[0]);
        both.fov[1] = fmaxf(a.fov[1], b.fov[1]);
        both.fov[2] = fmaxf(a.fov[2], b.fov[2]);
        both.fov[3] = fminf(a.fov[3], b.fov[3]);
        float back[3] = {0.0f, 0.0f, 0.06f}; /* metres, the headset's +z is behind */
        Vec r = rotate(both.orientation, {back[0], back[1], back[2]});
        both.position[0] += r.x;
        both.position[1] += r.y;
        both.position[2] += r.z;
        bo1_xr_eye saved = g_views.eye[0];
        g_views.eye[0] = both;
        eye_view(refdef, &g_mv_dpvs, 0);
        g_views.eye[0] = saved;
    }
    g_mv_built = true;
    return true;
}

/* where an eye of the headset sees the scene from: the game camera turned into that eye's */
static void eye_view(const refdef_s *refdef, GfxViewParms *view, int which)
{
    const bo1_xr_eye &eye = g_views.eye[which];

    /* the game's camera, levelled: its heading only, less the hand's aim (in game) */
    float fx = refdef->viewaxis[0][0], fy = refdef->viewaxis[0][1];
    float len = sqrtf(fx * fx + fy * fy);
    if (len < 1e-4f) {
        fx = 1.0f;
        fy = 0.0f;
    } else {
        fx /= len;
        fy /= len;
    }
    if (g_aim_yaw != 0.0f) {
        float a = -g_aim_yaw * ((float)M_PI / 180.0f), c = cosf(a), s = sinf(a);
        float rx = fx * c - fy * s, ry = fx * s + fy * c;
        fx = rx;
        fy = ry;
    }
    const float forward[3] = {fx, fy, 0.0f}, left[3] = {-fy, fx, 0.0f}, up[3] = {0.0f, 0.0f, 1.0f};
    /* a headset vector (x right, y up, -z ahead) in the game's frame (x ahead, y left, z up) */
    auto to_game = [&](Vec v, float out[3]) {
        for (int i = 0; i < 3; i++) out[i] = forward[i] * -v.z + left[i] * -v.x + up[i] * v.y;
    };

    to_game(rotate(eye.orientation, {0.0f, 0.0f, -1.0f}), view->axis[0]);
    to_game(rotate(eye.orientation, {-1.0f, 0.0f, 0.0f}), view->axis[1]);
    to_game(rotate(eye.orientation, {0.0f, 1.0f, 0.0f}), view->axis[2]);
    float offset[3];
    to_game({eye.position[0], eye.position[1], eye.position[2]}, offset);
    for (int i = 0; i < 3; i++) view->origin[i] = refdef->vieworg[i] + offset[i] * UNITS_PER_METRE;
    view->origin[3] = 1.0f;
    MatrixForViewer(view->origin, view->axis, view->viewMatrix.m);

    /* InfinitePerspectiveMatrix with the eye's off-centre frustum: x_ndc = (x * m0 + z * m8) / z */
    float l = tanf(eye.fov[0]), r = tanf(eye.fov[1]), u = tanf(eye.fov[2]), d = tanf(eye.fov[3]);
    float (*m)[4] = view->projectionMatrix.m;
    memset(m, 0, sizeof view->projectionMatrix.m);
    m[0][0] = MAX_11BIT_FLT * 2.0f / (r - l);
    m[1][1] = MAX_11BIT_FLT * 2.0f / (u - d);
    m[2][0] = MAX_11BIT_FLT * -(r + l) / (r - l);
    m[2][1] = MAX_11BIT_FLT * -(u + d) / (u - d);
    m[2][2] = MAX_11BIT_FLT;
    m[2][3] = 1.0f;
    m[3][2] = -view->zNear * MAX_11BIT_FLT;
    R_SetupViewProjectionMatrices(view, 1);
    if (which == g_eye || (g_eye == 2 && which == 0)) {
        memcpy(g_eye_vp, view->viewProjectionMatrix.m, sizeof g_eye_vp);
        g_eye_vp_eye = which;
    }
}

/* ---- controls in game (milestone 6), as the Halo port has them -------------------------------

CL_CreateCmd (cl_input_mp.cpp), after the game's own mouse and gamepad moves: the body's heading is
the game's yaw less the last aim, so turns the game makes itself (spawning, scripts) carry over; the
right stick's x turns it (smoothly, SMOOTH_TURN degrees a second); the game's view becomes the right
controller's aim ray, turned by that heading; and the left stick's move, which the game took as
along that aim, is turned to be along where the head faces. */
extern "C" void host_input_gamepad(struct bo1_gamepad *state);
static const float SMOOTH_TURN = 120.0f;

static float yaw_of(const float f[3]) { return atan2f(-f[0], -f[2]) * (180.0f / (float)M_PI); } /* LOCAL, left positive */

void bo1_vr_input(int localClientNum, float *viewangles, usercmd_s *cmd)
{
    static int last_ms;
    int now = Sys_Milliseconds();
    float dt = last_ms ? (now - last_ms) * 0.001f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    last_ms = now;

    bool playing = CL_GetLocalClientConnectionState(localClientNum) == CA_ACTIVE;
    if (!playing) g_aim_yaw = 0.0f;
    if (!playing) g_two_hand = g_bo1_mask_lb = false;
    /* the hand's aim: the last tracked one while tracking drops out for a moment */
    static float aim_forward[3];
    static bool aim_seen;
    if (g_views.aim_valid) {
        memcpy(aim_forward, g_views.aim_forward, sizeof aim_forward);
        aim_seen = true;
        g_hand = {true, {g_views.aim_position[0], g_views.aim_position[1], g_views.aim_position[2]},
                  {g_views.aim_orientation[0], g_views.aim_orientation[1], g_views.aim_orientation[2],
                   g_views.aim_orientation[3]}};
    }
    g_left.valid = g_views.left_valid != 0;
    if (g_left.valid)
        g_left = {true, {g_views.left_position[0], g_views.left_position[1], g_views.left_position[2]},
                  {g_views.left_orientation[0], g_views.left_orientation[1], g_views.left_orientation[2],
                   g_views.left_orientation[3]}};
    g_bo1_vr_controls = playing && !Key_IsCatcherActive(localClientNum, 16) && g_views.valid && aim_seen;
    {
        /* debug: why the controllers do or don't play, every 2 s for a while */
        static int logged, last_log;
        if (logged < 40 && now - last_log > 2000) {
            logged++;
            last_log = now;
            bo1_log("vr input: state %d (active %d), ui catcher %d, any catcher %d, views %u, aim %u -> controls %d",
                    (int)CL_GetLocalClientConnectionState(localClientNum), (int)CA_ACTIVE,
                    Key_IsCatcherActive(localClientNum, 16) ? 1 : 0, Key_IsCatcherActive(localClientNum, -1) ? 1 : 0,
                    g_views.valid, g_views.aim_valid, g_bo1_vr_controls ? 1 : 0);
        }
    }
    if (!g_bo1_vr_controls) return;

    const float *delta = CG_GetLocalClientGlobals(localClientNum)->predictedPlayerState.delta_angles;
    float body = viewangles[1] - g_aim_yaw;
    bo1_gamepad pad;
    host_input_gamepad(&pad);
    float turn = pad.right_x / 32767.0f;
    if (fabsf(turn) > 0.2f) body -= turn * SMOOTH_TURN * dt;

    /* both hands: engage at the support grip with the left grip, release when it's let go */
    {
        bool lb = (pad.buttons & 0x0100) != 0;
        static bool lb_was;
        float apart = 0.0f, near_d = 1e9f;
        if (g_left.valid && g_hand.valid) {
            float d[3] = {g_left.position[0] - g_hand.position[0], g_left.position[1] - g_hand.position[1],
                          g_left.position[2] - g_hand.position[2]};
            apart = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (g_support_valid) {
                float s2[3] = {g_left.position[0] - g_support_local[0], g_left.position[1] - g_support_local[1],
                               g_left.position[2] - g_support_local[2]};
                near_d = sqrtf(s2[0] * s2[0] + s2[1] * s2[1] + s2[2] * s2[2]);
            }
        }
        bool near = near_d < 0.13f && apart > 0.08f && apart < 0.8f;
        if (!g_two_hand && lb && !lb_was && near) {
            g_two_hand = true;
            bo1_log("vr: both hands on the gun (support hand %.2f m from the grip)", near_d);
        }
        if (g_two_hand && (!lb || !g_left.valid || apart < 0.05f || apart > 0.9f)) {
            g_two_hand = false;
            bo1_log("vr: support hand let go");
        }
        lb_was = lb;
        g_bo1_mask_lb = g_two_hand || near;
    }
    float two_hand_dir[3];
    if (g_two_hand) {
        for (int i = 0; i < 3; i++) two_hand_dir[i] = g_left.position[i] - g_hand.position[i];
        float l = sqrtf(two_hand_dir[0] * two_hand_dir[0] + two_hand_dir[1] * two_hand_dir[1] + two_hand_dir[2] * two_hand_dir[2]);
        for (int i = 0; i < 3; i++) two_hand_dir[i] /= l;
    }
    const float *f = g_two_hand ? two_hand_dir : aim_forward;
    g_aim_yaw = yaw_of(f);
    float pitch = -asinf(f[1] < -1.0f ? -1.0f : f[1] > 1.0f ? 1.0f : f[1]) * (180.0f / (float)M_PI);
    if (pitch > 85.0f) pitch = 85.0f;
    if (pitch < -85.0f) pitch = -85.0f;
    viewangles[1] = AngleNormalize360(body + g_aim_yaw);
    viewangles[0] = pitch - delta[0];

    /* the move along the head's heading rather than the aim's */
    float head[3];
    {
        Vec h = rotate(g_views.eye[0].orientation, {0.0f, 0.0f, -1.0f});
        head[0] = h.x;
        head[1] = h.y;
        head[2] = h.z;
    }
    float a = (yaw_of(head) - g_aim_yaw) * ((float)M_PI / 180.0f), c = cosf(a), s = sinf(a);
    float fwd = cmd->forwardmove, right = cmd->rightmove;
    {
        /* debug: the stick and the move, a few times a second for a while */
        static int logged, last;
        if (logged < 60 && now - last > 300 && (pad.left_x || pad.left_y || fwd != 0.0f || right != 0.0f)) {
            logged++;
            last = now;
            bo1_log("vr move: stick %d %d, move in %d %d, head-aim %.0f deg, pm_flags 0x%x weaponstate %d", pad.left_x, pad.left_y,
                    (int)cmd->forwardmove, (int)cmd->rightmove, a * 180.0f / (float)M_PI,
                    CG_GetLocalClientGlobals(localClientNum)->predictedPlayerState.pm_flags,
                    CG_GetLocalClientGlobals(localClientNum)->predictedPlayerState.weaponstate);
        }
    }
    float f2 = fwd * c + right * s, r2 = right * c - fwd * s;
    auto clamp = [](float v) { return (char)(v > 127.0f ? 127 : v < -127.0f ? -127 : (int)v); };
    cmd->forwardmove = clamp(f2);
    cmd->rightmove = clamp(r2);
}

/* ---- the gun in the hand, as the Halo port has it ------------------------------------------

CG_AddPlayerWeapon (cg_weapons.cpp), once the first-person weapon's pose is worked out from the
camera (its offset, bob and sway): that pose is kept relative to the camera, and the camera swapped
for the right controller, moved back by where a gun's grip sits from the camera, so the grip is in
the hand and the barrel along its aim. bo1_vr_gun_x/y/z: that grip offset (inches; ahead, left, up). */
static const dvar_s *gun_offset[3];
static float g_gun_axes[3][3]; /* this frame's gun "camera" axes in the world (the right controller's) */
/* the grip offset's correction, measured (bo1_vr_left_hand): what puts the game's right hand, wherever
   this gun's animation has it, on the controller */
static float g_gun_correction[3];
static bool g_gun_axes_valid;

/* a controller's pose (LOCAL) in the game's world: axes (ahead, left, up) and place, as the eyes are drawn */
static bool controller_world(const refdef_s *refdef, const float position[3], const float orientation[4], float out[4][3])
{
    float fx = refdef->viewaxis[0][0], fy = refdef->viewaxis[0][1];
    float len = sqrtf(fx * fx + fy * fy);
    if (len < 1e-4f) return false;
    fx /= len;
    fy /= len;
    {
        float a = -g_aim_yaw * ((float)M_PI / 180.0f), c = cosf(a), s = sinf(a);
        float rx = fx * c - fy * s, ry = fx * s + fy * c;
        fx = rx;
        fy = ry;
    }
    const float forward[3] = {fx, fy, 0.0f}, left[3] = {-fy, fx, 0.0f}, up[3] = {0.0f, 0.0f, 1.0f};
    auto to_game = [&](Vec v, float o[3]) {
        for (int i = 0; i < 3; i++) o[i] = forward[i] * -v.z + left[i] * -v.x + up[i] * v.y;
    };
    to_game(rotate(orientation, {0.0f, 0.0f, -1.0f}), out[0]);
    to_game(rotate(orientation, {-1.0f, 0.0f, 0.0f}), out[1]);
    to_game(rotate(orientation, {0.0f, 1.0f, 0.0f}), out[2]);
    float offset[3];
    to_game({position[0], position[1], position[2]}, offset);
    for (int i = 0; i < 3; i++) out[3][i] = refdef->vieworg[i] + offset[i] * UNITS_PER_METRE;
    return true;
}

bool bo1_vr_hand_weapon(const refdef_s *refdef, float axis[4][3])
{
    if (!g_bo1_vr_controls || !g_hand.valid || !g_views.valid) return false;
    if (!gun_offset[0]) {
        gun_offset[0] = _Dvar_RegisterFloat("bo1_vr_gun_x", 14.0f, -60.0f, 60.0f, 0, "VR: the gun's grip ahead of the hand (inches)");
        gun_offset[1] = _Dvar_RegisterFloat("bo1_vr_gun_y", -5.0f, -60.0f, 60.0f, 0, "VR: the gun's grip left of the hand (inches)");
        gun_offset[2] = _Dvar_RegisterFloat("bo1_vr_gun_z", -6.0f, -60.0f, 60.0f, 0, "VR: the gun's grip above the hand (inches)");
    }

    /* the body's frame in the world, as bo1_vr_view draws the eyes from */
    float fx = refdef->viewaxis[0][0], fy = refdef->viewaxis[0][1];
    float len = sqrtf(fx * fx + fy * fy);
    if (len < 1e-4f) return false;
    fx /= len;
    fy /= len;
    {
        float a = -g_aim_yaw * ((float)M_PI / 180.0f), c = cosf(a), s = sinf(a);
        float rx = fx * c - fy * s, ry = fx * s + fy * c;
        fx = rx;
        fy = ry;
    }
    const float forward[3] = {fx, fy, 0.0f}, left[3] = {-fy, fx, 0.0f}, up[3] = {0.0f, 0.0f, 1.0f};
    auto to_game = [&](Vec v, float out[3]) {
        for (int i = 0; i < 3; i++) out[i] = forward[i] * -v.z + left[i] * -v.x + up[i] * v.y;
    };

    /* the hand as a camera: its axes and place in the world */
    float hand[4][3];
    to_game(rotate(g_hand.orientation, {0.0f, 0.0f, -1.0f}), hand[0]);
    to_game(rotate(g_hand.orientation, {-1.0f, 0.0f, 0.0f}), hand[1]);
    to_game(rotate(g_hand.orientation, {0.0f, 1.0f, 0.0f}), hand[2]);
    float offset[3];
    to_game({g_hand.position[0], g_hand.position[1], g_hand.position[2]}, offset);
    for (int i = 0; i < 3; i++) {
        hand[3][i] = refdef->vieworg[i] + offset[i] * UNITS_PER_METRE;
        for (int j = 0; j < 3; j++) hand[3][i] -= (gun_offset[j]->current.value + g_gun_correction[j]) * hand[j][i];
    }

    if (g_two_hand && g_left.valid) {
        /* along the line from the right hand to the left, its up the right hand's */
        float left_world[4][3];
        if (controller_world(refdef, g_left.position, g_left.orientation, left_world)) {
            float f2[3], up[3];
            float right_place[3];
            {
                /* the right controller's own place (hand[3] is moved back by the grip offset) */
                float rw[4][3];
                controller_world(refdef, g_hand.position, g_hand.orientation, rw);
                memcpy(right_place, rw[3], sizeof right_place);
            }
            for (int i = 0; i < 3; i++) f2[i] = left_world[3][i] - right_place[i];
            float l = sqrtf(f2[0] * f2[0] + f2[1] * f2[1] + f2[2] * f2[2]);
            if (l > 1e-3f) {
                for (int i = 0; i < 3; i++) f2[i] /= l;
                float u = hand[2][0] * f2[0] + hand[2][1] * f2[1] + hand[2][2] * f2[2];
                for (int i = 0; i < 3; i++) up[i] = hand[2][i] - f2[i] * u;
                float ul = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
                if (ul > 1e-3f) {
                    for (int i = 0; i < 3; i++) up[i] /= ul;
                    float lf[3] = {up[1] * f2[2] - up[2] * f2[1], up[2] * f2[0] - up[0] * f2[2], up[0] * f2[1] - up[1] * f2[0]};
                    /* the grip offset is along the hand's axes: keep the hand where it is */
                    float handPlace[3];
                    for (int i = 0; i < 3; i++)
                        handPlace[i] = hand[3][i] + (gun_offset[0]->current.value + g_gun_correction[0]) * hand[0][i] +
                                       (gun_offset[1]->current.value + g_gun_correction[1]) * hand[1][i] +
                                       (gun_offset[2]->current.value + g_gun_correction[2]) * hand[2][i];
                    memcpy(hand[0], f2, sizeof f2);
                    memcpy(hand[1], lf, sizeof lf);
                    memcpy(hand[2], up, sizeof up);
                    for (int i = 0; i < 3; i++)
                        hand[3][i] = handPlace[i] - (gun_offset[0]->current.value + g_gun_correction[0]) * hand[0][i] -
                                     (gun_offset[1]->current.value + g_gun_correction[1]) * hand[1][i] -
                                     (gun_offset[2]->current.value + g_gun_correction[2]) * hand[2][i];
                }
            }
        }
    }
    memcpy(g_gun_axes, hand, sizeof g_gun_axes);
    g_gun_axes_valid = true;

    /* the weapon's pose relative to the game's camera, then put on the hand */
    float rel[4][3];
    for (int r = 0; r < 3; r++)
        for (int j = 0; j < 3; j++)
            rel[r][j] = axis[r][0] * refdef->viewaxis[j][0] + axis[r][1] * refdef->viewaxis[j][1] + axis[r][2] * refdef->viewaxis[j][2];
    for (int j = 0; j < 3; j++) {
        float d[3] = {axis[3][0] - refdef->vieworg[0], axis[3][1] - refdef->vieworg[1], axis[3][2] - refdef->vieworg[2]};
        rel[3][j] = d[0] * refdef->viewaxis[j][0] + d[1] * refdef->viewaxis[j][1] + d[2] * refdef->viewaxis[j][2];
    }
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 3; i++) axis[r][i] = rel[r][0] * hand[0][i] + rel[r][1] * hand[1][i] + rel[r][2] * hand[2][i];
    for (int i = 0; i < 3; i++) axis[3][i] = hand[3][i] + rel[3][0] * hand[0][i] + rel[3][1] * hand[1][i] + rel[3][2] * hand[2][i];
    return true;
}

/* ---- the left hand on its own, as the Halo port's free hands ---------------------------------

The first-person model is one skeleton: both arms, the gun on the right hand. With the gun in the
right hand (bo1_vr_hand_weapon) the left arm would follow it; instead the left arm is moved as a
whole, from its root bone (the clavicle, else the shoulder), so that its wrist is at the left
controller and its forearm along the controller's aim. That's a bone control (DObjSetLocalTag): a
rotation and offset the skeleton applies to the bone. How the engine applies them (in which frame)
is measured once per model, with test controls, rather than assumed (calibrate below). */
namespace {

struct Frame {
    float m[3][3]; /* axes as rows, model space */
    float o[3];
};

float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool read_bone(const cpose_t *pose, DObj *obj, int bone, Frame &f)
{
    DObjAnimMat *mat = CG_DObjGetLocalBoneMatrix(pose, obj, bone);
    if (!mat) return false;
    ConvertQuatToMat(mat, f.m);
    f.o[0] = mat->trans[0];
    f.o[1] = mat->trans[1];
    f.o[2] = mat->trans[2];
    return true;
}

void clear_skel(DObj *obj)
{
    DObjLock(obj);
    DObjClearSkel(obj);
    DObjUnlock(obj);
}

/* a control on bone: quaternion (x, y, z, w) and offset */
void set_control(DObj *obj, int bone, const float q[4], const float t[3])
{
    int partBits[5];
    memset(partBits, 255, sizeof partBits);
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    DObjSetLocalTag(obj, partBits, (unsigned)bone, t, zero);
    DObjAnimMat *rt = DObjGetRotTransArray(obj);
    if (!rt) return;
    for (int i = 0; i < 4; i++) rt[bone].quat[i] = q[i];
}

/* rows matrix -> quaternion, the standard way (for R = rows^T acting on columns) */
void quat_of_rows(const float a[3][3], float q[4], bool conj)
{
    /* R[r][c] = a[c][r] */
    float r00 = a[0][0], r11 = a[1][1], r22 = a[2][2];
    float r21 = a[1][2], r12 = a[2][1], r02 = a[2][0], r20 = a[0][2], r10 = a[0][1], r01 = a[1][0];
    float tr = r00 + r11 + r22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (r21 - r12) / s;
        q[1] = (r02 - r20) / s;
        q[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q[3] = (r21 - r12) / s;
        q[0] = 0.25f * s;
        q[1] = (r01 + r10) / s;
        q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q[3] = (r02 - r20) / s;
        q[0] = (r01 + r10) / s;
        q[1] = 0.25f * s;
        q[2] = (r12 + r21) / s;
    } else {
        float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q[3] = (r10 - r01) / s;
        q[0] = (r02 + r20) / s;
        q[1] = (r12 + r21) / s;
        q[2] = 0.25f * s;
    }
    if (conj) q[0] = -q[0], q[1] = -q[1], q[2] = -q[2];
}

struct Arm {
    const XModel *model = NULL; /* the hands model the bones and calibration are for */
    int root = -1, parent = -1, elbow = -1, wrist = -1;
    bool ok = false;        /* calibrated and usable */
    bool transpose = false; /* how a parent-relative frame becomes the local quaternion */
    bool conj = false;
    float base[3];          /* added to the local offset by the engine (the bone's rest offset, if any) */
} g_arm;

int bone_named(DObj *obj, const char *name)
{
    unsigned int s = SL_FindString(name, SCRIPTINSTANCE_SERVER);
    if (!s) return -1;
    unsigned char index = 254;
    return DObjGetBoneIndex(obj, s, &index, -1) ? index : -1;
}

float frame_residual(const Frame &f, const float pred[3][3])
{
    float r = 0.0f;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r += fabsf(f.m[i][j] - pred[i][j]);
    return r;
}

/* DObjSetLocalTag replaces a bone's transform relative to its parent: rows (model space) and origin
   -> the local rotation (rows relative to the parent's) and offset (in the parent's axes) */
void local_of(const Frame &parent, const float rows[3][3], const float origin[3], float L[3][3], float t[3])
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) L[i][j] = g_arm.transpose ? dot3(rows[j], parent.m[i]) : dot3(rows[i], parent.m[j]);
    float d[3] = {origin[0] - parent.o[0], origin[1] - parent.o[1], origin[2] - parent.o[2]};
    for (int j = 0; j < 3; j++) t[j] = dot3(d, parent.m[j]) - g_arm.base[j];
}

/* finds the bones and works out how DObjSetLocalTag's values are taken; false when it can't */
bool calibrate(const cpose_t *pose, DObj *obj)
{
    g_arm.ok = false;
    XModel *hands = DObjGetModel(obj, 0);
    g_arm.root = bone_named(obj, "j_shoulder_le");
    g_arm.elbow = bone_named(obj, "j_elbow_le");
    g_arm.wrist = bone_named(obj, "j_wrist_le");
    g_arm.parent = -1;
    if (hands && g_arm.root >= hands->numRootBones && g_arm.root < hands->numBones)
        g_arm.parent = g_arm.root - hands->parentList[g_arm.root - hands->numRootBones];
    bo1_log("vr hand: model %s, shoulder %d (parent %d) elbow %d wrist %d", hands ? hands->name : "?", g_arm.root,
            g_arm.parent, g_arm.elbow, g_arm.wrist);
    if (g_arm.root < 0 || g_arm.parent < 0 || g_arm.elbow < 0 || g_arm.wrist < 0) return false;

    Frame parent, shoulder, test;
    clear_skel(obj);
    if (!read_bone(pose, obj, g_arm.parent, parent) || !read_bone(pose, obj, g_arm.root, shoulder)) return false;

    /* the shoulder set to where it already is, each way of turning a frame into a quaternion: the
       one that leaves it there is the engine's */
    float best = 1e9f;
    bool best_transpose = false, best_conj = false;
    memset(g_arm.base, 0, sizeof g_arm.base);
    for (int variant = 0; variant < 4; variant++) {
        g_arm.transpose = variant & 1;
        g_arm.conj = (variant & 2) != 0;
        float L[3][3], t[3], q[4];
        local_of(parent, shoulder.m, shoulder.o, L, t);
        quat_of_rows(L, q, g_arm.conj);
        clear_skel(obj);
        set_control(obj, g_arm.root, q, t);
        if (!read_bone(pose, obj, g_arm.root, test)) return false;
        float r = frame_residual(test, shoulder.m);
        bo1_log("vr hand: rotation variant %d residual %.4f", variant, r);
        if (r < best) best = r, best_transpose = g_arm.transpose, best_conj = g_arm.conj;
    }
    g_arm.transpose = best_transpose;
    g_arm.conj = best_conj;
    if (best > 0.05f) {
        clear_skel(obj);
        return false;
    }
    /* the offset: put where it should be, see where it lands; the difference is the engine's own */
    {
        float L[3][3], t[3], q[4];
        local_of(parent, shoulder.m, shoulder.o, L, t);
        quat_of_rows(L, q, g_arm.conj);
        clear_skel(obj);
        set_control(obj, g_arm.root, q, t);
        if (!read_bone(pose, obj, g_arm.root, test)) return false;
        float d[3] = {test.o[0] - shoulder.o[0], test.o[1] - shoulder.o[1], test.o[2] - shoulder.o[2]};
        for (int j = 0; j < 3; j++) g_arm.base[j] = dot3(d, parent.m[j]);
        bo1_log("vr hand: offset base %.2f %.2f %.2f", g_arm.base[0], g_arm.base[1], g_arm.base[2]);
        /* check */
        local_of(parent, shoulder.m, shoulder.o, L, t);
        clear_skel(obj);
        set_control(obj, g_arm.root, q, t);
        if (!read_bone(pose, obj, g_arm.root, test)) return false;
        float e[3] = {test.o[0] - shoulder.o[0], test.o[1] - shoulder.o[1], test.o[2] - shoulder.o[2]};
        bo1_log("vr hand: shoulder put back within %.3f in", sqrtf(dot3(e, e)));
        if (sqrtf(dot3(e, e)) > 0.5f) {
            clear_skel(obj);
            return false;
        }
    }
    clear_skel(obj);
    g_arm.ok = true;
    return true;
}

} // namespace

/* CG_AddPlayerWeapon, the first-person model's skeleton just cleared (CG_UpdateViewModelPose) */
void bo1_vr_left_hand(DObj *obj, const cpose_t *pose, const float model[4][3], const refdef_s *refdef)
{
    if (!obj || !g_bo1_vr_controls || !g_left.valid || !g_hand.valid) return;
    XModel *hands = DObjGetModel(obj, 0);
    if (hands != g_arm.model) {
        g_arm.model = hands;
        calibrate(pose, obj);
    }
    if (!g_arm.ok) return;

    float hand[4][3];
    if (!controller_world(refdef, g_left.position, g_left.orientation, hand)) return;
    /* into the space the skeleton is in: the world's, or the model's (told apart by where the model's
       first bone, tag_view, sits: on the model's origin in the world, or at zero) */
    static int world_space = -1;
    if (world_space < 0) {
        Frame view;
        clear_skel(obj);
        if (!read_bone(pose, obj, 0, view)) return;
        float dw[3] = {view.o[0] - model[3][0], view.o[1] - model[3][1], view.o[2] - model[3][2]};
        world_space = dot3(dw, dw) < dot3(view.o, view.o) ? 1 : 0;
        bo1_log("vr hand: tag_view at %.1f %.1f %.1f, model at %.1f %.1f %.1f: skeleton in %s space", view.o[0], view.o[1],
                view.o[2], model[3][0], model[3][1], model[3][2], world_space ? "world" : "model");
    }
    float aim[3], target[3];
    for (int j = 0; j < 3; j++) {
        if (world_space) {
            aim[j] = hand[0][j];
            target[j] = hand[3][j];
        } else {
            aim[j] = dot3(hand[0], model[j]);
            float d[3] = {hand[3][0] - model[3][0], hand[3][1] - model[3][1], hand[3][2] - model[3][2]};
            target[j] = dot3(d, model[j]);
        }
    }
    static const dvar_s *lhand[3];
    if (!lhand[0]) {
        lhand[0] = _Dvar_RegisterFloat("bo1_vr_lhand_x", -2.0f, -30.0f, 30.0f, 0, "VR: the left wrist ahead of the controller (inches)");
        lhand[1] = _Dvar_RegisterFloat("bo1_vr_lhand_y", 1.5f, -30.0f, 30.0f, 0, "VR: the left wrist left of the controller (inches)");
        lhand[2] = _Dvar_RegisterFloat("bo1_vr_lhand_z", 0.0f, -30.0f, 30.0f, 0, "VR: the left wrist above the controller (inches)");
    }
    /* the left controller's and the gun's axes in the skeleton's space */
    float laxes[3][3], gaxes[3][3];
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++) {
            laxes[k][j] = world_space ? hand[k][j] : dot3(hand[k], model[j]);
            gaxes[k][j] = world_space ? g_gun_axes[k][j] : dot3(g_gun_axes[k], model[j]);
        }
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++) target[j] += lhand[k]->current.value * laxes[k][j];

    Frame parent, root, elbow, wrist;
    clear_skel(obj);
    {
        /* where the game's animation puts the left hand on the gun (the support grip), LOCAL space,
           for the support hand to be held to (bo1_vr_input) */
        Frame grip;
        float lw[4][3];
        const float level[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        if (read_bone(pose, obj, g_arm.wrist, grip) && controller_world(refdef, zero, level, lw)) {
            float w[3];
            for (int i = 0; i < 3; i++)
                w[i] = world_space ? grip.o[i] : model[3][i] + grip.o[0] * model[0][i] + grip.o[1] * model[1][i] + grip.o[2] * model[2][i];
            float d[3] = {w[0] - lw[3][0], w[1] - lw[3][1], w[2] - lw[3][2]};
            /* LOCAL: x right (-left), y up, z back (-forward); world units to metres */
            g_support_local[0] = -dot3(d, lw[1]) / UNITS_PER_METRE;
            g_support_local[1] = dot3(d, lw[2]) / UNITS_PER_METRE;
            g_support_local[2] = -dot3(d, lw[0]) / UNITS_PER_METRE;
            g_support_valid = true;
        }
        /* the right hand: where the gun's animation has it against where the controller is (plus a
           little: the controller's aim point is ahead of the wrist, bo1_vr_rwrist_*); the difference
           along the gun's axes goes into the grip offset, a part each frame */
        static int right_wrist = -2;
        static const XModel *right_wrist_model;
        XModel *hm = DObjGetModel(obj, 0);
        if (right_wrist_model != hm) {
            right_wrist_model = hm;
            right_wrist = bone_named(obj, "j_wrist_ri");
        }
        static const dvar_s *rwrist[3];
        if (!rwrist[0]) {
            rwrist[0] = _Dvar_RegisterFloat("bo1_vr_rwrist_x", -2.5f, -30.0f, 30.0f, 0, "VR: the right wrist ahead of the controller (inches)");
            rwrist[1] = _Dvar_RegisterFloat("bo1_vr_rwrist_y", 0.0f, -30.0f, 30.0f, 0, "VR: the right wrist left of the controller (inches)");
            rwrist[2] = _Dvar_RegisterFloat("bo1_vr_rwrist_z", -1.0f, -30.0f, 30.0f, 0, "VR: the right wrist above the controller (inches)");
        }
        Frame rw;
        float cw[4][3];
        if (right_wrist >= 0 && g_gun_axes_valid && read_bone(pose, obj, right_wrist, rw) &&
            controller_world(refdef, g_hand.position, g_hand.orientation, cw)) {
            float w[3], err[3];
            for (int i = 0; i < 3; i++)
                w[i] = world_space ? rw.o[i] : model[3][i] + rw.o[0] * model[0][i] + rw.o[1] * model[1][i] + rw.o[2] * model[2][i];
            for (int i = 0; i < 3; i++) {
                float t = cw[3][i];
                for (int j = 0; j < 3; j++) t += rwrist[j]->current.value * g_gun_axes[j][i];
                err[i] = w[i] - t;
            }
            for (int j = 0; j < 3; j++) {
                float e = dot3(err, g_gun_axes[j]);
                g_gun_correction[j] += e * 0.25f;
                if (g_gun_correction[j] > 30.0f) g_gun_correction[j] = 30.0f;
                if (g_gun_correction[j] < -30.0f) g_gun_correction[j] = -30.0f;
            }
            static int logged, last;
            int now = Sys_Milliseconds();
            if (logged < 15 && now - last > 4000) {
                logged++;
                last = now;
                bo1_log("vr gun: right wrist off by %.1f in, grip correction %.1f %.1f %.1f", sqrtf(dot3(err, err)),
                        g_gun_correction[0], g_gun_correction[1], g_gun_correction[2]);
            }
        }
    }
    if (!read_bone(pose, obj, g_arm.parent, parent) || !read_bone(pose, obj, g_arm.root, root) ||
        !read_bone(pose, obj, g_arm.elbow, elbow) || !read_bone(pose, obj, g_arm.wrist, wrist))
        return;

    /* the turn that gives the hand, relative to the left controller, the pose it has relative to the
       gun (holding it): its frame relative to the gun's axes, put on the controller's */
    if (!g_gun_axes_valid) return;
    float want[3][3];
    for (int i = 0; i < 3; i++)
        for (int c = 0; c < 3; c++) {
            want[i][c] = 0.0f;
            for (int j = 0; j < 3; j++) want[i][c] += dot3(wrist.m[i], gaxes[j]) * laxes[j][c];
        }
    float R[3][3]; /* acting on column vectors: R v = sum_i want[i] (wrist.m[i] . v) */
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) R[r][c] = want[0][r] * wrist.m[0][c] + want[1][r] * wrist.m[1][c] + want[2][r] * wrist.m[2][c];
    (void)R;

    /* the arm, bent as a real one: the shoulder about where the player's own is (below and left of
       the head, bo1_vr_lshoulder_*), the wrist on the controller, the elbow down and out between them */
    static const dvar_s *shoulder_off[3];
    if (!shoulder_off[0]) {
        shoulder_off[0] = _Dvar_RegisterFloat("bo1_vr_lshoulder_x", -2.0f, -30.0f, 30.0f, 0, "VR: the left shoulder ahead of the head (inches)");
        shoulder_off[1] = _Dvar_RegisterFloat("bo1_vr_lshoulder_y", 7.0f, -30.0f, 30.0f, 0, "VR: the left shoulder left of the head (inches)");
        shoulder_off[2] = _Dvar_RegisterFloat("bo1_vr_lshoulder_z", -8.0f, -30.0f, 30.0f, 0, "VR: the left shoulder above the head (inches)");
    }
    float body[4][3];
    {
        const float head[3] = {(g_views.eye[0].position[0] + g_views.eye[1].position[0]) * 0.5f,
                               (g_views.eye[0].position[1] + g_views.eye[1].position[1]) * 0.5f,
                               (g_views.eye[0].position[2] + g_views.eye[1].position[2]) * 0.5f};
        const float level[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (!controller_world(refdef, head, level, body)) return;
    }
    float baxes[3][3], headp[3];
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++) baxes[k][j] = world_space ? body[k][j] : dot3(body[k], model[j]);
    for (int j = 0; j < 3; j++) {
        float d[3] = {body[3][0] - model[3][0], body[3][1] - model[3][1], body[3][2] - model[3][2]};
        headp[j] = world_space ? body[3][j] : dot3(d, model[j]);
    }
    auto sub = [](const float *x, const float *y, float *o) { o[0] = x[0] - y[0], o[1] = x[1] - y[1], o[2] = x[2] - y[2]; };
    auto norm = [](float *v) {
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
        return l;
    };
    /* the shortest turn from direction u to direction v, acting on column vectors */
    auto rot_between = [](const float u[3], const float v[3], float M[3][3]) {
        float ax[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        float sn = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), cs = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
        memset(M, 0, sizeof(float) * 9);
        if (sn < 1e-6f) {
            M[0][0] = M[1][1] = M[2][2] = 1.0f;
            return;
        }
        for (int i = 0; i < 3; i++) ax[i] /= sn;
        float t = 1.0f - cs, x = ax[0], y = ax[1], z = ax[2];
        float R0[3][3] = {{cs + x * x * t, x * y * t - z * sn, x * z * t + y * sn},
                          {y * x * t + z * sn, cs + y * y * t, y * z * t - x * sn},
                          {z * x * t - y * sn, z * y * t + x * sn, cs + z * z * t}};
        memcpy(M, R0, sizeof R0);
    };
    auto turn_rows = [](const float M[3][3], const float in[3][3], float out[3][3]) {
        for (int i = 0; i < 3; i++)
            for (int r = 0; r < 3; r++) out[i][r] = M[r][0] * in[i][0] + M[r][1] * in[i][1] + M[r][2] * in[i][2];
    };

    /* a bone's frame once its controlled ancestors move: its place relative to its parent kept */
    XModel *hands_model = DObjGetModel(obj, 0);
    auto parent_of = [&](int bone) {
        return bone >= hands_model->numRootBones ? bone - hands_model->parentList[bone - hands_model->numRootBones] : -1;
    };
    struct ArmControl {
        int bone;
        float q[4], t[3];
    };

    /* one arm (side 1 left, -1 right): the shoulder about where the player's own is, the wrist where
       wanted, the elbow down and out between; out: the three bones' controls */
    auto solve_arm = [&](int rootB, int elbowB, int wristB, const float target[3], const float want[3][3], float side,
                         ArmControl out[3]) -> bool {
        int parentB = parent_of(rootB);
        Frame parent, root, elbow, wrist;
        if (parentB < 0 || !read_bone(pose, obj, parentB, parent) || !read_bone(pose, obj, rootB, root) ||
            !read_bone(pose, obj, elbowB, elbow) || !read_bone(pose, obj, wristB, wrist))
            return false;
        float S[3];
        for (int j = 0; j < 3; j++)
            S[j] = headp[j] + shoulder_off[0]->current.value * baxes[0][j] + side * shoulder_off[1]->current.value * baxes[1][j] +
                   shoulder_off[2]->current.value * baxes[2][j];
    float se[3], ew[3];
    sub(elbow.o, root.o, se);
    sub(wrist.o, elbow.o, ew);
    float a = norm(se), b = norm(ew);
    if (a < 0.5f || b < 0.5f) return false;
    float dvec[3];
    sub(target, S, dvec);
    float dist = norm(dvec);
    if (dist > a + b - 0.05f) { /* out of reach: the shoulder comes along (it's out of sight) */
        float reach = a + b - 0.05f;
        for (int j = 0; j < 3; j++) S[j] = target[j] - dvec[j] * reach;
        dist = reach;
    }
    if (dist < 0.01f) return false;
    float cosA = (a * a + dist * dist - b * b) / (2.0f * a * dist);
    if (cosA > 1.0f) cosA = 1.0f;
    if (cosA < -1.0f) cosA = -1.0f;
    float sinA = sqrtf(1.0f - cosA * cosA);
    float pole[3];
    for (int j = 0; j < 3; j++) pole[j] = -baxes[2][j] + side * 0.5f * baxes[1][j] - 0.2f * baxes[0][j];
    float pd = dot3(pole, dvec);
    for (int j = 0; j < 3; j++) pole[j] -= dvec[j] * pd;
    if (norm(pole) < 1e-4f)
        for (int j = 0; j < 3; j++) pole[j] = side * baxes[1][j];
    float E[3];
    for (int j = 0; j < 3; j++) E[j] = S[j] + dvec[j] * a * cosA + pole[j] * a * sinA;

    /* the bones' new frames: upper arm and forearm turned onto their new directions, the hand as wanted */
    Frame nS, nE, nW;
    {
        float to[3], M[3][3];
        sub(E, S, to);
        norm(to);
        rot_between(se, to, M);
        turn_rows(M, root.m, nS.m);
        memcpy(nS.o, S, sizeof S);
        sub(target, E, to);
        norm(to);
        {
            /* the forearm rolls with the hand: the frame that keeps the animation's wrist-to-forearm
               turn for the wanted hand, swung onto the forearm's new direction. The shortest turn alone
               left all of the hand's twist at the wrist, which bent at odd angles */
            float rel[3][3], F[3][3];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) rel[r][c] = dot3(wrist.m[r], elbow.m[c]);
            for (int c = 0; c < 3; c++)
                for (int j = 0; j < 3; j++) F[c][j] = rel[0][c] * want[0][j] + rel[1][c] * want[1][j] + rel[2][c] * want[2][j];
            float along[3], local[3], axis[3];
            memcpy(along, ew, sizeof along);
            norm(along);
            for (int r = 0; r < 3; r++) local[r] = dot3(along, elbow.m[r]);
            for (int j = 0; j < 3; j++) axis[j] = local[0] * F[0][j] + local[1] * F[1][j] + local[2] * F[2][j];
            if (norm(axis) > 1e-4f) {
                rot_between(axis, to, M);
                turn_rows(M, F, nE.m);
            } else {
                rot_between(ew, to, M);
                turn_rows(M, elbow.m, nE.m);
            }
        }
        memcpy(nE.o, E, sizeof E);
        memcpy(nW.m, want, sizeof nW.m);
        memcpy(nW.o, target, sizeof nW.o);
    }

    auto new_frame = [&](int bone, Frame &out) -> bool {
        int chain[16], n = 0;
        int bcur = bone;
        while (bcur >= 0 && bcur != rootB && bcur != elbowB && bcur != wristB && n < 16) {
            chain[n++] = bcur;
            bcur = parent_of(bcur);
        }
        Frame base;
        if (bcur == rootB) base = nS;
        else if (bcur == elbowB) base = nE;
        else if (bcur == wristB) base = nW;
        else return read_bone(pose, obj, bone, out); /* not below the arm: unmoved */
        Frame oldBase;
        if (!read_bone(pose, obj, bcur, oldBase)) return false;
        for (int i = n - 1; i >= 0; i--) {
            Frame cur;
            if (!read_bone(pose, obj, chain[i], cur)) return false;
            Frame next;
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    next.m[r][c] = dot3(cur.m[r], oldBase.m[0]) * base.m[0][c] + dot3(cur.m[r], oldBase.m[1]) * base.m[1][c] +
                                   dot3(cur.m[r], oldBase.m[2]) * base.m[2][c];
            float d[3];
            sub(cur.o, oldBase.o, d);
            for (int c = 0; c < 3; c++)
                next.o[c] = base.o[c] + dot3(d, oldBase.m[0]) * base.m[0][c] + dot3(d, oldBase.m[1]) * base.m[1][c] +
                            dot3(d, oldBase.m[2]) * base.m[2][c];
            oldBase = cur;
            base = next;
        }
        out = base;
        return true;
    };


        Frame pe, pw;
        if (!new_frame(parent_of(elbowB), pe) || !new_frame(parent_of(wristB), pw)) return false;
        float L[3][3];
        out[0].bone = rootB;
        local_of(parent, nS.m, nS.o, L, out[0].t);
        quat_of_rows(L, out[0].q, g_arm.conj);
        out[1].bone = elbowB;
        local_of(pe, nE.m, nE.o, L, out[1].t);
        quat_of_rows(L, out[1].q, g_arm.conj);
        out[2].bone = wristB;
        local_of(pw, nW.m, nW.o, L, out[2].t);
        quat_of_rows(L, out[2].q, g_arm.conj);
        return true;
    };

    /* the left arm to the left controller; the right one keeps its hand where it is (on the gun, in
       the right controller's hand) with its shoulder and elbow where a body would have them */
    ArmControl arm[6];
    int arm_count = 0;
    if (!g_two_hand && solve_arm(g_arm.root, g_arm.elbow, g_arm.wrist, target, want, 1.0f, arm)) arm_count = 3;
    {
        static int right_bones[3] = {-2, -2, -2};
        static const XModel *right_model;
        if (right_model != hands_model) {
            right_model = hands_model;
            right_bones[0] = bone_named(obj, "j_shoulder_ri");
            right_bones[1] = bone_named(obj, "j_elbow_ri");
            right_bones[2] = bone_named(obj, "j_wrist_ri");
        }
        Frame rw;
        if (right_bones[0] >= 0 && right_bones[1] >= 0 && right_bones[2] >= 0 && read_bone(pose, obj, right_bones[2], rw) &&
            solve_arm(right_bones[0], right_bones[1], right_bones[2], rw.o, rw.m, -1.0f, arm + arm_count))
            arm_count += 3;
    }
    if (!arm_count) return;
    /* the fingers: open, as in the model's rest pose, rather than the grip of the gun's foregrip */
    struct FingerControl {
        int bone;
        float q[4], t[3];
    } fingers[24];
    int finger_count = 0;
    {
        static int finger_bones[24], finger_total = -1;
        static const XModel *finger_model;
        XModel *hands = DObjGetModel(obj, 0);
        if (finger_total < 0 || finger_model != hands) {
            finger_model = hands;
            finger_total = 0;
            static const char *names[] = {"j_index_le_0", "j_index_le_1", "j_index_le_2", "j_index_le_3", "j_mid_le_0",
                                          "j_mid_le_1",   "j_mid_le_2",   "j_mid_le_3",   "j_ring_le_0",  "j_ring_le_1",
                                          "j_ring_le_2",  "j_ring_le_3",  "j_pinky_le_0", "j_pinky_le_1", "j_pinky_le_2",
                                          "j_pinky_le_3", "j_thumb_le_1", "j_thumb_le_2", "j_thumb_le_3"};
            for (const char *n : names) {
                int b = bone_named(obj, n);
                if (b >= hands->numRootBones && finger_total < 24) finger_bones[finger_total++] = b;
            }
        }
        for (int f = 0; f < (g_two_hand ? 0 : finger_total); f++) {
            int b = finger_bones[f], pb = b - hands->parentList[b - hands->numRootBones];
            Frame fb, fp;
            if (!read_bone(pose, obj, b, fb) || !read_bone(pose, obj, pb, fp)) continue;
            FingerControl &fc = fingers[finger_count++];
            fc.bone = b;
            const short *rq = hands->localQuats + (b - hands->numRootBones) * 4;
            for (int k = 0; k < 4; k++) fc.q[k] = rq[k] * (1.0f / 32767.0f);
            float d[3] = {fb.o[0] - fp.o[0], fb.o[1] - fp.o[1], fb.o[2] - fp.o[2]};
            for (int j = 0; j < 3; j++) fc.t[j] = dot3(d, fp.m[j]);
        }
    }

    clear_skel(obj);
    for (int k = 0; k < arm_count; k++) set_control(obj, arm[k].bone, arm[k].q, arm[k].t);
    for (int f = 0; f < finger_count; f++) set_control(obj, fingers[f].bone, fingers[f].q, fingers[f].t);

    {
        /* debug: how close the wrist lands, now and then */
        static int checks, last;
        int now = Sys_Milliseconds();
        if (checks < 20 && now - last > 3000) {
            checks++;
            last = now;
            Frame w;
            if (read_bone(pose, obj, g_arm.wrist, w)) {
                float e[3] = {w.o[0] - target[0], w.o[1] - target[1], w.o[2] - target[2]};
                bo1_log("vr hand: wrist misses its target by %.1f in", sqrtf(dot3(e, e)));
            }
            clear_skel(obj);
            for (int k = 0; k < arm_count; k++) set_control(obj, arm[k].bone, arm[k].q, arm[k].t);
            for (int f = 0; f < finger_count; f++) set_control(obj, fingers[f].bone, fingers[f].q, fingers[f].t);
        }
    }
}

/* ---- the crosshair in 3D ------------------------------------------------------------------------

The game's own crosshair (CG_DrawCrosshair), drawn into each eye's picture rather than on the 2D
panel, where that eye sees the point the shots would hit (a trace along the aim from where the game
fires), so it sits on the target at its depth. Its place is in the crosshair's units: +-320 across
and +-240 down the whole view, as CG_CalcCrosshairPosition gives them. */
/* VR: the shots leave from the hand - its offset from the eye in the world, valid while no wall is
   between them (bo1_vr_crosshair; used by g_weapon.cpp and cg_weapons.cpp) */
bool g_bo1_vr_muzzle_valid;
float g_bo1_vr_muzzle_offset[3];
float g_bo1_crosshair_eye1[2]; /* multiview: the right eye's crosshair less the left's, clip space (cg_draw_reticles.cpp) */

bool bo1_vr_crosshair(const refdef_s *refdef, int passEntityNum, float *x, float *y)
{
    if (!g_bo1_vr_controls || g_eye < 0 || g_eye > 2 || !g_views.valid) return false;
    float start[3], end[3];
    for (int i = 0; i < 3; i++) start[i] = refdef->vieworg[i];
    /* the shots leave from the hand (g_weapon.cpp, cg_weapons.cpp): the hand's place less the eye's,
       unless a wall is between them (then from the eye, as the game does) */
    {
        float cw[4][3];
        g_bo1_vr_muzzle_valid = false;
        if (g_hand.valid && controller_world(refdef, g_hand.position, g_hand.orientation, cw)) {
            trace_t wall;
            CG_LocationalTrace(&wall, refdef->vieworg, cw[3], passEntityNum, 0x280B001, 0, 0);
            if (wall.fraction >= 1.0f) {
                for (int i = 0; i < 3; i++) {
                    g_bo1_vr_muzzle_offset[i] = cw[3][i] - refdef->vieworg[i];
                    start[i] = cw[3][i];
                }
                g_bo1_vr_muzzle_valid = true;
            }
        }
    }
    for (int i = 0; i < 3; i++) end[i] = start[i] + refdef->viewaxis[0][i] * 15000.0f;
    trace_t trace;
    CG_LocationalTrace(&trace, start, end, passEntityNum, 0x280B001, 0, 0);
    float hit[4] = {start[0] + (end[0] - start[0]) * trace.fraction, start[1] + (end[1] - start[1]) * trace.fraction,
                    start[2] + (end[2] - start[2]) * trace.fraction, 1.0f};
    /* where this eye sees it: the eye's place and axes in the world (as bo1_vr_view draws it), then
       its off-centre frustum (the game draws its 2D before the eye's view is set up, so this is worked
       out here rather than taken from the view) */
    auto ndc = [&](int which, float *nx, float *ny) {
        const bo1_xr_eye &eye = g_views.eye[which];
        float e[4][3];
        if (!controller_world(refdef, eye.position, eye.orientation, e)) return false;
        float d[3] = {hit[0] - e[3][0], hit[1] - e[3][1], hit[2] - e[3][2]};
        float ahead = dot3(d, e[0]), right = -dot3(d, e[1]), up = dot3(d, e[2]);
        if (ahead < 1.0f) return false;
        float l = tanf(eye.fov[0]), r = tanf(eye.fov[1]), u = tanf(eye.fov[2]), dn = tanf(eye.fov[3]);
        *nx = (2.0f * (right / ahead) - (r + l)) / (r - l);
        *ny = (2.0f * (up / ahead) - (u + dn)) / (u - dn);
        return true;
    };
    float nx, ny;
    if (!ndc(g_eye == 2 ? 0 : g_eye, &nx, &ny)) return false;
    g_bo1_crosshair_eye1[0] = g_bo1_crosshair_eye1[1] = 0.0f;
    if (g_eye == 2) {
        /* one picture for both eyes: drawn where the left eye sees it, moved in the right eye */
        float nx1, ny1;
        if (!ndc(1, &nx1, &ny1)) return false;
        g_bo1_crosshair_eye1[0] = nx1 - nx;
        g_bo1_crosshair_eye1[1] = ny1 - ny;
    }
    /* into the 2D's units as the crosshair is drawn (centred, through scrPlaceView): the eye's half
       width or height in pixels over the virtual-to-real scale */
    const ScreenPlacement &place = scrPlaceView[0];
    if (place.scaleVirtualToReal[0] <= 0.0f || place.scaleVirtualToReal[1] <= 0.0f) return false;
    *x = nx * (place.realViewportSize[0] * 0.5f) / place.scaleVirtualToReal[0];
    *y = -ny * (place.realViewportSize[1] * 0.5f) / place.scaleVirtualToReal[1];
    return true;
}
