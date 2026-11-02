"""Rewrite clang's arm64_32 (Apple ILP32) assembly as AArch64 ELF assembly.

The guest is compiled with --target=arm64_32-apple-watchos, the only clang target that emits
AArch64 instructions with 32-bit pointers (the engine's fastfile structs depend on 4-byte
pointers). The instructions are plain AArch64; only Mach-O syntax differs: section names,
the leading underscore on symbols, assembler-local labels (l/L prefixes), relocation
operators (@PAGE, @PAGEOFF, @GOTPAGE...) and a few directives. This script translates those
so `clang --target=aarch64-linux-android -c` can assemble the result for ld.lld.

Usage: asm_convert.py in.s out.s
"""

import re
import sys

QUOTED = r'"(?:[^"\\]|\\.)*"'
TOKEN = re.compile(QUOTED + r'|[A-Za-z_.$][\w.$]*')
LABEL = re.compile(r'^(' + QUOTED + r'|[A-Za-z_.$][\w.$]*):(.*)$')

DROP = {
    ".subsections_via_symbols", ".build_version", ".loh", ".data_region", ".end_data_region",
    ".linker_option", ".indirect_symbol", ".addrsig", ".addrsig_sym", ".no_dead_strip",
    ".alt_entry", ".ident", ".desc", ".watchos_version_min", ".ios_version_min",
    ".macosx_version_min", ".tvos_version_min",
}

RODATA = {"__cstring", "__const", "__literal4", "__literal8", "__literal16", "__ustring"}

SHORTHAND = {
    ".text": ("__TEXT", "__text"), ".data": ("__DATA", "__data"), ".const": ("__TEXT", "__const"),
    ".cstring": ("__TEXT", "__cstring"), ".literal4": ("__TEXT", "__literal4"),
    ".literal8": ("__TEXT", "__literal8"), ".literal16": ("__TEXT", "__literal16"),
    ".const_data": ("__DATA", "__const"), ".static_data": ("__DATA", "__data"),
    ".mod_init_func": ("__DATA", "__mod_init_func"), ".mod_term_func": ("__DATA", "__mod_term_func"),
}

GOT_LOAD = re.compile(r'^([wx])(\d+)\s*,\s*\[\s*(x\d+|sp)\s*,\s*(' + QUOTED + r'|[\w.$]+)@GOTPAGEOFF\s*\]$')


class Error(Exception):
    pass


