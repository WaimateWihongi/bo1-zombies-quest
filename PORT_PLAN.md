# BO1 Zombies → Meta Quest 3 (VR) port plan

Source: `src/` (clone of MisaDev4/bo1-zombies-decompiled, Win32/x86/D3D9).
Reference: moistman42069/HaloCE-DecompRemix-VR-PC-Android (host/guest design, OpenXR host, VR game code).
The APK never contains Black Ops files; the player copies `zone/`, `main/` etc. from their own PC install.

## Architecture (borrowed from the Halo port)

```
APK
├── lib/arm64-v8a/libmain.so        64-bit HOST (NDK, bionic): SDL3, EGL/GLES3, OpenXR, AAudio, file paths
├── lib/arm64-v8a/libSDL3.so, libopenxr_loader.so
└── assets/bo1_guest.elf            32-bit GUEST (arm64_32 ILP32): the BO1 engine + musl + runtime
```

- **Why a 32-bit guest:** fastfiles (`zone/*.ff`) are loaded by `database/` straight into structs that hold
  32-bit pointers (`XAssetHeader` is 4 bytes). Keeping ILP32 keeps every on-disk layout valid without
  rewriting the asset loaders.
- **Guest compiler:** NDK clang on Windows, `--target=arm64_32-apple-watchos -U__APPLE__ -fno-define-target-os-macros
  -nostdinc`, `-S`, then a Mach-O→ELF assembly conversion (like Halo's `android_asm_convert.py`), then assembled
  with `--target=aarch64-linux-android`. Verified working with NDK 27.1 (32-bit pointer loads). WSL not required.
- **Guest libc:** musl 1.2.5 with an `arm64_32` arch dir; syscalls forwarded to the host (`host_syscall`).
  C++: libc++ headers (header-only use), `-fno-exceptions -fno-rtti` (no dynamic_cast/typeid in the engine;
  try/catch only in `qcommon/threads.cpp` and CubeMapGenLib).
- **Host↔guest calls:** an import list → generated 3-instruction trampolines; pointers pass through unchanged
  because all guest memory is mapped below 4 GB in the same process.

## Engine work (guest side)

| Area | Windows today | Quest replacement |
|---|---|---|
| Platform (`win32/`) | Win32 window, threads, timers, files, sockets | POSIX/musl + SDL3 shim (Halo's `guest_sdl.c` approach) |
| Renderer (`gfx_d3d/`, ~140k lines) | D3D9 device | **`d3d9_gles`**: an IDirect3DDevice9 implementation on GLES 3.2. Shaders in fastfiles are D3D9 SM3 bytecode → translated at load with MojoShader (zlib licence) to GLSL ES |
| Sound (`sound/`) | DirectSound / XAudio2 | SDL3 audio stream (AAudio) |
| Input (`win_input`, `win_gamepad`) | DirectInput/XInput | XR actions → virtual pad + VR-specific inputs |
| Video (`binklib/`) | Bink | Skip intros at first; later decode or stub |
| Online (`live/`, `DW/`, `steam/`) | DemonWare/Steam | Stubbed: solo zombies offline |
| x86 asm (7 game files) | `__asm` | Portable C equivalents |

## VR work

- Host: OpenXR (`XR_KHR_opengl_es_enable`), eye swapchains, HUD/menu quad layers, action bindings for Touch Plus.
- Game: stereo render of the player view (two refdefs, off-centre frustums from XR FOVs), head-relative camera,
  weapon viewmodel attached to the controller grip pose, firing along the controller aim ray,
  snap/smooth turn, laser pointer for menus, HUD on a head-locked quad, haptics on fire.

## Status (2026-10-31)

- **Milestone 2 (done, verified on a real Quest 3 and the emulator):** `python port/build.py && ninja -C build`
  makes `build/bo1-quest.apk`. The host reserves the free low 4 GB, loads `bo1_guest.elf` at the top of the
  largest free gap (relocating its pointer words), runs its `main` on a stack below 4 GB, and the smoke test
  passes all 11 checks (ILP32, malloc below 4 GB, stdio, clocks, 4 threads, TLS, setjmp, files, C++).
- **Milestone 1 (done): the whole engine compiles for the guest (1192 objects, 0 failures):** `ninja -C build engine`.
  The game's own D3D9 renderer (`gfx_d3d/`) now compiles against `port/guest/compat/d3d9.h` (the D3D9
  interfaces as abstract classes) and most of `win32/` compiles against the compat Win32 headers.
  Not built (see `ENGINE_REPLACED` in port/build.py): `win_main`, `win_net`, `win_wndproc`, `win_syscon`,
  `win_input` (the Quest platform layer replaces them), voice chat, Steam/DemonWare, Bink and cutscenes
  (`r_cinematic.cpp`), the XAudio2 driver, libvpx recording, HLSL compiling (`r_material_load_obj.cpp`).
- **Milestone 3 (done 2026-10-31, on a real Quest 3): Kino der Toten runs as a headless zombies server.**
  `bo1_guest.elf` is the whole engine + `port/guest/platform` (12 MB). Default command line (no
  `bo1_args.txt` in the data folder): `+set bo1_zombies 1 +set bo1_headless 1 +set QuitOnError 1 +devmap zombie_theater`.
  Result: code zones, 214 scripts and `zombie_theater.ff` load in 3.9 s, `sv_running 1`, frames advance;
  ~8% CPU, 505 MB RSS, 24 threads. The engine's own log is `main/console_mp.log` in the data folder (logcat's
  256 KB buffer overflows; `adb logcat -G 16M` helps).
  - The original `win_main`, `win_net`, `win_input` and `r_material_load_obj` now build on the Win32 layer;
    `win_wndproc`/`win_syscon` are replaced by `platform/win32_user.cpp`.
  - `platform/`: `win32_kernel.cpp` (handles, events/mutexes/semaphores, critical sections in place, SRW locks,
    threads, files incl. ReadFileEx + alertable SleepEx APCs, find, time, VirtualAlloc, memory status),
    `win32_user.cpp` (one fixed window, no message queue, monitors, clipboard, console -> logcat),
    `win32_crt.cpp` (MSVC text-mode fopen: CRLF -> LF, needed for localization.txt), `d3d9_null.cpp`
    (null device with real lock memory incl. DXT), `stubs.cpp` (Steam offline, voice, sound off via
    SD_Xaudio2CanInit, cutscenes, NvAPI, D3DX, CubeMapGen), `quest_main.cpp` (main -> WinMain), `libcxx_support.cpp`.
  - Host: `host_path.c` translates every guest path syscall ('\' -> '/', case-insensitive per component).
  - Data on the Quest: `/sdcard/Android/data/com.bo1zombies.quest/files/{zone/Common,zone/English,main}` from
    the user's Steam install (not `main/video`). `adb push` can't create folders there (scoped storage):
    make them with `adb shell mkdir -p` first, then push files.
  - Known: each console line reaches logcat twice; when the guest exits Android relaunches the activity once
    (the host should finish the activity instead); "Redundant asset" spam from the SP+MP code zones.

- **Milestone 4 (done 2026-10-31, on the Quest 3): the title screen renders, 72 fps, in a 1600x900 panel.**
  Default launch is now the game as Play.bat starts it (`+set bo1_zombies 1 +set logfile 2 +set net_ip 127.0.0.1`).
  ~1,100 draws a frame on the title scene; ~9 MB of streamed vertex/index data a frame.
  - `platform/d3d9_gles.cpp`: IDirect3DDevice9 on GLES 3.2. Resources keep CPU copies; GL objects are made on the
    GL thread (the first thread that draws: the render thread) when first needed. Everything renders into FBOs
    with row 0 = D3D's top row (MojoShader vpFlip), the back buffer is a texture, Present draws it onto the window
    flipped (Adreno silently ignores glBlitFramebuffer from an FBO to the window).
  - Shaders: MojoShader (pinned commit, zlib) `glsles3`, with MOJOSHADER_FLIP_RENDERTARGET, DEPTH_CLIPPING and
    XNA4_VERTEX_TEXTURES (vertex samplers on units 16-19). D3DCOLOR inputs via MojoShader swizzles per vertex
    declaration. The alpha test is injected into each pixel shader (wrap main, `uniform vec4 bo1_alpha`,
    float-only: an int-uniform if/else version made the Adreno driver fail the link with an empty log).
  - Buffers: static ones upload once; streamed ones (DYNAMIC, or <= 4 MB and rewritten after upload) copy just
    each draw's range into a 48 MB persistently mapped ring (EXT_buffer_storage, 3 parts fenced per frame,
    host_gl.c). Updating in-use GL buffers made the Adreno driver stall/copy: 4.5 fps -> 72 fps.
  - GL calls: `port/gl.list` -> `port/tools/gen_gl.py` reads the NDK's GLES3/gl32.h and writes host wrappers that
    take guest pointers / GLsizeiptr as 32-bit values; `special` ones (glGetString(i), glShaderSource) are by hand.
  - Caps: `R_CheckDxCaps` (gfx_d3d/r_caps.cpp) checks D3DCAPS9 by byte offset; d3d9_null.cpp static_asserts the
    layout. Vendor FOURCC formats (INTZ, NULL...) are refused so the renderer takes plain D3D9 paths.
  - Bink cutscenes: none; cinematic code textures get the engine's own no-video images (black/grey).
