#!/usr/bin/env python3
"""VFP short-vector -> scalar translator for libSpyMouse.so (EA audio mixer).

The mixer is hand-written ARM VFPv2 code that runs in short-vector mode: it sets FPSCR.LEN (vmrs / bic #0x370000 / orr #LEN<<16 /
vmsr) and then executes ordinary-looking VFP data-processing instructions that the hardware repeats LEN times over register banks.
The Vita's Cortex-A9 has no vector mode (such an instruction is UNDEFINED), and old Android kernels used to emulate it.

This tool finds every such region statically (the LEN is an immediate), and for each vector-capable instruction inside it emits
LEN scalar instructions into a code cave and replaces the original with a branch to the cave:

    original:   vmul.f32 s8, s8, s0     (LEN=4)         ->   b cave_k
    cave_k:     vmul.f32 s8,  s8,  s0
                vmul.f32 s9,  s9,  s0
                vmul.f32 s10, s10, s0
                vmul.f32 s11, s11, s0
                b original+4

The `orr rX, rX, #LEN<<16` that would put the hardware into vector mode becomes a nop.

Bank rules (ARM ARM, VFPv2): single-precision registers s0-s7 are the scalar bank, s8-s15, s16-s23, s24-s31 are vector banks of 8;
double-precision d0-d3 scalar, d4-d7, d8-d11, d12-d15 vector banks of 4. If the destination is in the scalar bank the operation is
scalar. Otherwise the operation repeats LEN times; destination and first source step through their bank (wrapping, by STRIDE),
and the second source (Fm) is fixed when it is in the scalar bank, else it steps too.

usage:  vfpvec.py analyze <so>
        vfpvec.py gen <so> <out.c> [cave_vaddr]
"""
import re, subprocess, sys, struct, os, collections

TEXT_END = 0x2ae158
OBJDUMP = "arm-vita-eabi-objdump"
LINE = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)\s*(.*)$')


def load(so):
    out = subprocess.check_output([OBJDUMP, "-d", so], text=True)
    ins = []
    for l in out.splitlines():
        m = LINE.match(l)
        if m:
            a = int(m.group(1), 16)
            if a < TEXT_END:
                ins.append((a, int(m.group(2), 16), m.group(3), m.group(4)))
    return ins


def find_sites(ins):
    """FPSCR writes: list of (addr, kind, L, stride, orr_addr). kind 'set' has an orr immediate; 'clear' does not."""
    sites = []
    for i, (a, w, mn, ops) in enumerate(ins):
        if mn == 'vmsr' and ops.startswith('fpscr'):
            reg = ops.split(',')[1].strip().split()[0]
            L = 1; stride = 1; orr_addr = None
            for j in range(max(0, i - 4), i):
                aj, wj, mj, oj = ins[j]
                m = re.match(r'(\w+),\s*(\w+),\s*#(\d+)', oj)
                if mj == 'orr' and m and m.group(1) == reg and m.group(2) == reg:
                    imm = int(m.group(3))
                    if imm & 0x370000:
                        L = ((imm >> 16) & 7) + 1; stride = ((imm >> 20) & 3) + 1; orr_addr = aj
            sites.append((a, 'set' if orr_addr else 'clear', L, stride, orr_addr, ins[i][1]))
    return sites


def regions(sites):
    regs = []; cur = None; warn = []
    for a, kind, L, stride, orr_addr, w in sites:
        if kind == 'set':
            if cur: warn.append("set at %#x while region from %#x still open" % (a, cur[0]))
            cur = [a, None, L, stride, orr_addr]
        else:
            if cur:
                cur[1] = a; regs.append(tuple(cur)); cur = None
            # a clear without an open region is a duplicate epilogue; ignore
    if cur: warn.append("region from %#x never closed" % cur[0])
    return regs, warn


