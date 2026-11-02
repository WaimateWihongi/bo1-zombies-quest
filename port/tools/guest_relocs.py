"""Makes bo1_guest.elf loadable at any 64 KB-aligned address below 4 GB.

The guest is linked at a nominal base (guest.ld) with --emit-relocs. Code reaches everything in
the image PC-relatively (asm_convert.py relaxes GOT loads to ADRP/ADD), so when the whole image
moves together only absolute pointer words need fixing: R_AARCH64_ABS32 and R_AARCH64_ABS64.
This script lists their image offsets in a `.guest_relocs` section and drops the .rela sections:

    uint32 magic 'GRL1', uint32 abs32_count, uint32 abs64_count,
    uint32 offsets[abs32_count], uint32 offsets[abs64_count]   (offsets from the link base)

The host adds (load address - link base) to each word before it runs any guest code.

usage: guest_relocs.py <llvm-objcopy> <linked.elf> <out.elf>
"""
import os
import struct
import subprocess
import sys

R_AARCH64_ABS64 = 257
R_AARCH64_ABS32 = 258
# absolute forms that this scheme can't fix; the guest compiler should never produce them
UNSUPPORTED = {259: "ABS16", 263: "MOVW_UABS_G0", 264: "MOVW_UABS_G0_NC", 265: "MOVW_UABS_G1",
               266: "MOVW_UABS_G1_NC", 267: "MOVW_UABS_G2", 268: "MOVW_UABS_G2_NC", 269: "MOVW_UABS_G3"}
SHT_SYMTAB, SHT_RELA = 2, 4
SHF_ALLOC = 2
SHN_UNDEF, SHN_ABS = 0, 0xFFF1
PT_LOAD = 1


def main():
    objcopy, src, out = sys.argv[1:4]
    data = open(src, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        sys.exit(f"{src}: not an ELF64 file")
    (e_phoff, e_shoff) = struct.unpack_from("<QQ", data, 0x20)
    (e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHHHH", data, 0x36)

    base = min(struct.unpack_from("<Q", data, e_phoff + i * e_phentsize + 0x10)[0]
               for i in range(e_phnum)
               if struct.unpack_from("<I", data, e_phoff + i * e_phentsize)[0] == PT_LOAD)

    sections = []
    for i in range(e_shnum):
        name, type_, flags, addr, offset, size, link, info, _align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", data, e_shoff + i * e_shentsize)
        sections.append(dict(name=name, type=type_, flags=flags, addr=addr, offset=offset, size=size,
                             link=link, info=info, entsize=entsize))
    strtab = sections[e_shstrndx]

    def section_name(s):
        start = strtab["offset"] + s["name"]
        return data[start:data.index(b"\0", start)].decode()

    abs32, abs64, problems = [], [], []
    rela_names = []
    symtabs = {}
    for s in sections:
        if s["type"] != SHT_RELA:
            continue
        target = sections[s["info"]]
        rela_names.append(section_name(s))
        if not target["flags"] & SHF_ALLOC:
            continue  # debug info and the like: never loaded
        if s["link"] not in symtabs:
            sym = sections[s["link"]]
            symtabs[s["link"]] = [struct.unpack_from("<IBBHQQ", data, sym["offset"] + j * 24)
                                  for j in range(sym["size"] // 24)]
        symbols = symtabs[s["link"]]
        for j in range(s["size"] // 24):
            r_offset, r_info, _addend = struct.unpack_from("<QQq", data, s["offset"] + j * 24)
            kind, sym_index = r_info & 0xFFFFFFFF, r_info >> 32
            if kind not in (R_AARCH64_ABS32, R_AARCH64_ABS64) and kind not in UNSUPPORTED:
                continue  # PC-relative: moves with the image
            shndx = symbols[sym_index][3] if sym_index else SHN_ABS
            if shndx in (SHN_UNDEF, SHN_ABS):
                continue  # a constant or an unresolved weak symbol (0): not an image address
            # with --emit-relocs, r_offset in an executable is the final virtual address
            if kind in UNSUPPORTED:
                problems.append(f"{UNSUPPORTED[kind]} at {r_offset:#x} in {section_name(target)}")
            elif kind == R_AARCH64_ABS32:
                abs32.append(r_offset - base)
            else:
                abs64.append(r_offset - base)
    if problems:
        sys.exit("guest_relocs: absolute relocations the loader can't move:\n  " + "\n  ".join(problems[:20]))

    table = struct.pack("<4sII", b"GRL1", len(abs32), len(abs64))
    table += struct.pack(f"<{len(abs32)}I", *sorted(abs32)) + struct.pack(f"<{len(abs64)}I", *sorted(abs64))
    table_path = out + ".relocs"
    with open(table_path, "wb") as f:
        f.write(table)
    command = [objcopy]
    for name in sorted(set(rela_names)):
        command.append(f"--remove-section={name}")
    command += [f"--add-section=.guest_relocs={table_path}", src, out]
    subprocess.check_call(command)
    os.remove(table_path)
    print(f"guest_relocs: {len(abs32)} ABS32 + {len(abs64)} ABS64 pointer words (link base {base:#x})")


if __name__ == "__main__":
    main()