- Engine-wide CRT fixes found on the way (platform/win32_crt.cpp + msvc_prelude.h): MSVC `rand()` (15-bit,
  RAND_MAX 0x7FFF; musl's 31-bit one hung GaussianRandom) and MSVC text-mode `fopen` (CRLF -> LF).
- Android: INTERNET permission (the game's loopback sockets), debuggable, landscape 1280x720dp panel.
- Debugging: `host_crash.c` logs a fatal signal's guest backtrace; `adb shell run-as com.bo1zombies.quest kill -USR2
  <pid>` logs every thread's (signal 42 per thread: ART blocks SIGUSR1). Addresses are link addresses for
  `llvm-symbolizer --obj=build/bo1_guest.linked.elf`. Link failures write GLSL to `<data>/glsl_errors/`.
- Testing off-head: the Quest sleeps (window destroyed) when not worn; `adb shell am broadcast -a
  com.oculus.vrpowermanager.prox_close` keeps it awake, `...automation_disable` restores it.
- Flat-panel input (tested 2026-10-31 over wireless adb): `host_input.c` queues Android key/pointer/wheel events and
  keeps an XInput-style gamepad state; `platform/quest_input.cpp` feeds them in from Win_GetEvent's per-frame
  Sys_ConsoleInput call. Verified: adb keys (Enter, arrows, the ` console), controller laser + trigger as cursor and
  clicks. Gamepad still untested. Back = Escape.
- Main menu (3D room with the menu on a TV) renders since 2026-10-31. The cause of the black screen was the GL
  bridge: arm64_32 (Apple ABI) packs stack arguments at 4 bytes, AAPCS64 gives each 8, so GL calls with more than
  9 integer arguments (glBlitFramebuffer, glTex[Sub]Image3D, ...) read garbage past the 9th. gen_gl.py now
  unpacks the guest's stack layout. Any other host import with >8 integer args needs the same care.
- Brightness: SetGammaRamp is applied in the Present pass (and in debug shots), so r_gamma / the Brightness setting
  work as on PC; the Quest default is r_gamma 1.5 (DEFAULT_COMMANDS), the PC's 0.9 looked very dark on the panel.
- 3D TV menus (front end): the pointer no longer turns the view there (quest_input.cpp skips CL_MouseEvent); instead
  it points at the TV. d3d9_gles.cpp `pick` redraws any draw that samples the UI3D texture for the one pointed-at
  pixel into a 1x1 float target, three times with that texture swapped for a coordinate texture / white / black, and
  solves (a-c)/(b-c) for the sampled texture coordinate; `Menu_PointAt3D` (ui_shared.cpp, BO1_GUEST) focuses the item
  there using the TV window's ScreenPlacement. Thumbstick/arrows still step, trigger/A/Enter select, B = Escape.
  Debug: `<data>/debug_point` holding "x y" fakes the pointer there (delete it after!); `point:` lines in logcat.
- Milestone 5, step 1 done 2026-10-31: OpenXR (Khronos loader 1.1.63, fetched by build.py) on the game's EGL context
  (pbuffer surface); the finished frame goes to an sRGB swapchain shown as a 2.4 m quad 2 m ahead (LOCAL space);
  controllers' aim rays drive the pointer (host_xr.c). Next: true stereo (per-eye views of the world, head tracking).
- Milestone 5, step 3 (stereo) working 2026-11-01, user finds it smooth: each game frame runs SCR_UpdateFrame once
  per eye (cl_scrn_mp.cpp); quest_vr.cpp turns each scene's camera into the eye's (levelled game camera + head pose,
  off-centre frustum) in R_SetViewParmsForScene; the eye + poses ride with the back-end data (R_EndFrame ->
  RB_BeginFrame -> host_xr_set_eye); host_xr.c begins the OpenXR frame at the left eye's Present and ends it with a
  projection layer at the right's. The right eye reuses the left's shadow maps and UI3D texture; extra cameras
  (front end monitor feeds) are off in VR. GL state cache (gl_state_cache.h), perf levels boosted.
  Perf: ~50 fps per eye on the front end (render thread CPU-bound, ~10 us per draw, ~1000 draws per eye).
  Perf pass 2026-11-01 (simpleperf + GPU timer queries): ~62 fps per eye on the front end (was ~33-39).
  Depth prepass off by default (r_depthPrepass 0, -290 draws/eye); GL sampler objects cached by state;
  BGRA textures upload raw with a GL swizzle (CPU conversion was ~9% of the render thread); no per-draw
  clock_gettime (a syscall in the guest). Debug: <data>/d3d9_debug bit 1<<24 skips draw calls, bit 1<<25
  logs GPU us per render pass. Now ~GPU-bound (~14 ms/frame for both eyes): main scene pass ~3.2 ms/eye,
  float-Z pass ~0.65 ms/eye (exists because INTZ is reported unsupported; INTZ needs the engine's NV_INTZ
  techniques). Profiling: `adb shell setprop security.perf_harden 0` then
  `simpleperf record --app com.bo1zombies.quest` (guest code shows as unknown; base = text map in /proc/pid/maps).
  Then INTZ: the depth texture format (GL_DEPTH24_STENCIL8, depth swizzled to all channels, nearest sampler)
  plus RESZ (the engine's POINTSIZE 0x7fa05000 point draw -> a depth glBlitFramebuffer) replace the float-Z pass:
  ~800 -> ~530 draws per eye. Defaults now also r_dof_enable 0 and r_motionblur_enable 0 (DOF was ~2 ms GPU per
  frame; both are uncomfortable in VR anyway). Result: front end 70-72 of 72 fps, GPU ~11.3 ms (87%), render
  thread ~7.1 ms per eye (budget 6.9). In-game (Kino) not measured yet. GPU cost of other toggles per frame:
  shadows ~1 ms, distortion ~0.6 ms, r_envMapSpecular ~0.
  Later: multiview (one pass for both eyes), 2D UI onto a quad layer (it is head-locked now), laser dot on the
  TV at the real hit point (now 1.5 m along the ray), then milestone 6 (controls in game).
- Debug aids in d3d9_gles.cpp, switched live by `<data>/d3d9_debug` (a number, read every 36 frames): 1 EQUAL depth
  as LEQUAL, 2 depth test off, 4 trace one frame's draws to logcat (+ shaders' GLSL to `<data>/shaders/`, their
  constants, StretchRects), 16 + units<<8 sample white on those units, 32 shot the scene target, 64 depth-tested
  draws solid magenta, 128 no culling. `listmenus` in the console also prints the open menus and, with
  `bo1_menudump <menus>` set, their items and scripts.
- Next (milestone 5+): handle the window going away and coming back, sRGB, shadows (depth texture compare),
  OpenXR stereo, Touch controllers into the same gamepad state.

### VR status 2026-11-01 (milestone 6 working, user-tested in Kino)
- Menus: 2D UI on a world panel (quad layer, follows the head past 30 deg); 3D TV menus point with the laser;
  ui_skipMainLockout 1 (no "Press Any Key"), version string hidden. TV/panel pointer modes: host_xr_pointer_mode.
- In game (quest_vr.cpp bo1_vr_input, hooked in CL_CreateCmd): right controller aims (the game's viewangles),
  right stick x smooth-turns (120 deg/s), left stick moves relative to the head; Touch -> virtual XInput pad
  (host_xr.c update_pad): RT fire, LT ADS, A jump, B crouch, X reload/use, Y switch, grips grenades (R lethal,
  L tactical), stick clicks sprint/melee. IN_Frame polls the gamepad even without window activation.
- Gun in the right hand (bo1_vr_hand_weapon, CG_AddPlayerWeapon): the viewmodel's pose kept relative to the
  camera, the camera swapped for the controller; grip offset auto-corrected each frame so j_wrist_ri lands on
  the controller. Arms: two-bone IK for both arms (bo1_vr_left_hand): shoulders below/beside the head,
  left wrist on the left controller (relaxed rest-pose fingers), right wrist where the gun has it.
  DObjSetLocalTag REPLACES a bone's parent-relative transform (calibrated at runtime). Skeleton is world space.
- Two-handed (Halo grip mode): left grip at the animated foregrip (13 cm) locks; gun aims right->left hand;
  left arm back to the animation; LB masked from the game while near/held.
- Crosshair: the game's reticle drawn into each eye (RC_SET_CUSTOM_CONSTANT 0xB01 marker pauses the panel
  redirect) at the projection of the shots' trace hit (bo1_vr_crosshair), 0.6x size.
- Physics fix: clang reuses a non-POD base's tail padding (MSVC doesn't): broad_phase_base got an explicit
  m_bo1_tail_padding, fixing the slot_pool assert crash when ragdolls were freed. Watch for this elsewhere.
