"""Package libmain.so and bo1_guest.elf into a signed APK (no Gradle: aapt2, zipalign, apksigner).

The APK is signed with ~/.android/debug.keystore (made with keytool on first use), so builds
from this machine install over each other without losing the app's data.

Usage: package_apk.py --build-tools DIR --android-jar JAR --manifest XML --lib SO [--extra-lib SO]... --guest ELF --out APK
"""

import argparse
import os
import shutil
import subprocess
import sys
import zipfile

EXE = ".exe" if os.name == "nt" else ""
BAT = ".bat" if os.name == "nt" else ""


def run(*cmd):
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(f"failed ({r.returncode}): {' '.join(cmd)}")


def debug_keystore():
    path = os.path.join(os.path.expanduser("~"), ".android", "debug.keystore")
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        keytool = shutil.which("keytool")
        if not keytool and os.environ.get("JAVA_HOME"):
            keytool = os.path.join(os.environ["JAVA_HOME"], "bin", "keytool" + EXE)
        if not keytool:
            sys.exit("keytool not found (install a JDK) to make the debug keystore")
        run(keytool, "-genkeypair", "-keystore", path, "-storepass", "android", "-alias", "androiddebugkey",
            "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
            "-dname", "CN=Android Debug,O=Android,C=US")
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-tools", required=True)
    ap.add_argument("--android-jar", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--extra-lib", action="append", default=[], help="another native library, e.g. the OpenXR loader")
    ap.add_argument("--guest", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    base = a.out + ".base"
    aligned = a.out + ".aligned"
    run(os.path.join(a.build_tools, "aapt2" + EXE), "link", "-o", base, "--manifest", a.manifest,
        "-I", a.android_jar)
    with zipfile.ZipFile(base, "a") as z:
        # stored, so Android can map the library straight from the APK and read the image fast
        z.write(a.lib, "lib/arm64-v8a/libmain.so", compress_type=zipfile.ZIP_STORED)
        for extra in a.extra_lib:
            z.write(extra, "lib/arm64-v8a/" + os.path.basename(extra), compress_type=zipfile.ZIP_STORED)
        z.write(a.guest, "assets/bo1_guest.elf", compress_type=zipfile.ZIP_STORED)
    run(os.path.join(a.build_tools, "zipalign" + EXE), "-f", "-P", "16", "4", base, aligned)
    run(os.path.join(a.build_tools, "apksigner" + BAT), "sign", "--ks", debug_keystore(), "--ks-pass",
        "pass:android", "--key-pass", "pass:android", "--out", a.out, aligned)
    os.remove(base)
    os.remove(aligned)
    if os.path.exists(a.out + ".idsig"):
        os.remove(a.out + ".idsig")


if __name__ == "__main__":
    main()
