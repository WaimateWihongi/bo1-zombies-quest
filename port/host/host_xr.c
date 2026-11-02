/* OpenXR: the game in the headset (milestone 5, first step: a virtual screen).
 *
 * The instance is made on the main thread at start-up (host_xr_initialize); the session, the
 * swapchain and every frame belong to the guest's render thread, where the GL context is current
 * (host_gl.c calls host_xr_start once the context exists, then host_xr_frame_begin/_end around
 * each of the game's Presents). The game's finished picture (its back buffer, through its gamma
 * ramp) is drawn into the swapchain, which is shown as a quad layer: a big screen floating in front
 * of the player in the LOCAL space, so it stays put as the head moves. xrWaitFrame paces the game
 * to the headset's refresh rate.
 *
 * Stereo (step 3): the game draws each frame once per eye (quest_vr.cpp). It fetches the eyes'
 * poses for a pair from host_xr_views (its main thread), and before presenting an eye's frame the
 * render thread says which eye it is and which poses it was drawn with (host_xr_set_eye). The left
 * eye's Present begins the OpenXR frame, the right eye's ends it with a projection layer of the two
 * images at exactly those poses. A frame that is no eye (-1) still goes to the virtual screen.
 *
 * Controllers (Touch): the aim ray of the hand that last pulled its trigger points. On the virtual
 * screen the pointer is where the ray meets it; in stereo it is the point POINT_DISTANCE along the
 * ray as the left eye's picture shows it. Either way it becomes the game's pointer (the same events
 * as the flat panel's laser, host_input.c) and a dot is drawn there. The trigger or A/X clicks, B/Y
 * is Back, a thumbstick scrolls (menus step up and down with it) and the left menu button is Escape.
 *
 * The panel (stereo): the game's 2D (menus, HUD) is drawn into its own transparent image rather than
 * into the eyes' pictures (d3d9_gles.cpp), and the left eye's Present copies it into the screen
 * swapchain (host_xr_panel_begin/_end), shown as a quad layer over the world: PANEL_DISTANCE ahead,
 * turning to follow the head once it looks more than PANEL_FOLLOW_DEG away, and moving with it. The
 * pointer is where the aim ray meets the panel, and its dot is drawn on the panel - except while the
 * game says a menu on an in-world screen has focus (host_xr_pointer_mode 1, the front end's TV), when
 * the pointer is the point along the ray as the left eye sees it, as before.
 *
 * In game (host_xr_pointer_mode 2: playing, no menu open) there is no pointer: the controllers are a
 * gamepad (host_input_xr_pad), laid out like the Halo port's - triggers fire and aim down the sights,
 * A jump, B crouch, X reload/use, Y switch weapon, grips throw grenades (right lethal, left tactical),
 * stick clicks sprint (left) and melee (right). The left stick moves, the right stick's x turns (the
 * game side, quest_vr.cpp, which also aims with the right controller: its aim ray goes with each pair's
 * eyes, host_xr_views). */
#include "host.h"
#include "android_native_app_glue.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <android/keycodes.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

/* the virtual screen: 16:9, 2.4 m wide, 2 m ahead at eye height when the session started */
#define SCREEN_DISTANCE 2.0f
#define SCREEN_WIDTH_M 2.4f
#define SCREEN_HEIGHT_M (SCREEN_WIDTH_M * 9.0f / 16.0f)
#define SCREEN_PIXELS_W 1920
#define SCREEN_PIXELS_H 1080
#define MAX_IMAGES 8
/* in stereo, how far along the aim ray the pointer is (about where the front end's TV is) */
#define POINT_DISTANCE 1.5f
/* the panel: the game's 2D in stereo */
#define PANEL_DISTANCE 2.0f
#define PANEL_WIDTH_M 1.9f
#define PANEL_HEIGHT_M (PANEL_WIDTH_M * 9.0f / 16.0f)
#define PANEL_FOLLOW_DEG 30.0f

static XrInstance g_instance = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static XrSession g_session = XR_NULL_HANDLE;
static XrSpace g_local = XR_NULL_HANDLE;
static XrSwapchain g_swapchain = XR_NULL_HANDLE;
static int g_srgb;
static uint32_t g_image_count;
static XrSwapchainImageOpenGLESKHR g_images[MAX_IMAGES];
static GLuint g_fbos[MAX_IMAGES];
static int g_running, g_in_frame, g_acquired, g_has_performance_settings;
static XrFrameState g_frame;

/* stereo: a swapchain per eye, the same size as the game's back buffer */
static XrSwapchain g_eye_swapchains[2];
static uint32_t g_eye_image_counts[2];
static XrSwapchainImageOpenGLESKHR g_eye_images[2][MAX_IMAGES];
static GLuint g_eye_fbos[2][MAX_IMAGES];
static XrSwapchain g_acquired_swapchain;
static int g_present_eye = -1;                 /* render thread: the eye of the frame being presented */
static struct bo1_xr_views g_present_views;    /* and the poses it was drawn with */
static struct bo1_xr_eye g_layer_eyes[2];      /* this OpenXR frame's eye images, as drawn */
static int g_layer_has[2], g_quad_has, g_stereo;
static pthread_mutex_t g_time_lock = PTHREAD_MUTEX_INITIALIZER;
static XrTime g_next_display_time;             /* when the pair the main thread builds next will show */