- Kino perf: ~3000-4000 draws/eye in places (menu ~500): needs the performance backlog.

### Audio plan (milestone 7, started 2026-11-01)
The engine's driver (sound/snd_driver_xaudio2*.cpp) is excluded from the build and SD_Xaudio2CanInit stubbed
false (stubs.cpp). Plan: keep the driver, implement the subset of XAudio2 it uses in the guest
(CreateSourceVoice/Submix/Mastering, SubmitSourceBuffer, Start/Stop/Destroy, GetState, SetFrequencyRatio,
SetOutputMatrix, SetEffectParameters, IXAudio2VoiceCallback for streams) on a software mixer (PCM16 +
MS-ADPCM decode, resampling, matrix to stereo), output through a host AAudio stream (48 kHz float stereo,
ring buffer filled by a guest mixer thread). Stage 2: run the game's own XAPO effects (voice DSP/futz,
radverb, master EQ/limiter). Stage 3: xWMA if zombies uses it.

### Kino pass (2026-11-01, done)
Kino was render-thread bound (GPU ~3-4 ms/eye, render thread 9-19 ms/eye at 900-2400 draws). Done:
- Streamed buffers: a draw reuses the ring copy an earlier draw made of the same unchanged range in the same
  ring part (Buffer::generation, Device::stream_cached): 2.5-8.5 MB -> ~1.2 MB copied per eye frame.
