/* Spy Mouse HD (Android armeabi) on PS Vita: shared declarations. */
#ifndef SM_H
#define SM_H
#include <stdint.h>
#include <stddef.h>

#define DATA_DIR      "ux0:data/spymouse"          /* libSpyMouse.so, assets/, files/ (game data), log.txt */
#define SO_PATH       DATA_DIR "/libSpyMouse.so"
#define SO_SIZE       2902272                       /* the only supported libSpyMouse.so (sha256 cb066bb7...a121f) */
#define SO_BASE       0x98000000u                  /* fixed load address (pc-relative code keeps text/data distance) */

/* The guest is armeabi (ARMv5TE, softfp): float/double arguments travel in core registers. Host functions that take or
 * return floating point and are called by guest code must use the soft-float calling convention. */
#define AAPCS __attribute__((pcs("aapcs")))

void sm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int sm_verbose;                              /* ux0:data/spymouse/verbose.txt exists */
void sm_io_stats(unsigned *v5);                     /* imports.c: opens, open ms, read KB, read ms, cached misses (resets) */
void sm_tex_stats(unsigned *v3);                    /* glwrap.c: texture uploads, upload ms, KB (resets) */
void sm_unimpl(const char *name);                   /* logs each unimplemented import once */

typedef struct { const char *name; void *addr; } ImpSym;
extern const ImpSym imp_syms[];                     /* imports.c: libc, math, pthread, bionic data */
extern const ImpSym gen_syms[];                     /* gen/gen_syms.c: GL entry points + logging stubs (tools/gen.py) */
void *sm_resolve(const char *name);                 /* NULL if unknown */

/* elfload.c */
typedef struct {
    uint32_t base, span;                            /* host address of the image start, bytes mapped */
    uint32_t exidx, exidx_size;                     /* PT_ARM_EXIDX (host address) for __gnu_Unwind_Find_exidx */
    uint32_t *init_array; uint32_t init_count;
    const uint32_t *hash; const void *symtab; const char *strtab;
} SoImage;
extern SoImage g_so;
int   elf_load(const char *path, void *(*resolve)(const char *name));
void *elf_sym(const char *name);                    /* exported symbol of the loaded image, NULL if absent */
void  elf_run_init(void);

/* audio.c */
void     audio_init(int rate, int channels);
int      audio_write(const int16_t *pcm, int nshorts);   /* blocks while the ring buffer is full */
void     audio_set_paused(int paused);
void     audio_stats(unsigned *v10);                      /* writes, frames in, underruns, peak, blocked polls, ring fill (resets) */
unsigned audio_frames_played(void);

/* jni.c */
void *jni_vm(void);                                 /* JavaVM* for JNI_OnLoad */
void *jni_env(void);                                /* JNIEnv* for the Java_* entry points */

/* glwrap.c: GL setup helpers */
#endif