static XrActionSet g_action_set = XR_NULL_HANDLE;
static XrAction g_aim, g_select, g_back, g_scroll, g_pause;
static XrAction g_trigger, g_grip, g_stick_click, g_button_ax, g_button_by; /* play */
static int g_pad_neutral = 1;
static XrPath g_hands[2];
static XrSpace g_aim_spaces[2];
static int g_hand = 1; /* the pointing hand: right until the left pulls its trigger */
static float g_cursor_u, g_cursor_v;
static int g_pointer_mode;           /* 1: an in-world screen's menu (point along the ray), 0: the panel */
static int g_panel_has, g_panel_acquired, g_panel_set, g_panel_following;
static float g_panel_yaw;            /* radians, 0 = facing -z */
static XrVector3f g_panel_centre;
static XrQuaternionf g_panel_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
static int g_cursor_visible, g_buttons, g_back_down, g_pause_down;

static int failed(XrResult r, const char *what)
{
    if (XR_SUCCEEDED(r)) return 0;
    char text[XR_MAX_RESULT_STRING_SIZE] = "";
    if (g_instance) xrResultToString(g_instance, r, text);
    host_error("xr: %s failed: %s (%d)", what, text, (int)r);
    return 1;
}

/* ---- start-up --------------------------------------------------------------------------------- */

int host_xr_initialize(struct android_app *app)
{
    PFN_xrInitializeLoaderKHR initialize_loader = NULL;
    if (failed(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initialize_loader),
               "xrGetInstanceProcAddr(xrInitializeLoaderKHR)"))
        return -1;
    XrLoaderInitInfoAndroidKHR loader = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader.applicationVM = app->activity->vm;
    loader.applicationContext = app->activity->clazz;
    if (failed(initialize_loader((const XrLoaderInitInfoBaseHeaderKHR *)&loader), "xrInitializeLoaderKHR")) return -1;

    const char *extensions[3] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    uint32_t extension_count = 2;
    {
        /* optional: performance levels (the render thread is CPU-bound, in stereo even more) */
        XrExtensionProperties available[128];
        uint32_t n = 0;
        for (int i = 0; i < 128; i++) available[i] = (XrExtensionProperties){XR_TYPE_EXTENSION_PROPERTIES};
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 128, &n, available)))
            for (uint32_t i = 0; i < n; i++)
                if (!strcmp(available[i].extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME)) {
                    extensions[extension_count++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
                    g_has_performance_settings = 1;
                }
    }
    XrInstanceCreateInfoAndroidKHR android = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = app->activity->vm;
    android.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo info = {XR_TYPE_INSTANCE_CREATE_INFO};
    info.next = &android;
    strcpy(info.applicationInfo.applicationName, "BO1 Zombies VR");
    info.applicationInfo.applicationVersion = 1;
    strcpy(info.applicationInfo.engineName, "bo1-quest");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = extension_count;
    info.enabledExtensionNames = extensions;
    if (failed(xrCreateInstance(&info, &g_instance), "xrCreateInstance")) {
        g_instance = XR_NULL_HANDLE;
        return -1;
    }
    XrInstanceProperties properties = {XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &properties)))
        host_info("xr: runtime %s %u.%u.%u", properties.runtimeName, XR_VERSION_MAJOR(properties.runtimeVersion),
                  XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
    XrSystemGetInfo system = {XR_TYPE_SYSTEM_GET_INFO};
    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (failed(xrGetSystem(g_instance, &system, &g_system), "xrGetSystem")) {
        xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
        return -1;
    }
    return 0;
}

int host_xr_available(void) { return g_instance != XR_NULL_HANDLE && g_system != XR_NULL_SYSTEM_ID; }
int host_xr_active(void) { return g_session != XR_NULL_HANDLE; }

static XrPath path(const char *text)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, text, &p);
    return p;
}

static XrAction make_action(const char *name, const char *label, XrActionType type)
{
    XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
    strcpy(info.actionName, name);
    strcpy(info.localizedActionName, label);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = g_hands;
    XrAction action = XR_NULL_HANDLE;
    failed(xrCreateAction(g_action_set, &info, &action), name);
    return action;
}