- Program binary cache: glCompileShader is deferred and glLinkProgram first tries
  <data>/shader_cache/<hash>.bin (glProgramBinary); links were ~13% of the render thread and the stutters.
- Texture units skip re-applying when gl_cache says the unit already holds that texture + sampler.
- Result: ~4.9 -> ~4.3 us per draw, 1400-draw views 18.8 -> 8.9 ms per eye; heavy views (2400 draws) still
  ~11 ms per eye (~45 fps). Left eye also draws the shadow maps (~600 draws); the right eye reuses them.
- Stats line now logs reused stream KB and shader compiles/links/cache hits every 5 s.

### Multiview (written 2026-11-01, built, NOT yet tested on the headset)
Both eyes in one pass with OVR_multiview2, off unless `bo1_vr_multiview 1` (read when the render targets are
made, so it takes effect from the next start; `+set bo1_vr_multiview 1` in bo1_args.txt for a test). Only while
playing (g_bo1_vr_controls); menus keep one frame per eye (the in-world TV menus need pointer picking).
- Front end (quest_vr.cpp, cl_scrn_mp.cpp, r_scene.cpp): one SCR_UpdateFrame per pair with eye 2; bo1_vr_view
  draws for the left eye and keeps the right eye's GfxViewParms plus a culling view (between the eyes, 6 cm back,
  union of the FOVs) that R_RenderScene uses as the DPVS view. The right eye's view travels with the frame data.
