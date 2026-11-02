"""Configure the Quest build: writes build/build.ninja, then run `ninja -C build`.

  python port/build.py            # smoke-test guest (milestone 2)
  ninja -C build                  # -> build/bo1-quest.apk

Needs the Android SDK (build-tools, a platform) and NDK; found through ANDROID_HOME /
ANDROID_NDK_HOME or the default Android Studio location. Runs on Windows or Linux.
"""

import argparse
import hashlib
import os
import platform
import re
import shutil
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.path.join(ROOT, "port")
BUILD = os.path.join(ROOT, "build")

MUSL_VERSION = "1.2.5"
MUSL_URL = f"https://musl.libc.org/releases/musl-{MUSL_VERSION}.tar.gz"
MUSL_SHA256 = "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"
MUSL = os.path.join(BUILD, "third_party", f"musl-{MUSL_VERSION}")
# SSE intrinsics on NEON for the engine's _mm_* code (MIT)
SSE2NEON_URL = "https://raw.githubusercontent.com/DLTcollab/sse2neon/v1.8.0/sse2neon.h"
SSE2NEON_SHA256 = "07723c9f9457dd4316f1fde3dd4eb6f31dd67d9955f6c21f4e609ac1698be48a"
SSE2NEON = os.path.join(BUILD, "third_party", "sse2neon")
# D3D9 shader bytecode -> GLSL ES (zlib licence), pinned to a commit
MOJOSHADER_COMMIT = "ad5dff84830c2863c841f4b1f4e3df78c705b383"
MOJOSHADER_URL = f"https://github.com/icculus/mojoshader/archive/{MOJOSHADER_COMMIT}.tar.gz"
MOJOSHADER_SHA256 = "728ff9098115059c4012895b7a85065688c857fdadf9fbc04484fec8c7be3081"
MOJOSHADER = os.path.join(BUILD, "third_party", f"mojoshader-{MOJOSHADER_COMMIT}")
# The Khronos OpenXR loader for Android (Apache-2.0): headers and libopenxr_loader.so, shipped in the APK
OPENXR_VERSION = "1.1.63"
OPENXR_URL = ("https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/"
              f"{OPENXR_VERSION}/openxr_loader_for_android-{OPENXR_VERSION}.aar")
OPENXR_SHA256 = "622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73"
OPENXR = os.path.join(BUILD, "third_party", f"openxr-{OPENXR_VERSION}")
# the NDK's GLES/KHR headers, copied apart from its libc headers for the guest
GL_HEADERS = os.path.join(BUILD, "gen", "glinc")
MUSL_ARCH = os.path.join(PORT, "guest", "musl", "arch", "arm64_32")
MUSL_GEN = os.path.join(BUILD, "musl")

EXE = ".exe" if os.name == "nt" else ""

GUEST_TARGET = [
    # the only clang target with AArch64 code and 32-bit pointers; asm_convert.py makes it ELF
    "--target=arm64_32-apple-watchos", "-U__APPLE__", "-U__MACH__", "-fno-define-target-os-macros",
    "-D__linux__=1", "-D__unix__=1", "-DBO1_GUEST=1",
    # generic NEON syntax: Apple's (`mov.16b v0, v1`) does not assemble for ELF
    "-mllvm", "-aarch64-neon-syntax=generic",
    # Quest 2/3 (ARMv8.2 + LSE); never the watch's Apple CPU. No RCpc (ldapr): the emulator's ARM
    # translator, used for testing without a headset, lacks it
    "-mcpu=cortex-a76+norcpc",
    "-nostdinc", "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-femulated-tls", "-ffp-contract=off", "-fno-omit-frame-pointer", "-O2", "-g0",
]