static int make_actions(void)
{
    g_hands[0] = path("/user/hand/left");
    g_hands[1] = path("/user/hand/right");
    XrActionSetCreateInfo set = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(set.actionSetName, "menu");
    strcpy(set.localizedActionSetName, "Menus");
    if (failed(xrCreateActionSet(g_instance, &set, &g_action_set), "xrCreateActionSet")) return -1;
    g_aim = make_action("aim", "Point", XR_ACTION_TYPE_POSE_INPUT);
    g_select = make_action("select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_back = make_action("back", "Back", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_scroll = make_action("scroll", "Scroll", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_pause = make_action("pause", "Pause", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_trigger = make_action("trigger", "Fire / aim", XR_ACTION_TYPE_FLOAT_INPUT);
    g_grip = make_action("grip", "Grenades", XR_ACTION_TYPE_FLOAT_INPUT);
    g_stick_click = make_action("stick_click", "Sprint / melee", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_button_ax = make_action("button_ax", "Jump / reload", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_button_by = make_action("button_by", "Crouch / switch weapon", XR_ACTION_TYPE_BOOLEAN_INPUT);

    struct { XrAction action; const char *path; } touch[] = {
        {g_aim, "/user/hand/left/input/aim/pose"},          {g_aim, "/user/hand/right/input/aim/pose"},
        {g_select, "/user/hand/left/input/trigger/value"},  {g_select, "/user/hand/right/input/trigger/value"},
        {g_select, "/user/hand/left/input/x/click"},        {g_select, "/user/hand/right/input/a/click"},
        {g_back, "/user/hand/left/input/y/click"},          {g_back, "/user/hand/right/input/b/click"},
        {g_scroll, "/user/hand/left/input/thumbstick"},     {g_scroll, "/user/hand/right/input/thumbstick"},
        {g_pause, "/user/hand/left/input/menu/click"},
        {g_trigger, "/user/hand/left/input/trigger/value"}, {g_trigger, "/user/hand/right/input/trigger/value"},
        {g_grip, "/user/hand/left/input/squeeze/value"},    {g_grip, "/user/hand/right/input/squeeze/value"},
        {g_stick_click, "/user/hand/left/input/thumbstick/click"},
        {g_stick_click, "/user/hand/right/input/thumbstick/click"},
        {g_button_ax, "/user/hand/left/input/x/click"},     {g_button_ax, "/user/hand/right/input/a/click"},
        {g_button_by, "/user/hand/left/input/y/click"},     {g_button_by, "/user/hand/right/input/b/click"},
    };
    XrActionSuggestedBinding bindings[sizeof touch / sizeof touch[0]];
    for (size_t i = 0; i < sizeof touch / sizeof touch[0]; i++) {
        bindings[i].action = touch[i].action;
        bindings[i].binding = path(touch[i].path);
    }
    XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = sizeof bindings / sizeof bindings[0];
    suggested.suggestedBindings = bindings;
    if (failed(xrSuggestInteractionProfileBindings(g_instance, &suggested), "xrSuggestInteractionProfileBindings"))
        return -1;
    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_action_set;
    if (failed(xrAttachSessionActionSets(g_session, &attach), "xrAttachSessionActionSets")) return -1;
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo space = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space.action = g_aim;
        space.subactionPath = g_hands[h];
        space.poseInActionSpace.orientation.w = 1.0f;
        if (failed(xrCreateActionSpace(g_session, &space, &g_aim_spaces[h]), "xrCreateActionSpace")) return -1;
    }
    return 0;
}

static int create_swapchain(int64_t format, XrSwapchain *swapchain, XrSwapchainImageOpenGLESKHR *images,
                            uint32_t *count, GLuint *fbos, const char *what)
{
    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = SCREEN_PIXELS_W;
    info.height = SCREEN_PIXELS_H;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (failed(xrCreateSwapchain(g_session, &info, swapchain), "xrCreateSwapchain")) return -1;
    for (int i = 0; i < MAX_IMAGES; i++) images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    if (failed(xrEnumerateSwapchainImages(*swapchain, MAX_IMAGES, count, (XrSwapchainImageBaseHeader *)images),
               "xrEnumerateSwapchainImages"))
        return -1;
    glGenFramebuffers((GLsizei)*count, fbos);
    for (uint32_t i = 0; i < *count; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[i].image, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) host_error("xr: %s framebuffer %u incomplete (%#x)", what, i, status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}

static int make_swapchain(void)
{
    int64_t formats[256];
    uint32_t count = 0;
    if (failed(xrEnumerateSwapchainFormats(g_session, 0, &count, NULL), "xrEnumerateSwapchainFormats")) return -1;
    if (count > 256) count = 256;
    if (failed(xrEnumerateSwapchainFormats(g_session, count, &count, formats), "xrEnumerateSwapchainFormats")) return -1;
    int64_t format = GL_RGBA8;
    for (uint32_t i = 0; i < count; i++)
        if (formats[i] == GL_SRGB8_ALPHA8) format = GL_SRGB8_ALPHA8;
    g_srgb = format == GL_SRGB8_ALPHA8;
    if (create_swapchain(format, &g_swapchain, g_images, &g_image_count, g_fbos, "screen") ||
        create_swapchain(format, &g_eye_swapchains[0], g_eye_images[0], &g_eye_image_counts[0], g_eye_fbos[0], "left eye") ||
        create_swapchain(format, &g_eye_swapchains[1], g_eye_images[1], &g_eye_image_counts[1], g_eye_fbos[1], "right eye"))
        return -1;
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t n = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                       &n, views)))
        host_info("xr: the runtime suggests %ux%u per eye", views[0].recommendedImageRectWidth,
                  views[0].recommendedImageRectHeight);
    host_info("xr: screen and eye swapchains %dx%d, %s", SCREEN_PIXELS_W, SCREEN_PIXELS_H, g_srgb ? "sRGB" : "RGBA8");
    return 0;
}