- Back end (quest_multiview.cpp; hooks in r_shade.cpp, r_state_utils.cpp R_BeginView, rb_backend.cpp marker):
  the main view is recognised by the left eye's view matrix; every view-dependent code constant (matrix groups
  view/proj/worldView/viewProj/WVP/outdoor lookup, near-plane and VPOS constants) is also derived for the right
  eye (Eye1Scope swaps viewParms; WVP fast path) and sent with bo1_d3d9_eye1_constants. eyeOffset stays the left
  eye's. The crosshair's 2D gets the right eye's clip-space offset (marker vec[1..2]).
- Device (d3d9_gles.cpp): the engine marks the per-eye render targets (r_rendertarget.cpp, end of
  R_InitRenderTargets_PC) and they become 2-layer GL_TEXTURE_2D_ARRAYs; FramebufferKey.mode = FB_MULTIVIEW when
  every attachment is layered in an eye-2 frame (host_gl_multiview_attach), else layer 0/1. Shader variants
  (g_compile_mode, hooked_glShaderSource): multiview VS (num_views 2) and every xs_uniforms_vec4 read picks
  bo1_e1_xs in view 1 (uploaded per program from the right-eye register files, MojoShader's packing from parse
  data); samplers of layered textures become sampler2DArray at layer gl_ViewID (0 outside multiview).
  StretchRect/RESZ copy both layers; Present shows layer 0 and 1 to the two eyes (bo1_vr_present_eye).