def strip_comment(line):
    """Drop a Darwin ';' or '//' comment, leaving string literals intact."""
    quote = False
    i = 0
    while i < len(line):
        c = line[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == '"':
                quote = False
        elif c == '"':
            quote = True
        elif c == ";" or line.startswith("//", i):
            return line[:i].rstrip()
        i += 1
    return line.rstrip()


class Converter:
    def __init__(self, lines):
        self.lines = [strip_comment(l).strip() for l in lines]
        self.out = []
        self.dropped = False
        self.text = False
        self.locals = set()
        self.weak = set()
        for l in self.lines:
            m = LABEL.match(l)
            if m and m.group(1)[0] in "lL":
                self.locals.add(m.group(1))

    def sym(self, name):
        if name.startswith('"'):
            inner = name[1:-1]
            return '"' + (inner[1:] if inner.startswith("_") else inner) + '"'
        if name in self.locals:
            return ".L" + name
        if name.startswith("_"):
            return name[1:]
        return name

    def operands(self, text):
        text = re.sub(r'(' + QUOTED + r'|[\w.$]+)@GOTPAGE\b', r'\1', text)
        text = re.sub(r'(' + QUOTED + r'|[\w.$]+)@PAGE\b', r'\1', text)
        text = re.sub(r'(\([^)]*\)|' + QUOTED + r'|[\w.$]+(?:[+-]\d+)?)@PAGEOFF\b', r':lo12:\1', text)
        if "@" in re.sub(QUOTED, '""', text):
            raise Error("unsupported relocation operator: " + text)

        def rename(m):
            tok = m.group(0)
            if tok.startswith(".") or tok == "lo12":
                return tok
            return self.sym(tok)

        return "".join(p if p == ":lo12:" else TOKEN.sub(rename, p) for p in re.split(r'(:lo12:)', text))

    def emit(self, s):
        self.out.append(s)

    def section(self, seg, name):
        self.dropped = seg in ("__DWARF", "__LD") or name in ("__compact_unwind", "__eh_frame",
                                                              "__gcc_except_tab")
        self.text = False
        if self.dropped:
            return
        if name == "__guest_header":
            self.emit('\t.section .guest_header,"a",@progbits')
        elif name == "__mod_init_func":
            self.emit('\t.section .guest_init,"aw",@progbits')
        elif name == "__mod_term_func":
            self.emit('\t.section .guest_fini,"aw",@progbits')
        elif seg == "__TEXT" and name in RODATA:
            self.emit('\t.section .rodata,"a",@progbits')
        elif seg == "__TEXT":
            self.emit("\t.text")
            self.text = True
        elif name in ("__bss", "__common"):
            self.emit("\t.bss")
        else:
            self.emit("\t.data")

    def line(self, s):
        if not s:
            return
        m = LABEL.match(s)
        if m:
            if not self.dropped:
                name = self.sym(m.group(1))
                if self.text and not name.startswith(".L"):
                    self.emit(f"\t.type {name},%function")
                self.emit(name + ":")
            self.line(m.group(2).strip())
            return
        m = re.match(r'^([A-Za-z_.$][\w.$]*)\s*=\s*(.+)$', s)
        if m:
            if not self.dropped:
                self.emit(f"\t.set {self.sym(m.group(1))}, {self.operands(m.group(2))}")
            return
        parts = s.split(None, 1)
        op, args = parts[0], parts[1].strip() if len(parts) > 1 else ""

        if op == ".section":
            seg, _, rest = args.partition(",")
            self.section(seg.strip(), rest.split(",")[0].strip())
            return
        if op in SHORTHAND:
            self.section(*SHORTHAND[op])
            return
        if op == ".zerofill":
            parts = [p.strip() for p in args.split(",")]
            if len(parts) >= 4:
                self.emit("\t.pushsection .bss")
                self.emit(f"\t.p2align {parts[4] if len(parts) > 4 else 0}")
                self.emit(f"{self.sym(parts[2])}:")
                self.emit(f"\t.zero {parts[3]}")
                self.emit("\t.popsection")
            return
        if self.dropped or op in DROP or op.startswith(".cfi_"):
            return
        if op in (".tbss", ".tlv", ".tdata") or "@TLVP" in args:
            raise Error("native TLS is not supported (build with -femulated-tls): " + s)
        if op == ".globl":
            self.emit(f"\t.globl {self.sym(args)}")
        elif op == ".private_extern":
            self.emit(f"\t.hidden {self.sym(args)}")
        elif op in (".weak_definition", ".weak_reference", ".weak_def_can_be_hidden", ".weak"):
            self.weak.add(self.sym(args))
            self.emit(f"\t.weak {self.sym(args)}")
        elif op in (".comm", ".lcomm"):
            a = [x.strip() for x in args.split(",")]
            align = 1 << int(a[2]) if len(a) > 2 else 1
            if op == ".lcomm":
                self.emit(f"\t.local {self.sym(a[0])}")
            self.emit(f"\t.comm {self.sym(a[0])},{a[1]},{align}")
        elif op == ".align":
            self.emit(f"\t.p2align {args}")
        elif op in (".ascii", ".asciz", ".string"):
            self.emit(f"\t{op} {args}")
        elif op.startswith("."):
            self.emit(f"\t{op} {self.operands(args)}" if args else f"\t{op}")
        elif "@GOTPAGEOFF" in args:
            # The image is static and below 4 GB: a GOT load relaxes to the address itself.
            g = GOT_LOAD.match(args)
            if op != "ldr" or not g:
                raise Error("unsupported GOT access: " + s)
            self.emit(f"\tadd x{g.group(2)}, {g.group(3)}, :lo12:{self.sym(g.group(4))}")
        else:
            self.emit(f"\t{op} {self.operands(args)}" if args else f"\t{op}")

    def run(self):
        for n, l in enumerate(self.lines, 1):
            try:
                self.line(l)
            except Error as e:
                raise Error(f"line {n}: {e}") from None
        # a weak symbol is already global; a second .globl only draws a warning
        return "\n".join(l for l in self.out
                         if not (l.startswith("\t.globl ") and l[8:] in self.weak)) + "\n"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding="utf-8", errors="surrogateescape") as f:
        lines = f.read().split("\n")
    try:
        text = Converter(lines).run()
    except Error as e:
        sys.exit(f"{sys.argv[1]}: {e}")
    with open(sys.argv[2], "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
