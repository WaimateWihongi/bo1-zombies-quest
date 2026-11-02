# BO1 Zombies for Meta Quest 3 (native VR)

A native, standalone Meta Quest 3 port of **Call of Duty: Black Ops (2010) Zombies**, built on the
community decompilation [MisaDev4/bo1-zombies-decompiled](https://github.com/MisaDev4/bo1-zombies-decompiled),
with full 6DoF VR and Touch controller support.

> **No game files are included or distributed.** You need your own legal copy of Call of Duty: Black Ops
> (Steam) and copy its data onto your headset yourself. This is an unofficial fan project, not affiliated
> with or endorsed by Activision or Treyarch.

## Features

- Runs natively on Quest 3 (no PC, no streaming): the engine runs as a 32-bit ARM guest inside a 64-bit
  Android host, D3D9 is translated to OpenGL ES 3.2 (MojoShader), and output goes through OpenXR.
- Stereo rendering, including an optional **multiview** path that draws both eyes in one pass
  (about 70-72 fps in Kino der Toten versus 48-60 drawing each eye separately).
- Controls modelled on the HaloCE VR port: the gun is held and aimed with the right controller and fires
  from the hand; two-handed grip with the left grip button; arm IK for both hands; a 3D crosshair placed
  where shots will land; smooth turning; movement relative to the head.
- Menus and HUD on a floating panel with a laser pointer; the front end's in-world TV menus are usable
  with the pointer.
- Audio through AAudio, including the game's xWMA sounds (decoded with FFmpeg's WMA v2 decoder).

## Repository layout

| Path | What |
|---|---|
| `port/host/` | 64-bit Android host: loader, syscalls, GL/EGL, OpenXR, input, audio, WMA decoding |
| `port/guest/platform/` | Win32/D3D9 replacement layer for the engine: `d3d9_gles.cpp`, VR logic (`quest_vr.cpp`, `quest_multiview.cpp`), input, sound driver |
| `port/guest/compat/`, `port/guest/musl/`, `port/guest/runtime/` | Headers and runtime for the 32-bit guest (musl libc port) |
| `port/tools/` | Build helpers: Mach-O to ELF assembly conversion, import trampolines, GL bindings |
| `port/build.py` | Configures the build (writes a ninja file) |
| `engine-changes.patch` | The changes this port makes to the decompiled engine source |
| `PORT_PLAN.md` | Design notes, status and findings (including the Quest driver quirks hit by multiview) |

## Building

Requirements: Windows, Python 3, the Android SDK (build-tools, a platform) and NDK 27, and ninja.

1. Clone the engine source next to `port/` as `src/` and apply the port's changes:
   ```
   git clone https://github.com/MisaDev4/bo1-zombies-decompiled src
   cd src
   git checkout 102f699
   git apply ../engine-changes.patch
   cd ..
   ```
2. Build FFmpeg's WMA v2 decoder for Android (LGPL 2.1; only this decoder is needed). Using FFmpeg 6.1.2,
   configure for `aarch64` Android with the NDK clang, `--disable-everything --enable-decoder=wmav2
   --enable-static --disable-shared`, build it, and put the result in
   `build/third_party/ffmpeg-wma/{include,lib}` (`libavcodec.a`, `libavutil.a` and their headers).
3. Configure and build (musl, MojoShader, sse2neon and the OpenXR loader are downloaded automatically):
   ```
   python port/build.py
   ninja -C build
   ```
   The result is `build/bo1-quest.apk`.

## Installing and playing

1. Install the APK: `adb install build/bo1-quest.apk`.
2. Copy the game data from your Steam install (`Call of Duty Black Ops`): the `zone/`, `main/` and
   `players/` folders, onto the headset at
   `/sdcard/Android/data/com.bo1zombies.quest/files/`.
3. Launch "BO1 Zombies" from the headset's app library (Unknown Sources).

Optional: a `bo1_args.txt` file in that folder replaces the default command line. For example, adding
`+set bo1_vr_multiview 1` turns on single-pass stereo (multiview); bloom and the final colour grading are
skipped in that mode for now.

### Controls

| Control | Action |
|---|---|
| Right controller | Aim the gun |
| Right trigger | Fire |
| Left trigger | Aim down the sights |
| Left stick | Move (where you look) |
| Right stick | Smooth turn |
| Left grip near the gun's foregrip | Hold the gun with both hands |
| Grips | Grenades |
| A / B / X / Y | Jump, crouch, reload/use, switch weapon |
| Stick clicks | Sprint, melee |

## Credits

- [MisaDev4/bo1-zombies-decompiled](https://github.com/MisaDev4/bo1-zombies-decompiled): the decompiled engine.
- [moistman42069/HaloCE-DecompRemix-VR-PC-Android](https://github.com/moistman42069/HaloCE-DecompRemix-VR-PC-Android):
  the host/guest architecture and VR design this port follows.
- [MojoShader](https://github.com/icculus/mojoshader) (zlib), [musl](https://musl.libc.org) (MIT),
  [sse2neon](https://github.com/DLTcollab/sse2neon) (MIT), the Khronos OpenXR loader (Apache 2.0) and
  [FFmpeg](https://ffmpeg.org) (LGPL 2.1, WMA v2 decoder only).

Call of Duty and Black Ops are trademarks of Activision Publishing, Inc.