- Status 2026-11-02: WORKING (tested on the headset, both eyes right). Kino 70-72 fps up to ~1500 draws, ~58 at
  ~3000 (per-eye path: 48-60). Fixes that made the Adreno driver's multiview work (d3d9_gles.cpp rewrite_for_mode):
  gl_Position written once (MojoShader read it back for vpFlip/z remap -> view 1's other outputs were garbage);
  gl_ViewID_OVR only in the vertex shader (in a pixel shader it's always 0) -> the vertex shader runs its body twice
  (position with the view's constants, all other outputs with the left eye's) and passes the view as a flat varying;
  no right-eye copy of pixel-shader constants (view 1 read it as zeros); apply_sampler makes its unit active before
  realize() (it bound textures over the previous unit); the INTZ depth copy marked layered by texture; the 256x256
  composite target NOT layered (drawn once, read by all materials); main view known by the left eye's view matrix.
  Also fixed after user testing (2026-11-02): the sun flare and light coronas are placed in the left eye's clip
  space on the CPU (identity mode) -> rb_sky.cpp/rb_corona.cpp wrap them in BO1_REPROJECT and every multiview vertex
  shader applies bo1_clip1 (inv(VP0)*VP1, in D3D clip space) to view 1's position; layered StretchRect / RESZ depth
  copies use glCopyImageSubData per layer (the framebuffer blits put both layers into layer 0: ghost through glass
  in the left eye, black/missing muzzle flashes in the right); the resolved scene copy is still made when post is
  skipped. User confirmed everything looks right.
  Known gap: post-processing (bloom, final tonemap) is skipped in multiview frames (rb_postfx.cpp; it came out
  dark/black - bo1_vr_mv_post 1 to test). Enable with +set bo1_vr_multiview 1 (not saved to config).
- Older notes (2026-11-01, before the fixes):  Layered storage + per-eye path works (menus, game). In multiview
  frames the left layer of the scene is correct before post-processing, but the right layer (view 1) comes out
  magenta-tinted with black blocks even with identical shader inputs for both views (debug bits 29/30: left-eye
  constants and layer-0 sampling everywhere), and post-processing then turns the left eye dark/blurry and the right
  black. Ruled out: eye1 view parms (bit 19), the view varying vs gl_ViewID_OVR in the pixel shader (bit 20),
  clears (now per layer anyway), stencil/depth format (bit 21), the 256x256 composite target (unmarked: it's drawn
  once and sampled by every material), framebuffer modes (all passes mode 1), GL errors (bit 28, none).
  Next ideas: Adreno multiview with many texture units / sampler objects / uniform counts; try a minimal multiview
  draw inside the renderer; capture with RenderDoc-like tooling. Debug bits in <data>/d3d9_debug: 17 shot before
  bloom, 18 tint by view, 19 eye1=eye0 parms, 20 no view varying, 21 depth-only array, 26 force off, 27 force on
  without controllers, 28 GL errors, 29 left-eye constants + layer 0, 30 left-eye constants, 31 dump mv shaders,
  64 (with shot_request) the right layer.
- Test plan: 1) dvar on, front end (layered targets, per-eye path) must look as before; 2) in game: logcat
  "multiview: main view found", fps (the stats line counts one Present per pair in multiview), stereo depth
  correct, crosshair on target in both eyes, post effects (bloom etc.) per eye; 3) GL errors / link failures in
  <data>/glsl_errors. Known gaps: fixed-function draws sampling a layered texture; specular/fog use the left eye's
  position for both eyes (eyeOffset).