# ---- VFP data-processing decode -----------------------------------------------------------------------------------------
def dp_decode(w):
    """Return dict for a VFP data-processing instruction, or None. cond is kept."""
    if (w & 0x0F000E10) != 0x0E000A00:
        return None
    dbl = (w >> 8) & 1
    p, D, q, r = (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    Fn, Fd, N, s, M, Fm = (w >> 16) & 15, (w >> 12) & 15, (w >> 7) & 1, (w >> 6) & 1, (w >> 5) & 1, w & 15
    if dbl:
        if D or N or M: return None                     # VFPv3 D32 registers: not in this code
        d, n, m = Fd, Fn, Fm
    else:
        d, n, m = (Fd << 1) | D, (Fn << 1) | N, (Fm << 1) | M
    key = (p, q, r)
    three = {(0, 0, 0): 'mac', (0, 0, 1): 'msc', (0, 1, 0): 'mul', (0, 1, 1): 'add', (1, 0, 0): 'div'}
    if key in three:
        return dict(kind='3op', op=three[key], s=s, dbl=dbl, d=d, n=n, m=m, cond=w >> 28)
    if key == (1, 1, 1):
        # extension space, selected by the Fn field (the "opcode2")
        if Fn in (0, 1):                                 # vmov/vabs, vneg/vsqrt
            return dict(kind='2op', op='ext%d%d' % (Fn, s), dbl=dbl, d=d, m=m, cond=w >> 28, fn=Fn)
        if Fn == 8 or Fn in (12, 13):                    # int->float, float->int
            return dict(kind='2op', op='cvt%d' % Fn, dbl=dbl, d=d, m=m, cond=w >> 28, fn=Fn)
        return dict(kind='other', op='ext%d' % Fn, dbl=dbl, cond=w >> 28, fn=Fn)
    return dict(kind='other', op='?', dbl=dbl, cond=w >> 28)


def bank_size(dbl): return 4 if dbl else 8


def elem(reg, i, stride, dbl):
    bs = bank_size(dbl); base = (reg // bs) * bs
    return base + ((reg % bs) + i * stride) % bs


def encode_dp(w, d=None, n=None, m=None):
    """Return w with the register fields replaced (single-precision numbering unless the instruction is double)."""
    dbl = (w >> 8) & 1
    def put(w, reg, lo, hi_bit_pos, lo_bits):
        return w
    if dbl:
        if d is not None: w = (w & ~(0xF << 12)) | (d << 12)
        if n is not None: w = (w & ~(0xF << 16)) | (n << 16)
        if m is not None: w = (w & ~0xF) | m
    else:
        if d is not None: w = (w & ~((0xF << 12) | (1 << 22))) | ((d >> 1) << 12) | ((d & 1) << 22)
        if n is not None: w = (w & ~((0xF << 16) | (1 << 7))) | ((n >> 1) << 16) | ((n & 1) << 7)
        if m is not None: w = (w & ~(0xF | (1 << 5))) | (m >> 1) | ((m & 1) << 5)
    return w


def expand(w, L, stride):
    """Scalar instruction words equivalent to the vector-mode instruction w under LEN=L, or None if it is scalar anyway / unknown."""
    dd = dp_decode(w)
    if dd is None or dd['kind'] == 'other':
        return None if dd is None else ('unsupported', dd)
    dbl = dd['dbl']; bs = bank_size(dbl)
    if dd['kind'] == '2op' and dd['op'].startswith('cvt'):
        return None                                      # int<->float conversions are always scalar (Linux vfp: OP_SCALAR); expanding
                                                         # them re-converted already converted registers (noisy audio, test 9)
    if dd['d'] < bs:                                     # destination in the scalar bank: scalar operation
        return None
    out = []
    if dd['kind'] == '3op':
        for i in range(L):
            d = elem(dd['d'], i, stride, dbl)
            n = elem(dd['n'], i, stride, dbl)
            m = dd['m'] if dd['m'] < bs else elem(dd['m'], i, stride, dbl)
            out.append(encode_dp(w, d, n, m))
    else:                                                # monadic: Fd, Fm
        for i in range(L):
            d = elem(dd['d'], i, stride, dbl)
            m = dd['m'] if dd['m'] < bs else elem(dd['m'], i, stride, dbl)
            out.append(encode_dp(w, d, None, m))
    return out


def analyze(so):
    ins = load(so)
    sites = find_sites(ins)
    regs, warn = regions(sites)
    print("FPSCR writes: %d  (set %d, clear %d)" % (len(sites), sum(1 for s in sites if s[1] == 'set'), sum(1 for s in sites if s[1] == 'clear')))
    print("regions: %d   LEN values: %s   stride values: %s" % (len(regs), sorted(collections.Counter(r[2] for r in regs).items()),
          sorted(collections.Counter(r[3] for r in regs).items())))
    for wn in warn: print("WARNING:", wn)
    idx = {a: i for i, (a, *_ ) in enumerate(ins)}
    tot = 0; unsup = []; kinds = collections.Counter(); branches = []
    for (s, c, L, stride, orr) in regs:
        n = 0
        for (a, w, mn, ops) in ins[idx[s] + 1: idx[c]]:
            e = expand(w, L, stride)
            if e is None:
                dd = dp_decode(w)
                if dd: kinds['scalar-bank/other DP: ' + mn] += 1
                continue
            if isinstance(e, tuple):
                unsup.append((a, mn, ops, e[1]['op'])); continue
            n += 1; kinds[mn] += 1
            if mn in ('bl', 'blx', 'bx'): branches.append((a, mn))
        tot += n
        print("  region %#x..%#x  LEN=%d stride=%d: %d vector instructions" % (s, c, L, stride, n))
    print("vector instructions to expand: %d" % tot)
    print("mnemonics:", dict(kinds.most_common(30)))
    print("unsupported DP forms in regions: %d" % len(unsup))
    for u in unsup[:20]: print("   ", "%#x" % u[0], u[1], u[2], u[3])
    # control flow leaving a region (calls) matters: vector mode would still be on in the callee
    for (s, c, L, stride, orr) in regs:
        for (a, w, mn, ops) in ins[idx[s] + 1: idx[c]]:
            if mn in ('bl', 'blx') or (mn.startswith('b') and mn not in ('bic', 'bics', 'bfc', 'bfi') and re.match(r'[0-9a-f]+ ', ops) and
                                        not (s <= int(ops.split()[0], 16) < c)):
                print("   control flow in/out of region %#x..%#x: %#x %s %s" % (s, c, a, mn, ops))


def b_insn(frm, to, link=False):
    off = (to - (frm + 8)) >> 2
    assert -(1 << 23) <= off < (1 << 23)
    return 0xEA000000 | (0x01000000 if link else 0) | (off & 0xFFFFFF)


def gen(so, out_c, cave_vaddr):
    ins = load(so)
    sites = find_sites(ins)
    regs, warn = regions(sites)
    if warn:
        for wn in warn: print("WARNING:", wn)
        sys.exit("refusing to generate with warnings")
    idx = {a: i for i, (a, *_ ) in enumerate(ins)}
    patches = []        # (addr, word)
    cave = []           # words, at cave_vaddr + 4*i
    for (s, c, L, stride, orr) in regs:
        patches.append((orr, 0xE1A00000))                # mov r0, r0 (nop): do not put the hardware into vector mode
        for (a, w, mn, ops) in ins[idx[s] + 1: idx[c]]:
            e = expand(w, L, stride)
            if e is None: continue
            if isinstance(e, tuple):
                sys.exit("unsupported vector instruction at %#x: %s %s (%s)" % (a, mn, ops, e[1]['op']))
            at = cave_vaddr + 4 * len(cave)
            cave.extend(e)
            cave.append(b_insn(cave_vaddr + 4 * len(cave), a + 4))
            patches.append((a, b_insn(a, at)))
    with open(out_c, "w") as f:
        f.write("/* generated by tools/vfpvec.py: VFP short-vector -> scalar patches for libSpyMouse.so (see that file) */\n#include <stdint.h>\n")
        f.write("const uint32_t vfp_cave_vaddr = %#x;\nconst unsigned vfp_cave_words = %d;\n" % (cave_vaddr, len(cave)))
        f.write("const uint32_t vfp_cave[%d] = {\n" % max(1, len(cave)))
        for i in range(0, len(cave), 8): f.write("    " + ", ".join("%#010x" % x for x in cave[i:i + 8]) + ",\n")
        f.write("};\nconst unsigned vfp_patch_count = %d;\nconst uint32_t vfp_patches[][2] = {\n" % len(patches))
        for a, w in patches: f.write("    { %#x, %#010x },\n" % (a, w))
        f.write("};\n")
    print("%d regions, %d patches, cave %d words (%d bytes) at %#x" % (len(regs), len(patches), len(cave), 4 * len(cave), cave_vaddr))


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "analyze": analyze(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "gen": gen(sys.argv[2], sys.argv[3], int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x2f6000)
    else: print(__doc__)
