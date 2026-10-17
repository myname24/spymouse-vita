/* Minimal ELF32 ARM shared-object loader for libSpyMouse.so.
 *
 * The image is laid out exactly as linked (text and data keep their distance): an RX block for the executable segment and an
 * RW block for the data segment, both at fixed addresses from kubridge. Relocation happens in a work buffer; the RX part is then
 * copied with kuKernelCpuUnrestrictedMemcpy and the caches flushed. Needs ur0:tai/kubridge.skprx on the Vita.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include "kubridge.h"
#include "sm.h"

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050
#endif

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_ARM_EXIDX 0x70000001
#define PF_X 1
#define DT_NULL 0
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } Phdr;
typedef struct { int32_t tag; uint32_t val; } Dyn;
typedef struct { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; } Sym;
typedef struct { uint32_t offset, info; } Rel;

SoImage g_so;

/* gen/vfp_patch.c (tools/vfpvec.py) */
extern const uint32_t vfp_cave_vaddr;
extern const unsigned vfp_cave_words, vfp_patch_count;
extern const uint32_t vfp_cave[];
extern const uint32_t vfp_patches[][2];

static SceUID alloc_at(const char *name, SceKernelMemBlockType type, uint32_t addr, uint32_t size)
{
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof opt);
    opt.size = sizeof opt;
    opt.attr = 0x1;                      /* fixed virtual address */
    opt.field_C = addr;
    return kuKernelAllocMemBlock(name, type, size, &opt);
}

static uint32_t elf_hash(const char *s)
{
    uint32_t h = 0, g;
    while (*s) { h = (h << 4) + (uint8_t)*s++; if ((g = h & 0xf0000000u)) h ^= g >> 24; h &= ~g; }
    return h;
}

void *elf_sym(const char *name)
{
    if (!g_so.hash) return NULL;
    uint32_t nb = g_so.hash[0];
    const Sym *st = g_so.symtab;
    for (uint32_t i = g_so.hash[2 + elf_hash(name) % nb]; i; i = g_so.hash[2 + nb + i])
        if (st[i].shndx && !strcmp(g_so.strtab + st[i].name, name)) return (void *)(uintptr_t)(g_so.base + st[i].value);
    return NULL;
}

static int do_rel(uint8_t *work, const Rel *r, uint32_t n, const Dyn *dyn, const Sym *symtab, const char *strtab,
                  void *(*resolve)(const char *), uint32_t *missing)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t type = r[i].info & 0xff, sym = r[i].info >> 8;
        uint32_t *where = (uint32_t *)(work + r[i].offset);
        uint32_t S = 0;
        if (type == R_ARM_RELATIVE) { *where += SO_BASE; continue; }
        if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) {
            sm_log("elf: unsupported relocation type %u at %#x\n", type, r[i].offset);
            return -1;
        }
        const Sym *s = &symtab[sym];
        if (s->shndx) S = SO_BASE + s->value;
        else {
            const char *nm = strtab + s->name;
            void *p = resolve(nm);
            if (!p) {
                S = 0xDEAD0000u + (*missing)++ * 4;     /* a call lands here and the crash dump names the index */
                sm_log("elf: UNRESOLVED %s -> %#x\n", nm, S);
            } else S = (uint32_t)(uintptr_t)p;
        }
        if (type == R_ARM_ABS32) *where += S; else *where = S;
    }
    (void)dyn;
    return 0;
}