### Performance backlog (measured 2026-11-01, not done yet)
Front end is at 70-72/72; the render thread is ~0.2 ms per eye over budget, GPU ~87%. In rough order of value:
- In-game (Kino) measurement first: likely heavier than the menu.
- Render thread (about half its time is in the Adreno driver): VAOs per (declaration, vertex buffers) instead of
  VAO 0 + attribute pointers every draw; fewer redundant glUniform uploads (MojoShader pushes whole arrays when
  any register changed); RB_UpdateDynamicBuffers (~0.55 ms per eye before each frame, mostly copies).
- Multiview (both eyes in one pass): the big one, but the engine's per-draw constants include the view, so
  shaders would need the eye's matrices chosen by gl_ViewID_OVR. Halves draws and most driver work.
- GPU: per-eye size 1920x1080 doesn't match the eye's FOV (runtime recommends 1680x1760); a squarer, similar
  pixel count looks sharper vertically; slightly fewer pixels saves fill. Needs r_mode/back buffer and the
  host's eye swapchains to agree.
- GPU toggles if needed: shadows ~1 ms/frame (sm_enable, sm_qualitySpotShadow 0 ~0.5 ms), distortion ~0.6 ms.
- Tools: <data>/d3d9_debug bit 1<<25 logs GPU us per render pass; bit 1<<24 skips draw calls; simpleperf (above).