# musl files the guest runtime replaces (port/guest/runtime)
MUSL_REPLACED = {
    "src/thread/pthread_create.c", "src/env/__init_tls.c", "src/env/__libc_start_main.c",
    "src/thread/__set_thread_area.c", "src/thread/__unmapself.c", "src/thread/clone.c",
    "src/thread/pthread_cancel.c",
}
# AArch64 assembly from musl that works unchanged for ILP32 (it addresses memory through
# zero-extended pointers); replaces the generic file of the same name
MUSL_ARCH_ASM = ["src/setjmp/aarch64/setjmp.s", "src/setjmp/aarch64/longjmp.s",
                 "src/signal/aarch64/sigsetjmp.s", "src/fenv/aarch64/fenv.s"]


def find_sdk():
    sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if not sdk:
        sdk = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk") if os.name == "nt" \
            else os.path.expanduser("~/Android/Sdk")
    if not os.path.isdir(sdk):
        sys.exit("Android SDK not found: set ANDROID_HOME")
    return sdk


def newest(path):
    names = sorted(os.listdir(path), key=lambda n: [int(x) if x.isdigit() else x for x in re.split(r"[.-]", n)])
    return os.path.join(path, names[-1]) if names else None


def find_ndk(sdk):
    ndk = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT")
    if not ndk and os.path.isdir(os.path.join(sdk, "ndk")):
        ndk = newest(os.path.join(sdk, "ndk"))
    if not ndk or not os.path.isdir(ndk):
        sys.exit("Android NDK not found: set ANDROID_NDK_HOME")
    return ndk


