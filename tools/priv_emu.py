#!/usr/bin/env python3
"""priv_emu.py -- a tiny Unicorn-based harness to *execute* fragments of the
user's own PRCD.EXE (16-bit real mode) for clean-room asset extraction.

CLEAN-ROOM / ASSET-TIME tool. Reads the user's own legal PRCD.EXE; produces
only local artifacts. Used to recover the CONV/*.VPK speech codec by running
the game's own decompressor instead of guessing it (see
docs/bar_speech_vpk_format.md).

Design
------
* The whole load module (file >= 0xae00) is mapped into emulated memory at a
  fixed paragraph base BASE_SEG, so the docs' flat convention
  `seg = (file_off - 0xae00) >> 4` maps a file offset to a runtime address of
  (BASE_SEG + seg):off.  MZ relocations are applied for the resident image.
* `call_far(file_off, args, ...)` pushes a sentinel return address, sets up the
  stack with C-style word args, and single-steps until control returns to the
  sentinel (or a hook fires).  Good for leaf functions (no DOS/overlay calls).
"""
from __future__ import annotations

import struct
from pathlib import Path

from unicorn import (
    Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED,
)
from unicorn.x86_const import (
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
    UC_X86_REG_SP, UC_X86_REG_BP, UC_X86_REG_IP, UC_X86_REG_AX,
    UC_X86_REG_BX, UC_X86_REG_FLAGS,
)

LOAD_FILE_START = 0xAE00          # e_cparhdr*16; image-seg 0 lives here
RESIDENT_END = 0x7FF60            # overlay pool begins here
BASE_SEG = 0x0100                 # image-seg 0 -> linear 0x1000 (low, fits <1MB)
STACK_SEG = 0xE000               # stack lives at 0xE0000..0xEFFF0
SENTINEL_LIN = 0xF0000           # far-return sentinel (seg 0xF000:0), mapped