/* render thread, the GL context current: the session and everything in it */
int host_xr_start(void *display, void *config, void *context)
{
    if (!host_xr_available()) return -1;
    PFN_xrGetOpenGLESGraphicsRequirementsKHR requirements_fn = NULL;
    xrGetInstanceProcAddr(g_instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&requirements_fn);
    XrGraphicsRequirementsOpenGLESKHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!requirements_fn || failed(requirements_fn(g_instance, g_system, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR"))
        return -1;
    XrGraphicsBindingOpenGLESAndroidKHR binding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = (EGLDisplay)display;
    binding.config = (EGLConfig)config;
    binding.context = (EGLContext)context;
    XrSessionCreateInfo info = {XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = g_system;
    if (failed(xrCreateSession(g_instance, &info, &g_session), "xrCreateSession")) {
        g_session = XR_NULL_HANDLE;
        return -1;
    }
    XrReferenceSpaceCreateInfo space = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1.0f;
    if (failed(xrCreateReferenceSpace(g_session, &space, &g_local), "xrCreateReferenceSpace") || make_swapchain() ||
        make_actions()) {
        xrDestroySession(g_session);
        g_session = XR_NULL_HANDLE;
        return -1;
    }
    host_info("xr: session made");
    return 0;
}

/* ---- events ----------------------------------------------------------------------------------- */

/* the headset's top CPU and GPU levels (XR_EXT_performance_settings), once the session runs */
static void set_performance_levels(void)
{
    if (!g_has_performance_settings) return;
    PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = NULL;
    if (XR_FAILED(xrGetInstanceProcAddr(g_instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&set_level)) ||
        !set_level)
        return;
    XrResult cpu = set_level(g_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
    XrResult gpu = set_level(g_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
    host_info("xr: performance levels boost: CPU %d, GPU %d", (int)cpu, (int)gpu);
}

static void poll_events(void)
{
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrSessionState state = ((XrEventDataSessionStateChanged *)&event)->state;
            host_info("xr: session state %d", (int)state);
            if (state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (!failed(xrBeginSession(g_session, &begin), "xrBeginSession")) {
                    g_running = 1;
                    set_performance_levels();
                }
            } else if (state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                g_running = 0;
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = 0;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_running = 0;
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        event.next = NULL;
    }
}

/* ---- controllers ------------------------------------------------------------------------------ */

static int get_bool(XrAction action, int hand, int *changed)
{
    XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = g_hands[hand];
    XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(g_session, &info, &state)) || !state.isActive) {
        if (changed) *changed = 0;
        return 0;
    }
    if (changed) *changed = state.changedSinceLastSync;
    return state.currentState;
}

/* v rotated by the unit quaternion q */
static XrVector3f rotate(XrQuaternionf q, XrVector3f v)
{
    XrVector3f u = {q.x, q.y, q.z};
    XrVector3f t = {2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z), 2.0f * (u.x * v.y - u.y * v.x)};
    XrVector3f r = {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                    v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
    return r;
}

/* where point P (LOCAL space) shows in an eye's picture: u across, v down, 0..1; 0 when outside */
static int project(const struct bo1_xr_eye *eye, XrVector3f point, float *u, float *v)
{
    XrQuaternionf inverse = {-eye->orientation[0], -eye->orientation[1], -eye->orientation[2], eye->orientation[3]};
    XrVector3f d = {point.x - eye->position[0], point.y - eye->position[1], point.z - eye->position[2]};
    XrVector3f local = rotate(inverse, d);
    if (local.z > -1e-3f) return 0;
    float x = local.x / -local.z, y = local.y / -local.z;
    float l = tanf(eye->fov[0]), r = tanf(eye->fov[1]), up = tanf(eye->fov[2]), down = tanf(eye->fov[3]);
    *u = (x - l) / (r - l);
    *v = (up - y) / (up - down);
    return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

static XrVector3f g_point; /* stereo: the pointed-at point */
static int locate_views(XrTime time, XrView views[2]);

/* the panel follows the head: its heading once the head turns PANEL_FOLLOW_DEG away (until nearly
   centred again), its place with the head's */
static void update_panel(XrTime time)
{
    XrView views[2];
    if (!locate_views(time, views)) return;
    XrVector3f head = {(views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                       (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                       (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
    XrVector3f ahead = rotate(views[0].pose.orientation, (XrVector3f){0.0f, 0.0f, -1.0f});
    if (ahead.x * ahead.x + ahead.z * ahead.z < 1e-4f) return; /* looking straight up or down */
    float yaw = atan2f(-ahead.x, -ahead.z);
    if (!g_panel_set) g_panel_yaw = yaw;
    float off = yaw - g_panel_yaw;
    while (off > (float)M_PI) off -= 2.0f * (float)M_PI;
    while (off < -(float)M_PI) off += 2.0f * (float)M_PI;
    float deg = fabsf(off) * 180.0f / (float)M_PI;
    if (deg > PANEL_FOLLOW_DEG) g_panel_following = 1;
    if (g_panel_following) {
        g_panel_yaw += off * 0.08f;
        if (deg < 2.0f) g_panel_following = 0;
    }
    g_panel_orientation = (XrQuaternionf){0.0f, sinf(g_panel_yaw * 0.5f), 0.0f, cosf(g_panel_yaw * 0.5f)};
    XrVector3f target = {head.x - sinf(g_panel_yaw) * PANEL_DISTANCE, head.y - 0.1f,
                         head.z - cosf(g_panel_yaw) * PANEL_DISTANCE};
    if (!g_panel_set) {
        g_panel_centre = target;
        g_panel_set = 1;
    }
    g_panel_centre.x += (target.x - g_panel_centre.x) * 0.08f;
    g_panel_centre.y += (target.y - g_panel_centre.y) * 0.08f;
    g_panel_centre.z += (target.z - g_panel_centre.z) * 0.08f;
}

/* where a ray (LOCAL space) meets the panel: u across, v down; 0 when it misses */
static int hit_panel(XrVector3f p, XrVector3f d, float *u, float *v)
{
    XrQuaternionf inverse = {-g_panel_orientation.x, -g_panel_orientation.y, -g_panel_orientation.z,
                             g_panel_orientation.w};
    XrVector3f lp = rotate(inverse, (XrVector3f){p.x - g_panel_centre.x, p.y - g_panel_centre.y, p.z - g_panel_centre.z});
    XrVector3f ld = rotate(inverse, d);
    if (ld.z > -1e-4f || lp.z <= 0.0f) return 0; /* not heading into its front */
    float t = -lp.z / ld.z;
    *u = (lp.x + t * ld.x) / PANEL_WIDTH_M + 0.5f;
    *v = 0.5f - (lp.y + t * ld.y) / PANEL_HEIGHT_M;
    return *u >= 0.0f && *u <= 1.0f && *v >= 0.0f && *v <= 1.0f;
}

static float get_float(XrAction action, int hand)
{
    XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = g_hands[hand];
    XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(g_session, &info, &state)) || !state.isActive) return 0.0f;
    return state.currentState;
}

static int16_t axis(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

/* in game: the controllers as an Xbox pad (XInput bits) */
static void update_pad(void)
{
    struct bo1_gamepad pad;
    memset(&pad, 0, sizeof pad);
    for (int h = 0; h < 2; h++) {
        XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = g_scroll;
        info.subactionPath = g_hands[h];
        XrActionStateVector2f stick = {XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &info, &stick)) && stick.isActive) {
            if (h == 0) pad.left_x = axis(stick.currentState.x), pad.left_y = axis(stick.currentState.y);
            else pad.right_x = axis(stick.currentState.x), pad.right_y = axis(stick.currentState.y);
        }
    }
    pad.left_trigger = (uint8_t)(get_float(g_trigger, 0) * 255.0f);
    pad.right_trigger = (uint8_t)(get_float(g_trigger, 1) * 255.0f);
    if (get_float(g_grip, 0) > 0.6f) pad.buttons |= 0x0100;  /* left shoulder: tactical grenade */
    if (get_float(g_grip, 1) > 0.6f) pad.buttons |= 0x0200;  /* right shoulder: lethal grenade */
    if (get_bool(g_stick_click, 0, NULL)) pad.buttons |= 0x0040; /* left thumb: sprint */
    if (get_bool(g_stick_click, 1, NULL)) pad.buttons |= 0x0080; /* right thumb: melee */
    if (get_bool(g_button_ax, 1, NULL)) pad.buttons |= 0x1000;   /* A: jump */
    if (get_bool(g_button_by, 1, NULL)) pad.buttons |= 0x2000;   /* B: crouch */
    if (get_bool(g_button_ax, 0, NULL)) pad.buttons |= 0x4000;   /* X: reload / use */
    if (get_bool(g_button_by, 0, NULL)) pad.buttons |= 0x8000;   /* Y: switch weapon */
    host_input_xr_pad(&pad);
    g_pad_neutral = 0;
}

static void update_controllers(XrTime time)
{
    XrActiveActionSet active = {g_action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(g_session, &sync))) return;

    /* the hand that pulls its trigger becomes the pointer */
    int select[2], changed[2];
    for (int h = 0; h < 2; h++) {
        select[h] = get_bool(g_select, h, &changed[h]);
        if (select[h] && changed[h]) g_hand = h;
    }

    /* in game: a gamepad, no pointer (the menu button still pauses) */
    if (g_pointer_mode == 2) {
        g_cursor_visible = 0;
        if (g_buttons) host_input_buttons(0);
        g_buttons = 0;
        update_pad();
        int pause = get_bool(g_pause, 0, NULL);
        if (pause != g_pause_down) host_input_key(AKEYCODE_ESCAPE, pause);
        g_pause_down = pause;
        return;
    }
    if (!g_pad_neutral) {
        struct bo1_gamepad neutral;
        memset(&neutral, 0, sizeof neutral);
        host_input_xr_pad(&neutral);
        g_pad_neutral = 1;
    }

    /* where the pointing hand's aim ray meets the screen (the plane z = -SCREEN_DISTANCE, facing +z) */
    g_cursor_visible = 0;
    XrSpaceLocation location = {XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_SUCCEEDED(xrLocateSpace(g_aim_spaces[g_hand], g_local, time, &location)) &&
        (location.locationFlags & valid) == valid) {
        XrVector3f p = location.pose.position, forward = {0.0f, 0.0f, -1.0f};
        XrVector3f d = rotate(location.pose.orientation, forward);
        if (g_stereo && !g_pointer_mode) {
            float u, v;
            if (hit_panel(p, d, &u, &v)) {
                g_cursor_visible = 1;
                if (u != g_cursor_u || v != g_cursor_v) host_input_pointer(u, v);
                g_cursor_u = u;
                g_cursor_v = v;
            }
        } else if (g_stereo) {
            /* the point along the ray, where the left eye's latest picture shows it */
            g_point = (XrVector3f){p.x + d.x * POINT_DISTANCE, p.y + d.y * POINT_DISTANCE, p.z + d.z * POINT_DISTANCE};
            float u, v;
            if (project(&g_present_views.eye[0], g_point, &u, &v)) {
                g_cursor_visible = 1;
                if (u != g_cursor_u || v != g_cursor_v) host_input_pointer(u, v);
                g_cursor_u = u;
                g_cursor_v = v;
            }
        } else if (d.z < -1e-4f) {
            float t = (-SCREEN_DISTANCE - p.z) / d.z;
            float u = (p.x + t * d.x) / SCREEN_WIDTH_M + 0.5f, v = 0.5f - (p.y + t * d.y) / SCREEN_HEIGHT_M;
            if (t > 0.0f && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                g_cursor_visible = 1;
                if (u != g_cursor_u || v != g_cursor_v) host_input_pointer(u, v);
                g_cursor_u = u;
                g_cursor_v = v;
            }
        }
    }

    int buttons = select[g_hand] ? 1 : 0;
    if (buttons != g_buttons) host_input_buttons(buttons);
    g_buttons = buttons;

    int back = get_bool(g_back, 0, NULL) || get_bool(g_back, 1, NULL);
    if (back != g_back_down) host_input_key(AKEYCODE_BACK, back);
    g_back_down = back;
    int pause = get_bool(g_pause, 0, NULL);
    if (pause != g_pause_down) host_input_key(AKEYCODE_ESCAPE, pause);
    g_pause_down = pause;

    /* a thumbstick held up or down: the same stream of wheel fractions the flat panel gets */
    for (int h = 0; h < 2; h++) {
        XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = g_scroll;
        info.subactionPath = g_hands[h];
        XrActionStateVector2f stick = {XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &info, &stick)) && stick.isActive &&
            fabsf(stick.currentState.y) > 0.5f)
            host_input_scroll(stick.currentState.y * 0.35f);
    }
}

/* ---- frames ----------------------------------------------------------------------------------- */

static int locate_views(XrTime time, XrView views[2])
{
    XrViewLocateInfo info = {XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = time;
    info.space = g_local;
    XrViewState state = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    views[0] = (XrView){XR_TYPE_VIEW};
    views[1] = (XrView){XR_TYPE_VIEW};
    if (XR_FAILED(xrLocateViews(g_session, &info, &state, 2, &n, views)) || n != 2) return 0;
    return (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
}

/* The game's main thread, before drawing a stereo pair: the eyes as they are expected to be when the
   pair is shown. 0 (and views->valid 0) when the headset isn't showing us. */
uint32_t host_xr_views(uint32_t guest_views)
{
    struct bo1_xr_views *out = GUEST_PTR(struct bo1_xr_views *, guest_views);
    memset(out, 0, sizeof *out);
    if (g_session == XR_NULL_HANDLE || !g_running) return 0;
    pthread_mutex_lock(&g_time_lock);
    XrTime time = g_next_display_time;
    pthread_mutex_unlock(&g_time_lock);
    XrView views[2];
    if (!time || !locate_views(time, views)) return 0;
    for (int e = 0; e < 2; e++) {
        struct bo1_xr_eye *eye = &out->eye[e];
        XrPosef pose = views[e].pose;
        eye->orientation[0] = pose.orientation.x;
        eye->orientation[1] = pose.orientation.y;
        eye->orientation[2] = pose.orientation.z;
        eye->orientation[3] = pose.orientation.w;
        eye->position[0] = pose.position.x;
        eye->position[1] = pose.position.y;
        eye->position[2] = pose.position.z;
        eye->fov[0] = views[e].fov.angleLeft;
        eye->fov[1] = views[e].fov.angleRight;
        eye->fov[2] = views[e].fov.angleUp;
        eye->fov[3] = views[e].fov.angleDown;
    }
    XrSpaceLocation hand = {XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_SUCCEEDED(xrLocateSpace(g_aim_spaces[1], g_local, time, &hand)) && (hand.locationFlags & tracked) == tracked) {
        XrVector3f f = rotate(hand.pose.orientation, (XrVector3f){0.0f, 0.0f, -1.0f});
        out->aim_valid = 1;
        out->aim_position[0] = hand.pose.position.x;
        out->aim_position[1] = hand.pose.position.y;
        out->aim_position[2] = hand.pose.position.z;
        out->aim_forward[0] = f.x;
        out->aim_forward[1] = f.y;
        out->aim_forward[2] = f.z;
        out->aim_orientation[0] = hand.pose.orientation.x;
        out->aim_orientation[1] = hand.pose.orientation.y;
        out->aim_orientation[2] = hand.pose.orientation.z;
        out->aim_orientation[3] = hand.pose.orientation.w;
    }
    XrSpaceLocation left = {XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(g_aim_spaces[0], g_local, time, &left)) && (left.locationFlags & tracked) == tracked) {
        out->left_valid = 1;
        out->left_position[0] = left.pose.position.x;
        out->left_position[1] = left.pose.position.y;
        out->left_position[2] = left.pose.position.z;
        out->left_orientation[0] = left.pose.orientation.x;
        out->left_orientation[1] = left.pose.orientation.y;
        out->left_orientation[2] = left.pose.orientation.z;
        out->left_orientation[3] = left.pose.orientation.w;
    }
    out->valid = 1;
    return 1;
}

/* The render thread, before presenting a frame: which eye it is (-1: none, the virtual screen) and
   the poses it was drawn with. */
void host_xr_set_eye(int32_t eye, uint32_t guest_views)
{
    g_present_eye = eye >= 0 && eye <= 1 ? eye : -1;
    if (g_present_eye >= 0) {
        g_present_views = *GUEST_PTR(const struct bo1_xr_views *, guest_views);
        g_stereo = 1;
    } else {
        g_stereo = 0;
    }
}

static void end_frame(void);

/* Starts a frame's picture: the framebuffer of its image (the virtual screen's, or the eye's), its
   size and whether it is sRGB (size[0..2]). The left eye (or a flat frame) begins an OpenXR frame,
   the right eye draws into the one the left began. 0 when the session isn't running. */
uint32_t host_xr_frame_begin(uint32_t size[3])
{
    int eye = g_present_eye;
    if (eye <= 0 || !g_in_frame) {
        if (g_in_frame) end_frame(); /* a pair left unfinished */
        poll_events();
        if (!g_running) return 0;
        XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
        g_frame = (XrFrameState){XR_TYPE_FRAME_STATE};
        if (failed(xrWaitFrame(g_session, &wait, &g_frame), "xrWaitFrame")) return 0;
        pthread_mutex_lock(&g_time_lock);
        g_next_display_time = g_frame.predictedDisplayTime + g_frame.predictedDisplayPeriod;
        pthread_mutex_unlock(&g_time_lock);
        XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
        if (failed(xrBeginFrame(g_session, &begin), "xrBeginFrame")) return 0;
        g_in_frame = 1;
        g_layer_has[0] = g_layer_has[1] = g_quad_has = g_panel_has = 0;
        update_panel(g_frame.predictedDisplayTime);
        update_controllers(g_frame.predictedDisplayTime);
    }

    XrSwapchain swapchain = eye < 0 ? g_swapchain : g_eye_swapchains[eye];
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (failed(xrAcquireSwapchainImage(swapchain, &acquire, &index), "xrAcquireSwapchainImage")) return 0;
    XrSwapchainImageWaitInfo image_wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    image_wait.timeout = XR_INFINITE_DURATION;
    if (failed(xrWaitSwapchainImage(swapchain, &image_wait), "xrWaitSwapchainImage")) return 0;
    g_acquired = 1;
    g_acquired_swapchain = swapchain;
    size[0] = SCREEN_PIXELS_W;
    size[1] = SCREEN_PIXELS_H;
    size[2] = (uint32_t)g_srgb;
    return eye < 0 ? g_fbos[index] : g_eye_fbos[eye][index];
}

/* the pointer's dot, drawn into the image just drawn (bound) with scissored clears: on the virtual
   screen where the ray meets it, in an eye's picture where that eye sees the pointed-at point */
static void draw_cursor(int eye)
{
    float u = g_cursor_u, v = g_cursor_v;
    if (!g_cursor_visible) return;
    if (eye >= 0 && !g_pointer_mode) return; /* it's on the panel */
    if (eye >= 0 && !project(&g_present_views.eye[eye], g_point, &u, &v)) return;
    int x = (int)(u * SCREEN_PIXELS_W), y = (int)((1.0f - v) * SCREEN_PIXELS_H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x - 7, y - 7, 14, 14);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(x - 5, y - 5, 10, 10);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

/* submits the OpenXR frame: both eyes' pictures as a projection layer at the poses they were drawn
   with, or the virtual screen as a quad */
static void end_frame(void)
{
    XrCompositionLayerProjectionView views[2];
    XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad panel = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t count = 0;
    if (g_layer_has[0] && g_layer_has[1]) {
        for (int e = 0; e < 2; e++) {
            const struct bo1_xr_eye *eye = &g_layer_eyes[e];
            views[e] = (XrCompositionLayerProjectionView){XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[e].pose.orientation = (XrQuaternionf){eye->orientation[0], eye->orientation[1], eye->orientation[2],
                                                        eye->orientation[3]};
            views[e].pose.position = (XrVector3f){eye->position[0], eye->position[1], eye->position[2]};
            views[e].fov = (XrFovf){eye->fov[0], eye->fov[1], eye->fov[2], eye->fov[3]};
            views[e].subImage.swapchain = g_eye_swapchains[e];
            views[e].subImage.imageRect.extent.width = SCREEN_PIXELS_W;
            views[e].subImage.imageRect.extent.height = SCREEN_PIXELS_H;
        }
        projection.space = g_local;
        projection.viewCount = 2;
        projection.views = views;
        layers[count++] = (const XrCompositionLayerBaseHeader *)&projection;
        if (g_panel_has) {
            /* premultiplied alpha: the 2D's coverage lets the world show around and through it */
            panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            panel.space = g_local;
            panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            panel.subImage.swapchain = g_swapchain;
            panel.subImage.imageRect.extent.width = SCREEN_PIXELS_W;
            panel.subImage.imageRect.extent.height = SCREEN_PIXELS_H;
            panel.pose.orientation = g_panel_orientation;
            panel.pose.position = g_panel_centre;
            panel.size.width = PANEL_WIDTH_M;
            panel.size.height = PANEL_HEIGHT_M;
            layers[count++] = (const XrCompositionLayerBaseHeader *)&panel;
        }
    } else if (g_quad_has) {
        quad.space = g_local;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_swapchain;
        quad.subImage.imageRect.extent.width = SCREEN_PIXELS_W;
        quad.subImage.imageRect.extent.height = SCREEN_PIXELS_H;
        quad.pose.orientation.w = 1.0f;
        quad.pose.position.z = -SCREEN_DISTANCE;
        quad.size.width = SCREEN_WIDTH_M;
        quad.size.height = SCREEN_HEIGHT_M;
        layers[count++] = (const XrCompositionLayerBaseHeader *)&quad;
    }
    XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
    end.displayTime = g_frame.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = g_frame.shouldRender ? count : 0;
    end.layers = layers;
    failed(xrEndFrame(g_session, &end), "xrEndFrame");
    g_in_frame = 0;
}

/* a picture is drawn: the left eye waits for the right, which (like a flat frame) ends the frame */
void host_xr_frame_end(void)
{
    if (!g_in_frame) return;
    int eye = g_present_eye;
    if (g_acquired) {
        draw_cursor(eye);
        XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_acquired_swapchain, &release);
        g_acquired = 0;
        if (eye >= 0) {
            g_layer_has[eye] = 1;
            g_layer_eyes[eye] = g_present_views.eye[eye];
        } else {
            g_quad_has = 1;
        }
    }
    if (eye == 0) return;
    end_frame();
}

/* ---- the panel -------------------------------------------------------------------------------- */

/* The render thread, during a stereo frame: the framebuffer of the panel's next image and its size
   and sRGB-ness (guest uint32_t[3]); 0xFFFFFFFF when there is no panel to draw this frame. */
uint32_t host_xr_panel_begin(uint32_t guest_size)
{
    uint32_t *size = GUEST_PTR(uint32_t *, guest_size);
    if (!g_in_frame || !g_stereo || g_panel_acquired || g_panel_has) return 0xFFFFFFFFu;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (failed(xrAcquireSwapchainImage(g_swapchain, &acquire, &index), "xrAcquireSwapchainImage (panel)"))
        return 0xFFFFFFFFu;
    XrSwapchainImageWaitInfo image_wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    image_wait.timeout = XR_INFINITE_DURATION;
    if (failed(xrWaitSwapchainImage(g_swapchain, &image_wait), "xrWaitSwapchainImage (panel)")) return 0xFFFFFFFFu;
    g_panel_acquired = 1;
    size[0] = SCREEN_PIXELS_W;
    size[1] = SCREEN_PIXELS_H;
    size[2] = (uint32_t)g_srgb;
    return g_fbos[index];
}

/* the panel's image is drawn (and bound): the pointer's dot on it, then it's handed back */
void host_xr_panel_end(void)
{
    if (!g_panel_acquired) return;
    if (!g_pointer_mode) draw_cursor(-1);
    XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_swapchain, &release);
    g_panel_acquired = 0;
    g_panel_has = 1;
}

/* the game, each frame: 1 while a menu on an in-world screen has focus (the pointer goes along the
   ray into the world), 2 while playing with no menu open (the controllers are a gamepad), 0 for the
   panel */
void host_xr_pointer_mode(int32_t mode) { g_pointer_mode = mode >= 0 && mode <= 2 ? mode : 0; }