int elf_load(const char *path, void *(*resolve)(const char *name))
{
    FILE *f = fopen(path, "rb");
    if (!f) { sm_log("elf: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc(n);
    if (!img || fread(img, 1, n, f) != (size_t)n) { sm_log("elf: read failed\n"); fclose(f); return -1; }
    fclose(f);
    const Ehdr *eh = (const Ehdr *)img;
    if (memcmp(eh->ident, "\177ELF", 4) || eh->machine != 40) { sm_log("elf: not an ARM ELF\n"); return -1; }
    const Phdr *ph = (const Phdr *)(img + eh->phoff);

    uint32_t span = 0;
    const Phdr *dynph = NULL, *exph = NULL;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD && ph[i].vaddr + ph[i].memsz > span) span = ph[i].vaddr + ph[i].memsz;
        if (ph[i].type == PT_DYNAMIC) dynph = &ph[i];
        if (ph[i].type == PT_ARM_EXIDX) exph = &ph[i];
    }
    span = (span + 0xFFF) & ~0xFFFu;
    uint8_t *work = calloc(1, span);
    if (!work || !dynph) { sm_log("elf: no memory / no PT_DYNAMIC\n"); return -1; }
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD) memcpy(work + ph[i].vaddr, img + ph[i].offset, ph[i].filesz);

    const Dyn *dyn = (const Dyn *)(work + dynph->vaddr);
    uint32_t hash = 0, symtab = 0, strtab = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, init = 0, initsz = 0;
    for (const Dyn *d = dyn; d->tag != DT_NULL; d++) switch (d->tag) {
        case DT_HASH: hash = d->val; break;     case DT_STRTAB: strtab = d->val; break;
        case DT_SYMTAB: symtab = d->val; break; case DT_REL: rel = d->val; break;
        case DT_RELSZ: relsz = d->val; break;   case DT_JMPREL: jmprel = d->val; break;
        case DT_PLTRELSZ: pltrelsz = d->val; break;
        case DT_INIT_ARRAY: init = d->val; break; case DT_INIT_ARRAYSZ: initsz = d->val; break;
    }
    uint32_t missing = 0;
    if (do_rel(work, (const Rel *)(work + rel), relsz / 8, dyn, (const Sym *)(work + symtab), (const char *)(work + strtab), resolve, &missing) ||
        do_rel(work, (const Rel *)(work + jmprel), pltrelsz / 8, dyn, (const Sym *)(work + symtab), (const char *)(work + strtab), resolve, &missing))
        return -1;
    sm_log("elf: relocated, %u unresolved imports\n", missing);

    /* Zero what operator new returns. The game's constructors rely on zeroed memory (the render object at guest 0xc5b64 never sets
     * two of its fields; Android's allocator handed out fresh pages) but its EA pool allocator recycles dirty blocks, and its
     * pointers cannot go through host free() (it keeps its own headers). Each new variant ends in a tail call "b <EA alloc>"
     * (r0 = allocator, r1 = size). That branch is redirected to a cave in the padding after the code:
     *     push {r1,lr}; bl <EA alloc>; pop {r1,lr}; cmp r0,#0; bxeq lr; push {r0,lr}; mov r2,r1; mov r1,#0;
     *     ldr r12,[pc,#4]; blx r12; pop {r0,pc}; .word memset */
    {
        const uint32_t *hh = (const uint32_t *)(work + hash);
        const Sym *st = (const Sym *)(work + symtab);
        const char *str = (const char *)(work + strtab);
        static const char *const names[] = { "_Znwj", "_Znaj", "_ZnwjRKSt9nothrow_t", "_ZnajRKSt9nothrow_t", NULL };
        uint32_t cave = 0x2ae158u;                          /* end of the text segment's file data; the page is zero padding */
        for (int n = 0; names[n]; n++) {
            uint32_t nb = hh[0], f = 0;
            for (uint32_t i = hh[2 + elf_hash(names[n]) % nb]; i; i = hh[2 + nb + i])
                if (st[i].shndx && !strcmp(str + st[i].name, names[n])) { f = st[i].value & ~1u; break; }
            if (!f) { sm_log("elf: %s not found\n", names[n]); continue; }
            int done = 0;
            for (uint32_t a = f; a < f + 0x48 && !done; a += 4) {
                uint32_t insn = *(uint32_t *)(work + a);
                if ((insn >> 24) != 0xEA) continue;         /* unconditional b */
                uint32_t target = a + 8 + ((uint32_t)((int32_t)(insn << 8) >> 8) << 2);
                if (target >= f && target < f + 0x48) continue;     /* a branch inside the function, not the tail call */
                cave = (cave + 7) & ~7u;
                uint32_t *c = (uint32_t *)(work + cave);
                c[0] = 0xe92d4002u;                                   /* push {r1, lr} */
                c[1] = 0xEB000000u | (((target - (cave + 4 + 8)) >> 2) & 0xFFFFFFu);   /* bl target */
                c[2] = 0xe8bd4002u;                                   /* pop {r1, lr} */
                c[3] = 0xe3500000u;                                   /* cmp r0, #0 */
                c[4] = 0x012fff1eu;                                   /* bxeq lr */
                c[5] = 0xe92d4001u;                                   /* push {r0, lr} */
                c[6] = 0xe1a02001u;                                   /* mov r2, r1 */
                c[7] = 0xe3a01000u;                                   /* mov r1, #0 */
                c[8] = 0xe59fc004u;                                   /* ldr r12, [pc, #4] */
                c[9] = 0xe12fff3cu;                                   /* blx r12 */
                c[10] = 0xe8bd8001u;                                  /* pop {r0, pc} */
                c[11] = (uint32_t)(uintptr_t)memset;
                *(uint32_t *)(work + a) = 0xEA000000u | (((cave - (a + 8)) >> 2) & 0xFFFFFFu);   /* b cave */
                sm_log("elf: %s zero-fills (tail call at %#x -> %#x via cave %#x)\n", names[n], a, target, cave);
                cave += 48;
                done = 1;
            }
            if (!done) sm_log("elf: %s: no tail call found\n", names[n]);
        }
    }

    /* VFP short-vector -> scalar: tools/vfpvec.py generated these (the EA audio mixer uses FPSCR.LEN vector mode, which the Vita's
     * CPU does not have). Each vector instruction in the text becomes a branch into the cave, which holds its scalar expansion. */
    for (unsigned i = 0; i < vfp_patch_count; i++) *(uint32_t *)(work + vfp_patches[i][0]) = vfp_patches[i][1];
    sm_log("elf: %u VFP vector patches, cave %u words at %#x\n", vfp_patch_count, vfp_cave_words, vfp_cave_vaddr);

    /* map one block per PT_LOAD at its linked address */
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        uint32_t lo = ph[i].vaddr & ~0xFFFu, hi = (ph[i].vaddr + ph[i].memsz + 0xFFF) & ~0xFFFu;
        int x = ph[i].flags & PF_X;
        SceUID id = alloc_at(x ? "so_text" : "so_data", x ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RX : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                             SO_BASE + lo, hi - lo);
        if (id < 0) { sm_log("elf: block at %#x (%#x bytes) failed: %#x (kubridge.skprx installed?)\n", SO_BASE + lo, hi - lo, id); return -1; }
        void *base = NULL;
        sceKernelGetMemBlockBase(id, &base);
        if ((uint32_t)(uintptr_t)base != SO_BASE + lo) { sm_log("elf: block placed at %p, wanted %#x\n", base, SO_BASE + lo); return -1; }
        if (x) { kuKernelCpuUnrestrictedMemcpy(base, work + lo, hi - lo); kuKernelFlushCaches(base, hi - lo); }
        else memcpy(base, work + lo, hi - lo);
        sm_log("elf: %s %#x..%#x\n", x ? "text" : "data", SO_BASE + lo, SO_BASE + hi);
    }
    if (vfp_cave_words) {                                /* the cave: an RX block right after the data segment */
        uint32_t csz = (vfp_cave_words * 4 + 0xFFF) & ~0xFFFu;
        SceUID cid = alloc_at("so_vfp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, SO_BASE + vfp_cave_vaddr, csz);
        void *cb = NULL;
        if (cid < 0 || sceKernelGetMemBlockBase(cid, &cb) < 0 || (uint32_t)(uintptr_t)cb != SO_BASE + vfp_cave_vaddr) {
            sm_log("elf: VFP cave block at %#x failed: %#x\n", SO_BASE + vfp_cave_vaddr, cid);
            return -1;
        }
        kuKernelCpuUnrestrictedMemcpy(cb, vfp_cave, vfp_cave_words * 4);
        kuKernelFlushCaches(cb, csz);
    }
    g_so.base = SO_BASE; g_so.span = span;
    g_so.hash = (const uint32_t *)(uintptr_t)(SO_BASE + hash);
    g_so.symtab = (const void *)(uintptr_t)(SO_BASE + symtab);
    g_so.strtab = (const char *)(uintptr_t)(SO_BASE + strtab);
    if (exph) { g_so.exidx = SO_BASE + exph->vaddr; g_so.exidx_size = exph->memsz; }
    g_so.init_array = (uint32_t *)(uintptr_t)(SO_BASE + init); g_so.init_count = initsz / 4;
    free(work); free(img);
    return 0;
}

void elf_run_init(void)
{
    for (uint32_t i = 0; i < g_so.init_count; i++) {
        void (*fn)(void) = (void (*)(void))(uintptr_t)g_so.init_array[i];
        if (i < 3 || i % 50 == 0) sm_log("init %u/%u: %p\n", i, g_so.init_count, fn);
        fn();
    }
    sm_log("init array done\n");
}