class PrivEmu:
    def __init__(self, exe_path: str = "re/PRCD.EXE", mode: str = "resident"):
        # mode "resident": map file 0xAE00..0x7FF60 (resident image, relocs applied)
        # mode "overlay":  map the overlay pool 0x7FF60..EOF at linear 0x1000
        #                  (for leaf-function emulation; no relocs)
        self.data = bytearray(Path(exe_path).read_bytes())
        self.mode = mode
        self._parse_mz()
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self._map_image()
        if mode == "resident":
            self._apply_relocs()
        else:
            self.nrelocs = 0

    @property
    def pool_file_start(self):
        return RESIDENT_END

    # ---- MZ parsing --------------------------------------------------------
    def _parse_mz(self):
        d = self.data
        assert d[:2] == b"MZ"
        (self.e_cblp, self.e_cp, self.e_crlc, self.e_cparhdr) = struct.unpack_from("<HHHH", d, 2)
        (self.e_ss, self.e_sp, _csum, self.e_ip, self.e_cs, self.e_lfarlc) = struct.unpack_from(
            "<HHHHHH", d, 0x0E)
        self.load_start = self.e_cparhdr * 16          # == 0xAE00
        self.image = self.data[self.load_start:]

    # ---- memory layout -----------------------------------------------------
    def lin(self, seg, off):
        return ((seg & 0xFFFF) << 4) + (off & 0xFFFF)

    def file_to_addr(self, file_off):
        """Map a PRCD.EXE *file* offset to an emulated linear address."""
        base = getattr(self, "blob_file_base", self.load_start)
        return self.lin(BASE_SEG, 0) + (file_off - base)

    def _map_image(self):
        # Real mode can only address ~1MB, and the full file (893KB) + buffers
        # won't fit. So map exactly 1MB and write only the RESIDENT image
        # (file 0xAE00..0x7FF60) at linear 0x1000; overlay windows are copied
        # into the heap on demand via map_code_window().
        self.mapped = 0x100000
        self.uc.mem_map(0, self.mapped)
        base = self.lin(BASE_SEG, 0)                     # 0x1000
        if self.mode == "overlay":
            blob = self.data[RESIDENT_END:]              # overlay pool
            self.blob_file_base = RESIDENT_END
        else:
            blob = self.data[self.load_start:RESIDENT_END]
            self.blob_file_base = self.load_start
        self.uc.mem_write(base, bytes(blob))
        self.resident_len = len(blob)
        # Heap (buffers) sits above the resident image, below the stack.
        self.heap0 = (base + self.resident_len + 0x1000) & ~0xF
        self.heap = self.heap0
        self.heap_top = self.lin(STACK_SEG, 0)           # don't grow into stack
        self.sentinel_lin = SENTINEL_LIN

    def _apply_relocs(self):
        """Add BASE_SEG to every relocated segment word in the resident image."""
        d = self.data
        off = self.e_lfarlc
        n = 0
        for _ in range(self.e_crlc):
            r_off, r_seg = struct.unpack_from("<HH", d, off)
            off += 4
            file_target = self.load_start + r_seg * 16 + r_off
            if file_target >= RESIDENT_END - 1:
                continue                       # reloc lands in unmapped overlay area
            addr = self.file_to_addr(file_target)
            cur = struct.unpack("<H", self.uc.mem_read(addr, 2))[0]
            self.uc.mem_write(addr, struct.pack("<H", (cur + BASE_SEG) & 0xFFFF))
            n += 1
        self.nrelocs = n

    # ---- simple allocator --------------------------------------------------
    def alloc(self, data_or_size):
        if isinstance(data_or_size, (bytes, bytearray)):
            size = len(data_or_size)
        else:
            size = int(data_or_size)
        a = self.heap
        self.heap = (self.heap + size + 0xF) & ~0xF
        if isinstance(data_or_size, (bytes, bytearray)):
            self.uc.mem_write(a, bytes(data_or_size))
        else:
            self.uc.mem_write(a, b"\x00" * size)
        return a  # linear address

    def addr_to_segoff(self, addr):
        """Express a linear addr as seg:off using BASE_SEG-relative paragraphs."""
        seg = (addr >> 4) & 0xFFFF
        off = addr & 0xF
        return seg, off

    # ---- calling a far function -------------------------------------------
    def call_far(self, file_off, args=(), ds=None, max_insns=2_000_000,
                 trace=False, watch_src=None):
        """watch_src=(lin,len): if given, returns stats dict capturing the
        write address range, total writes, and whether `src` was read."""
        uc = self.uc
        stack_seg = STACK_SEG
        sp = 0xFFF0
        uc.reg_write(UC_X86_REG_SS, stack_seg)
        uc.reg_write(UC_X86_REG_SP, sp)
        uc.reg_write(UC_X86_REG_BP, sp)

        code_lin = self.file_to_addr(file_off)
        cs = (code_lin >> 4) & 0xFFFF
        ip = code_lin & 0xF
        uc.reg_write(UC_X86_REG_CS, cs)
        uc.reg_write(UC_X86_REG_IP, ip)
        if ds is None:
            ds = BASE_SEG
        uc.reg_write(UC_X86_REG_DS, ds)
        uc.reg_write(UC_X86_REG_ES, ds)

        # Push C args (right-to-left) then a sentinel far return (off, seg).
        def push16(v):
            nonlocal sp
            sp = (sp - 2) & 0xFFFF
            uc.mem_write(self.lin(stack_seg, sp), struct.pack("<H", v & 0xFFFF))
        for a in reversed(args):
            push16(a)
        sent_seg = (self.sentinel_lin >> 4) & 0xFFFF
        sent_off = self.sentinel_lin & 0xF
        push16(sent_seg)                     # ret CS (pushed first / higher addr)
        push16(sent_off)                     # ret IP (top of stack)
        uc.reg_write(UC_X86_REG_SP, sp)

        bad = {"err": None}
        def memhook(uc_, access, address, size_, value, _ud):
            bad["err"] = f"unmapped mem @ {address:#x} access={access}"
            uc_.emu_stop()
            return False
        hm = uc.hook_add(UC_HOOK_MEM_UNMAPPED, memhook)
        hw = None
        stats = None
        if watch_src is not None:
            from unicorn import UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
            s_lin, s_len = watch_src
            stats = {"wmin": None, "wmax": None, "nw": 0, "src_read": False}
            def wh(uc_, access, address, size_, value, _ud):
                st = stats
                st["nw"] += 1
                if st["wmin"] is None or address < st["wmin"]:
                    st["wmin"] = address
                end = address + size_
                if st["wmax"] is None or end > st["wmax"]:
                    st["wmax"] = end
                return True
            def rh(uc_, access, address, size_, value, _ud):
                if s_lin <= address < s_lin + s_len:
                    stats["src_read"] = True
                return True
            hw = uc.hook_add(UC_HOOK_MEM_WRITE, wh)
            hr = uc.hook_add(UC_HOOK_MEM_READ, rh)
        h = None
        if trace:
            def hook(uc_, address, sizei, _ud):
                cur_cs = uc_.reg_read(UC_X86_REG_CS)
                ip_ = uc_.reg_read(UC_X86_REG_IP)
                print(f"  {cur_cs:04x}:{ip_:04x}")
            h = uc.hook_add(UC_HOOK_CODE, hook)

        try:
            # Native `until` stop at the sentinel == far return (no per-insn cb).
            uc.emu_start(self.lin(cs, ip), self.sentinel_lin, count=max_insns)
        finally:
            if h is not None:
                uc.hook_del(h)
            uc.hook_del(hm)
            if hw is not None:
                uc.hook_del(hw)
                uc.hook_del(hr)
        if bad["err"] and watch_src is None:
            raise RuntimeError(bad["err"])
        cur = self.lin(uc.reg_read(UC_X86_REG_CS), uc.reg_read(UC_X86_REG_IP))
        hit = (cur == self.sentinel_lin)
        ax = uc.reg_read(UC_X86_REG_AX)
        if watch_src is not None:
            stats["hit"] = hit
            stats["faulted"] = bad["err"] is not None
            return ax, stats
        return ax, hit


if __name__ == "__main__":
    # Self-test: emulate the trivial resident getter at file 0x14461:
    #   push bp; mov bp,sp; push si; mov si,[bp+6]; mov ax,[si+0x31]; ...; retf
    # We pass a far pointer to a struct whose +0x31 word = 0x1234 and expect AX.
    emu = PrivEmu()
    print(f"[priv_emu] mapped image, applied {emu.nrelocs} relocs, "
          f"heap @ {emu.heap:#x}")
    obj = emu.alloc(0x40)
    emu.uc.mem_write(obj + 0x31, struct.pack("<H", 0x1234))
    seg, off = emu.addr_to_segoff(obj)        # near ptr: DS=seg, arg=off
    ax, hit = emu.call_far(0x14461, args=(off,), ds=seg)
    print(f"[selftest] getter@0x14461 returned AX={ax:#06x} hit={hit} "
          f"(expect 0x1234)")