def fetch_musl():
    if os.path.isdir(MUSL):
        return
    os.makedirs(os.path.dirname(MUSL), exist_ok=True)
    archive = MUSL + ".tar.gz"
    if not os.path.exists(archive):
        print(f"downloading {MUSL_URL}")
        urllib.request.urlretrieve(MUSL_URL, archive)
    with open(archive, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != MUSL_SHA256:
            sys.exit(f"{archive}: SHA-256 mismatch")
    with tarfile.open(archive) as t:
        t.extractall(os.path.dirname(MUSL))


def fetch_sse2neon():
    path = os.path.join(SSE2NEON, "sse2neon.h")
    if os.path.exists(path):
        return
    os.makedirs(SSE2NEON, exist_ok=True)
    print(f"downloading {SSE2NEON_URL}")
    data = urllib.request.urlopen(SSE2NEON_URL).read()
    if hashlib.sha256(data).hexdigest() != SSE2NEON_SHA256:
        sys.exit("sse2neon.h: SHA-256 mismatch")
    with open(path, "wb") as f:
        f.write(data)


def fetch_mojoshader():
    version_header = os.path.join(MOJOSHADER, "mojoshader_version.h")
    if os.path.exists(version_header):
        return
    os.makedirs(os.path.dirname(MOJOSHADER), exist_ok=True)
    archive = MOJOSHADER + ".tar.gz"
    if not os.path.exists(archive):
        print(f"downloading {MOJOSHADER_URL}")
        urllib.request.urlretrieve(MOJOSHADER_URL, archive)
    with open(archive, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != MOJOSHADER_SHA256:
            sys.exit(f"{archive}: SHA-256 mismatch")
    with tarfile.open(archive) as t:
        t.extractall(os.path.dirname(MOJOSHADER))
    # what MojoShader's CMakeLists.txt would generate
    write_if_changed(version_header, "\n".join([
        "#ifndef _INCL_MOJOSHADER_VERSION_H_",
        "#define _INCL_MOJOSHADER_VERSION_H_",
        "#define MOJOSHADER_VERSION -1",
        f'#define MOJOSHADER_CHANGESET "git-{MOJOSHADER_COMMIT}"',
        "#endif",
        ""]))


def fetch_openxr():
    """build/third_party/openxr-<version>/{include/openxr/*.h, lib/libopenxr_loader.so, LICENSE}"""
    loader = os.path.join(OPENXR, "lib", "libopenxr_loader.so")
    if os.path.exists(loader):
        return
    archive = OPENXR + ".aar"
    if not os.path.exists(archive):
        print(f"downloading {OPENXR_URL}")
        os.makedirs(os.path.dirname(OPENXR), exist_ok=True)
        urllib.request.urlretrieve(OPENXR_URL, archive)
    with open(archive, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != OPENXR_SHA256:
            sys.exit(f"{archive}: SHA-256 mismatch")
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            if name.startswith("prefab/modules/headers/include/openxr/") and name.endswith(".h"):
                out = os.path.join(OPENXR, "include", "openxr", os.path.basename(name))
            elif name == "jni/arm64-v8a/libopenxr_loader.so":
                out = loader
            elif name == "META-INF/LICENSE":
                out = os.path.join(OPENXR, "LICENSE")
            else:
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(z.read(name))


def copy_gl_headers(sysroot_include):
    for sub in ("GLES2", "GLES3", "KHR"):
        src = os.path.join(sysroot_include, sub)
        for name in os.listdir(src):
            with open(os.path.join(src, name), encoding="utf-8") as f:
                write_if_changed(os.path.join(GL_HEADERS, sub, name), f.read())


def write_if_changed(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def generate_musl_headers():
    """musl's generated headers (what its Makefile does with sed)."""
    def alltypes_line(line):
        m = re.match(r"^TYPEDEF (.*) ([^ ]*);$", line)
        if m:
            return (f"#if defined(__NEED_{m[2]}) && !defined(__DEFINED_{m[2]})\ntypedef {m[1]} {m[2]};\n"
                    f"#define __DEFINED_{m[2]}\n#endif\n")
        for kind in ("STRUCT", "UNION"):
            m = re.match(rf"^{kind} * ([^ ]*) (.*);$", line)
            if m:
                k = kind.lower()
                return (f"#if defined(__NEED_{k}_{m[1]}) && !defined(__DEFINED_{k}_{m[1]})\n{k} {m[1]} {m[2]};\n"
                        f"#define __DEFINED_{k}_{m[1]}\n#endif\n")
        return line
    text = ""
    for src in (os.path.join(MUSL_ARCH, "bits", "alltypes.h.in"), os.path.join(MUSL, "include", "alltypes.h.in")):
        with open(src, encoding="utf-8") as f:
            for line in f.read().splitlines():
                out = alltypes_line(line)
                text += out if out != line else line + "\n"
    write_if_changed(os.path.join(MUSL_GEN, "include", "bits", "alltypes.h"), text)
    with open(os.path.join(MUSL_ARCH, "bits", "syscall.h.in"), encoding="utf-8") as f:
        syscall = f.read()
    syscall += "".join(l.replace("__NR_", "SYS_") + "\n" for l in syscall.splitlines() if "__NR_" in l)
    write_if_changed(os.path.join(MUSL_GEN, "include", "bits", "syscall.h"), syscall)
    write_if_changed(os.path.join(MUSL_GEN, "src", "internal", "version.h"), f'#define VERSION "{MUSL_VERSION}"\n')


def musl_sources():
    replaced = set(MUSL_REPLACED)
    for asm in MUSL_ARCH_ASM:
        d, name = os.path.split(asm)
        replaced.add(os.path.join(os.path.dirname(d), os.path.splitext(name)[0] + ".c").replace("\\", "/"))
    dirs = [os.path.join("src", d) for d in sorted(os.listdir(os.path.join(MUSL, "src")))
            if os.path.isdir(os.path.join(MUSL, "src", d))]
    dirs.append("src/malloc/mallocng")
    sources = []
    for d in dirs:
        for name in sorted(os.listdir(os.path.join(MUSL, d))):
            rel = f"{d}/{name}".replace("\\", "/")
            if name.endswith(".c") and rel not in replaced:
                sources.append(rel)
    return sources + MUSL_ARCH_ASM


ENGINE = os.path.join(ROOT, "src")
# Folders whose code the port replaces rather than compiles (Windows platform, Direct3D 9,
# Steam/DemonWare online, Bink video, NVIDIA and Tracy tooling); see PORT_PLAN.md.
ENGINE_REPLACED = (
    # win32/: desktop-only pieces; the rest of win32/ builds on the Win32 layer in port/guest/platform
    "src/win32/win_voice", "src/win32/win_steam", "src/win32/win_mini_dumper",
    "src/win32/win_wndproc", "src/win32/win_syscon",  # desktop window + console: port/guest/platform/quest_window.cpp
    "src/steam/", "src/nvapi/", "src/binklib/", "src/tracy/", "src/DW/", "DemonWare/",
    "src/CubeMapGenLib/",  # reflection-probe baking (Windows UI, exceptions): stubbed in platform/stubs.cpp
    "src/sound/snd_driver_xaudio2",  # XAudio2: an Android audio backend replaces it
    "src/groupvoice/play_dsound", "src/groupvoice/record_dsound",  # DirectSound voice chat
    "src/vpx/", "src/mjpeg/avi.cpp",  # libvpx video recording
    "src/gfx_d3d/r_cinematic.cpp",  # Bink cutscenes: skipped until a decoder exists
)


def engine_sources():
    """The engine's .c/.cpp files, from its own CMake source list."""
    with open(os.path.join(ENGINE, "cmake_files.cmake"), encoding="utf-8") as f:
        text = f.read()
    dirs = {"SRC_DIR": "src", "TL_DIR": "tl", "DW_DIR": "DemonWare"}
    sources = []
    for var, rel in re.findall(r'"\$\{(SRC_DIR|TL_DIR|DW_DIR)\}/([^"]+\.(?:c|cpp))"', text):
        path = f"{dirs[var]}/{rel}"
        if not path.startswith(ENGINE_REPLACED) and path not in sources:
            sources.append(path)
    return sources


def ninja_path(p):
    p = os.path.relpath(p, BUILD).replace("\\", "/")
    return p.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def quote(p):
    p = p.replace("\\", "/")
    return f'"{p}"' if " " in p else p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.parse_args()

    sdk = find_sdk()
    ndk = find_ndk(sdk)
    host_os = "windows-x86_64" if os.name == "nt" else ("darwin-x86_64" if platform.system() == "Darwin" else "linux-x86_64")
    llvm = os.path.join(ndk, "toolchains", "llvm", "prebuilt", host_os)
    bin_ = os.path.join(llvm, "bin")
    clang = os.path.join(bin_, "clang" + EXE)
    clangxx = os.path.join(bin_, "clang++" + EXE)
    builtins = None
    clang_lib = os.path.join(llvm, "lib", "clang")
    if os.path.isdir(clang_lib):
        builtins = os.path.join(newest(clang_lib), "lib", "linux", "libclang_rt.builtins-aarch64-android.a")
    build_tools = newest(os.path.join(sdk, "build-tools"))
    android_jar = os.path.join(newest(os.path.join(sdk, "platforms")), "android.jar")
    glue = os.path.join(ndk, "sources", "android", "native_app_glue")
    for p in (clang, builtins, build_tools, android_jar, glue):
        if not p or not os.path.exists(p):
            sys.exit(f"missing: {p}")

    fetch_musl()
    fetch_sse2neon()
    fetch_mojoshader()
    fetch_openxr()
    copy_gl_headers(os.path.join(llvm, "sysroot", "usr", "include"))
    generate_musl_headers()
    clang_resource = os.path.join(newest(clang_lib), "include")

    py = quote(sys.executable)
    tools = os.path.join(PORT, "tools")
    musl_inc = [os.path.join(PORT, "guest", "musl", "src_include"), MUSL_ARCH, os.path.join(MUSL, "arch", "generic"),
                os.path.join(MUSL_GEN, "src", "internal"), os.path.join(MUSL, "src", "include"),
                os.path.join(MUSL, "src", "internal"), os.path.join(MUSL_GEN, "include"), os.path.join(MUSL, "include")]
    public_inc = [MUSL_ARCH, os.path.join(MUSL, "arch", "generic"), os.path.join(MUSL_GEN, "include"),
                  os.path.join(MUSL, "include"), os.path.join(PORT, "include")]

    def incs(dirs):
        return " ".join("-I" + quote(d) for d in dirs)

    target = " ".join(GUEST_TARGET)
    libcxx = os.path.join(llvm, "sysroot", "usr", "include", "c++", "v1")
    lines = [
        "# generated by port/build.py",
        "ninja_required_version = 1.10",
        f"py = {py}",
        f"clang = {quote(clang)}",
        f"clangxx = {quote(clangxx)}",
        f"tools = {quote(tools)}",
        f"musl_cflags = {target} -std=c99 -ffreestanding -D_XOPEN_SOURCE=700 -fno-strict-aliasing -frounding-math -w {incs(musl_inc)}",
        f"runtime_cflags = {target} -std=gnu11 -D_GNU_SOURCE -Wall -Wno-unused-function {incs(musl_inc + [os.path.join(PORT, 'include'), os.path.join(PORT, 'guest', 'runtime')])}",
        f"runtime_cxxflags = {target} -std=gnu++20 -fno-exceptions -fno-rtti -nostdinc++ -Wall {incs(public_inc + [os.path.join(PORT, 'guest', 'runtime')])}",
        f"smoke_cflags = {target} -std=gnu11 -Wall {incs(public_inc)}",
        f"mojo_cflags = {target} -std=gnu11 -w -DMOJOSHADER_FLIP_RENDERTARGET -DMOJOSHADER_DEPTH_CLIPPING -DMOJOSHADER_XNA4_VERTEX_TEXTURES "
        "-DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_ARB1=0 "
        "-DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_METAL=0 -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 "
        "-DSUPPORT_PROFILE_GLSL=1 -DSUPPORT_PROFILE_GLSL120=0 -DSUPPORT_PROFILE_GLSLES=1 -DSUPPORT_PROFILE_GLSLES3=1 "
        f"-DMOJOSHADER_EFFECT_SUPPORT=0 {incs(public_inc + [MOJOSHADER, GL_HEADERS])}",
        f"gltable_cflags = {target} -std=gnu11 -Wall {incs(public_inc + [GL_HEADERS])}",
        f"smoke_cxxflags = {target} -std=gnu++20 -fno-exceptions -fno-rtti -nostdinc++ -Wall {incs(public_inc)}",
        f"engine_cxxflags = {target} -std=gnu++20 -fms-extensions -mms-bitfields -fno-exceptions -fno-rtti -nostdinc++ "
        "-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -fshort-wchar "
        "-D_LIBCPP_HAS_MUSL_LIBC -D_LIBCPP_DISABLE_EXTERN_TEMPLATE -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE "
        "-DBO1_MP -DNDEBUG -D_CRT_SECURE_NO_WARNINGS -Wno-everything -ferror-limit=50 "
        f"-include {quote(os.path.join(PORT, 'guest', 'compat', 'msvc_prelude.h'))} "
        f"{incs([os.path.join(PORT, 'guest', 'compat'), SSE2NEON, libcxx, GL_HEADERS, MOJOSHADER] + public_inc + [ENGINE, os.path.join(ENGINE, 'src'), os.path.join(ENGINE, 'src', 'libs'), os.path.join(ENGINE, 'src', 'libs', 'libtomcrypt-1.17', 'src', 'headers'), os.path.join(ENGINE, 'src', 'libs', 'libtommath-1.0'), clang_resource])}",
        f"engine_cflags = {target} -std=gnu99 -fms-extensions -mms-bitfields -fno-strict-aliasing -fwrapv -fno-math-errno -fshort-wchar "
        "-DBO1_MP -DNDEBUG -Wno-everything -ferror-limit=50 "
        f"-include {quote(os.path.join(PORT, 'guest', 'compat', 'msvc_prelude.h'))} "
        f"{incs([os.path.join(PORT, 'guest', 'compat'), SSE2NEON] + public_inc + [ENGINE, os.path.join(ENGINE, 'src'), os.path.join(ENGINE, 'src', 'libs'), os.path.join(ENGINE, 'src', 'libs', 'libtomcrypt-1.17', 'src', 'headers'), os.path.join(ENGINE, 'src', 'libs', 'libtommath-1.0'), clang_resource])}",
        "host_cflags = --target=aarch64-linux-android29 -fPIC -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers "
        f"{incs([os.path.join(PORT, 'include'), os.path.join(PORT, 'host'), glue, os.path.join(BUILD, 'gen'), os.path.join(OPENXR, 'include'), os.path.join(BUILD, 'third_party', 'ffmpeg-wma', 'include')])}",
        "",
        "rule guest_cc",
        "  command = $py $tools/guest_cc.py $clang $out $in $out.d -- $flags",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = guest $in",
        "rule guest_cxx",
        "  command = $py $tools/guest_cc.py $clangxx $out $in $out.d -- $flags",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = guest $in",
        "rule guest_as",
        "  command = $clang --target=aarch64-linux-android -mcpu=cortex-a76 -c $in -o $out",
        "  description = guest-as $in",
        "rule ar",
        f"  command = {quote(os.path.join(bin_, 'llvm-ar' + EXE))} rcs $out @$out.rsp",
        "  rspfile = $out.rsp",
        "  rspfile_content = $in",
        "  description = ar $out",
        "rule guest_link",
        f"  command = {quote(os.path.join(bin_, 'ld.lld' + EXE))} -static -nostdlib -z max-page-size=16384 -T $script --emit-relocs -o $out @$out.rsp",
        "  rspfile = $out.rsp",
        "  rspfile_content = $in $libs",
        "  description = link $out",
        "rule guest_relocs",
        f"  command = $py $tools/guest_relocs.py {quote(os.path.join(bin_, 'llvm-objcopy' + EXE))} $in $out",
        "  description = relocs $out",
        "rule imports",
        "  command = $py $tools/gen_imports.py $out $in",
        "rule gen_gl",
        "  command = $py $tools/gen_gl.py $header $in $out",
        "  description = gl bindings",
        "  description = imports",
        "rule host_cc",
        "  command = $clang $host_cflags -MD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = host $in",
        "rule host_link",
        f"  command = $clang --target=aarch64-linux-android29 -shared -o $out $in -landroid -llog -lm -lEGL -lGLESv3 -laaudio "
        f"-L{quote(os.path.join(BUILD, 'third_party', 'ffmpeg-wma', 'lib'))} -lavcodec -lavutil "
        f""
        f"-L{quote(os.path.join(OPENXR, 'lib'))} -lopenxr_loader "
        "-Wl,-u,ANativeActivity_onCreate -Wl,-z,max-page-size=16384 -Wl,--no-undefined",
        "  description = link $out",
        "rule apk",
        f"  command = $py $tools/package_apk.py --build-tools {quote(build_tools)} --android-jar {quote(android_jar)} "
        f"--manifest $manifest --lib $lib --extra-lib {quote(os.path.join(OPENXR, 'lib', 'libopenxr_loader.so'))} "
        "--guest $guest --out $out",
        "  description = apk $out",
        "",
    ]

    def obj(path, kind):
        return os.path.join(BUILD, "obj", kind, os.path.relpath(path, ROOT).replace("..", "_")) + ".o"

    # musl
    musl_objs = []
    for rel in musl_sources():
        src = os.path.join(MUSL, rel)
        o = os.path.join(BUILD, "obj", "musl", rel) + ".o"
        if rel.endswith(".s"):
            lines.append(f"build {ninja_path(o)}: guest_as {ninja_path(src)}")
        else:
            lines.append(f"build {ninja_path(o)}: guest_cc {ninja_path(src)}\n  flags = $musl_cflags")
        musl_objs.append(o)
    libc = os.path.join(BUILD, "lib", "libguestc.a")
    lines.append(f"build {ninja_path(libc)}: ar {' '.join(ninja_path(o) for o in musl_objs)}")

    # imports
    imports_list = os.path.join(PORT, "imports.list")
    guest_imports = os.path.join(BUILD, "gen", "guest_imports.s")
    host_imports = os.path.join(BUILD, "gen", "host_imports.c")
    gl_list = os.path.join(PORT, "gl.list")
    gl_imports = os.path.join(BUILD, "gen", "gl_imports.list")
    host_gl_wrappers = os.path.join(BUILD, "gen", "host_gl_wrappers.c")
    guest_gl_table = os.path.join(BUILD, "gen", "guest_gl_table.c")
    lines.append(f"build {ninja_path(gl_imports)} {ninja_path(host_gl_wrappers)} {ninja_path(guest_gl_table)}: gen_gl "
                 f"{ninja_path(gl_list)} | {ninja_path(os.path.join(tools, 'gen_gl.py'))}")
    lines.append(f"  header = {quote(os.path.join(GL_HEADERS, 'GLES3', 'gl32.h'))}")
    lines.append(f"build {ninja_path(guest_imports)} {ninja_path(host_imports)}: imports {ninja_path(imports_list)} {ninja_path(gl_imports)}"
                 f" | {ninja_path(os.path.join(tools, 'gen_imports.py'))}")

    # guest runtime + program
    guest_objs = []
    runtime = os.path.join(PORT, "guest", "runtime")
    for name in sorted(os.listdir(runtime)):
        src = os.path.join(runtime, name)
        if name.endswith(".c"):
            flags = "$runtime_cflags"
        elif name.endswith(".cpp"):
            flags = "$runtime_cxxflags"
        else:
            continue
        o = obj(src, "guest")
        lines.append(f"build {ninja_path(o)}: guest_cc {ninja_path(src)}\n  flags = {flags}")
        guest_objs.append(o)
    o = obj(guest_imports, "guest")
    lines.append(f"build {ninja_path(o)}: guest_as {ninja_path(guest_imports)}")
    guest_objs.append(o)
    smoke_objs = []
    smoke = os.path.join(PORT, "guest", "smoke")
    for name in sorted(os.listdir(smoke)):
        src = os.path.join(smoke, name)
        flags = "$smoke_cxxflags" if name.endswith(".cpp") else "$smoke_cflags"
        o = obj(src, "guest")
        lines.append(f"build {ninja_path(o)}: guest_cc {ninja_path(src)}\n  flags = {flags}")
        smoke_objs.append(o)

    script = os.path.join(PORT, "guest", "guest.ld")

    def guest_image(name, objs):
        """links objs with musl into build/<name>.elf, relocatable by the host (guest_relocs.py)"""
        elf = os.path.join(BUILD, name + ".elf")
        linked = os.path.join(BUILD, name + ".linked.elf")  # with --emit-relocs output
        lines.append(f"build {ninja_path(elf)}: guest_relocs {ninja_path(linked)}"
                     f" | {ninja_path(os.path.join(tools, 'guest_relocs.py'))}")
        lines.append(f"build {ninja_path(linked)}: guest_link {' '.join(ninja_path(o) for o in objs)}"
                     f" | {ninja_path(libc)} {ninja_path(script)}")
        lines.append(f"  script = {quote(script)}")
        lines.append(f"  libs = --start-group {quote(libc)} {quote(builtins)} --end-group")
        return elf

    # milestone 2's checks of the runtime alone: `ninja -C build smoke`
    smoke_elf = guest_image("bo1_smoke", guest_objs + smoke_objs)
    lines.append(f"build smoke: phony {ninja_path(smoke_elf)}")

    # host
    host_objs = []
    host_srcs = [os.path.join(PORT, "host", n) for n in sorted(os.listdir(os.path.join(PORT, "host"))) if n.endswith(".c")]
    host_srcs += [os.path.join(glue, "android_native_app_glue.c"), host_imports, host_gl_wrappers]
    for src in host_srcs:
        o = obj(src, "host") if src.startswith(ROOT) else os.path.join(BUILD, "obj", "host", os.path.basename(src) + ".o")
        lines.append(f"build {ninja_path(o)}: host_cc {ninja_path(src)}")
        host_objs.append(o)
    libmain = os.path.join(BUILD, "lib", "arm64-v8a", "libmain.so")
    lines.append(f"build {ninja_path(libmain)}: host_link {' '.join(ninja_path(o) for o in host_objs)}")

    # the engine
    engine_objs = []
    for rel in engine_sources():
        src = os.path.join(ENGINE, rel)
        o = os.path.join(BUILD, "obj", "engine", rel) + ".o"
        rule, flags = ("guest_cxx", "$engine_cxxflags") if rel.endswith(".cpp") else ("guest_cc", "$engine_cflags")
        lines.append(f"build {ninja_path(o)}: {rule} {ninja_path(src)}")
        lines.append(f"  flags = {flags}")
        engine_objs.append(o)
    libengine = os.path.join(BUILD, "lib", "libengine.a")
    lines.append(f"build {ninja_path(libengine)}: ar {' '.join(ninja_path(o) for o in engine_objs)}")
    lines.append(f"build engine: phony {ninja_path(libengine)}")

    # the Quest platform layer (port/guest/platform), built like the engine
    platform_objs = []
    platform = os.path.join(PORT, "guest", "platform")
    for name in sorted(os.listdir(platform)):
        if not name.endswith((".c", ".cpp")):
            continue
        src = os.path.join(platform, name)
        rule, flags = ("guest_cxx", "$engine_cxxflags") if name.endswith(".cpp") else ("guest_cc", "$engine_cflags")
        o = obj(src, "guest")
        lines.append(f"build {ninja_path(o)}: {rule} {ninja_path(src)}")
        lines.append(f"  flags = {flags}")
        platform_objs.append(o)

    # MojoShader and the guest's GL name table, for the D3D9 device
    for rel in ["mojoshader.c", "mojoshader_common.c", "mojoshader_opengl.c",
                "profiles/mojoshader_profile_common.c", "profiles/mojoshader_profile_glsl.c"]:
        src = os.path.join(MOJOSHADER, rel)
        o = os.path.join(BUILD, "obj", "mojoshader", rel) + ".o"
        lines.append(f"build {ninja_path(o)}: guest_cc {ninja_path(src)}")
        lines.append("  flags = $mojo_cflags")
        platform_objs.append(o)
    o = os.path.join(BUILD, "obj", "guest", "guest_gl_table.c.o")
    lines.append(f"build {ninja_path(o)}: guest_cc {ninja_path(guest_gl_table)}")
    lines.append("  flags = $gltable_cflags")
    platform_objs.append(o)

    # the game: runtime + platform layer + every engine object (whole objects, so nothing that
    # registers itself by construction is dropped)
    guest_elf = guest_image("bo1_guest", guest_objs + platform_objs + engine_objs)

    # apk
    manifest = os.path.join(PORT, "android", "AndroidManifest.xml")
    apk = os.path.join(BUILD, "bo1-quest.apk")
    lines.append(f"build {ninja_path(apk)}: apk | {ninja_path(manifest)} {ninja_path(libmain)} {ninja_path(guest_elf)}"
                 f" {ninja_path(os.path.join(tools, 'package_apk.py'))}")
    lines.append(f"  manifest = {quote(manifest)}")
    lines.append(f"  lib = {quote(libmain)}")
    lines.append(f"  guest = {quote(guest_elf)}")
    lines.append(f"build guest: phony {ninja_path(guest_elf)}")
    lines.append(f"default {ninja_path(apk)}")


    write_if_changed(os.path.join(BUILD, "build.ninja"), "\n".join(lines) + "\n")
    print(f"wrote {os.path.join(BUILD, 'build.ninja')} ({len(musl_objs)} musl, {len(engine_objs)} engine files); now run: ninja -C build")


if __name__ == "__main__":
    main()