### Engine source changes (in src/, all also valid for the Windows build; `git -C src diff`)
- `universal/assertive.h`: release-build assert macros variadic (callers pass extra arguments).
- `sound/snd_driver_xaudio2_dsp.h`: XAudio2 effect classes under `#ifndef BO1_GUEST`.
- `physics/phys_colgeom.h`: guest size for `gjk_partition_t` (Itanium ABI tail padding).
- `game_mp/g_scr_main_mp.h`: opaque enum declaration with its underlying type.
- `game_sp/g_sp_save_actor.inl`: `offsetof` through the plain array, not `operator[]`.
- `universal/q_shared.h`: angle indexes and the byte-swap helpers outside the `#ifdef WIN32` block.
- `universal/q_shared.cpp`, `clientscript/cscr_evaluate.*`: `g_com_error` / `g_script_error` are `jmp_buf`
  (MSVC's is int[16], the old declaration; ARM's is larger).
- `clientscript/cscr_debugger.h`: `operator new(size_t)`. `ui/ui_main.cpp`: initialised const array.
- `universal/memfile.cpp`: `lzo_init()` (MSVC ignored the extra arguments).
  `universal/com_expressions_eval.cpp`: function pointer cast to `void *`.
- `win32/win_net.cpp`: `sin_addr.s_addr` and `FD_ZERO`/`FD_SET` (portable spellings of Winsock's fields).
- `win32/win_main.cpp`: the x86 crash/stack-walk code is `#ifndef BO1_GUEST` with no-op guest versions.
- `#ifdef BO1_GUEST` branches: `qcommon/msg.cpp` (x87 FISTP rounding → `lrint`), `client_mp/cl_main_pc_mp.cpp`
  (no Steam ticket), `monkey/monkey_comm.cpp` (portable `fd_set`), `qcommon/threads.cpp` (no SEH thread naming),
  `physics/phys_gjk.cpp` (x87 check), `sound/snd_driver_xaudio2*.h` (XAudio2 effect members).

### Toolchain facts
- Guest: NDK clang `--target=arm64_32-apple-watchos` → `port/tools/asm_convert.py` → ELF.
  `-mcpu=cortex-a76+norcpc` (the emulator's translator lacks `ldapr`), `-mms-bitfields`
  (MSVC bitfield layout, needed for fastfile structs), `-femulated-tls`.
- Aliases don't exist on the Darwin target: musl's `weak_alias` is an assembler `.set`
  (`port/guest/musl/src_include/features.h`).
- The low 4 GB layout differs per device. Quest 3 (Horizon OS, 2026-10): ART's large-object space and
  JIT caches cover 0x76ac1000–0x9eac1000, so a fixed 0x80000000 base fails there. Free on the Quest:
  0x10000000–0x12c00000, 0x32c00000–0x700a0000 (980 MB), 0x9eac1000–0xebad6000 (1232 MB),
  0xebad7000–0xffff0000 (325 MB). Hence the relocatable image: linked at 0x80000000 with
  `--emit-relocs`, `port/tools/guest_relocs.py` lists the ABS32/ABS64 words in `.guest_relocs`, and
  the host places the image at the top of the largest gap.
- clang on the Darwin-derived target folds sin+cos into `__sincos(f)_stret`; the runtime's own definition must
  call sin/cos through pointers or it recurses forever (it did: a 16 MB stack overflow at map load).
- The guest's `exit_group` uses `_exit`: `exit()` from a non-main thread runs static destructors under
  ART and the runtime aborts (SIGABRT).
- Test device: Quest 3 over adb.
- On a case-sensitive filesystem build.py still needs to add `Windows.h` as an alias of the
  compat `windows.h` (on Windows they are the same file).

## Milestones

1. **Toolchain:** guest build (CMake + NDK clang) of the platform-independent engine code; asm converter; musl.
2. **Host loader:** APK that loads `bo1_guest.elf`, runs `main`, logs to logcat (no rendering).
3. **Engine boots headless:** filesystem against `/sdcard/Android/data/<pkg>/files`, fastfiles load, server
   runs a zombies map with `dedicated`-style null renderer.
4. **d3d9_gles + MojoShader:** flat 2D on Quest (menu visible on a quad).
5. **OpenXR stereo:** in-world stereo, head tracking.
6. **VR controls:** hands, weapon in hand, aiming, menus with laser, comfort options.
7. **Audio, performance pass (72/90 Hz), data import UI.**
