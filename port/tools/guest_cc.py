"""Compile one guest source file to an ELF object.

  1. clang --target=arm64_32-apple-watchos -S   (ILP32 AArch64 code, Mach-O assembly)
  2. asm_convert.py                              (Mach-O assembly -> ELF assembly)
  3. clang --target=aarch64-linux-android -c     (assemble)

Usage: guest_cc.py CLANG OUT.o SRC DEPFILE -- COMPILE_FLAGS...
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asm_convert  # noqa: E402


def main():
    if len(sys.argv) < 6 or sys.argv[5] != "--":
        sys.exit(__doc__)
    clang, out, src, dep = sys.argv[1:5]
    flags = sys.argv[6:]
    darwin = out + ".darwin.s"
    elf = out + ".s"
    r = subprocess.run([clang, *flags, "-S", src, "-o", darwin, "-MD", "-MF", dep])
    if r.returncode:
        sys.exit(r.returncode)
    with open(darwin, encoding="utf-8", errors="surrogateescape") as f:
        lines = f.read().split("\n")
    try:
        text = asm_convert.Converter(lines).run()
    except asm_convert.Error as e:
        sys.exit(f"{src}: {e}")
    with open(elf, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write(text)
    r = subprocess.run([clang, "--target=aarch64-linux-android", "-mcpu=cortex-a76", "-c", elf, "-o", out])
    if r.returncode:
        sys.exit(r.returncode)
    os.remove(darwin)
    os.remove(elf)


if __name__ == "__main__":
    main()
