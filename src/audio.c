/* Audio output: the game's EA audio core mixes into 16-bit PCM and hands it to a Java AudioTrack (write([SII)I). java.c emulates the
 * AudioTrack and calls audio_write() here. PCM goes into a ring buffer; a thread on the Vita's 48 kHz main port resamples it
 * (linear) and outputs 1024-frame grains, which also paces the writer (audio_write blocks while the ring is full, like a real
 * AudioTrack in MODE_STREAM). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <pthread.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include "sm.h"

#define RING_FRAMES (1 << 15)
#define GRAIN 1024
static int16_t ring[RING_FRAMES * 2];                /* always stereo */
static volatile uint32_t wpos, rpos;                 /* frame counters, single producer / single consumer */
static volatile uint32_t played;
static int src_rate = 44100, src_ch = 2, port = -1, started;
static volatile int paused;
static volatile uint32_t st_writes, st_frames_in, st_underruns, st_peak, st_blocked;
static volatile uint32_t st_omin = 0xffffffffu, st_omax, st_late, st_gmax;     /* sceAudioOutOutput duration / spacing of grains */

/* [audio] stats for the 300-frame log line; resets the counters */
void audio_stats(unsigned *v)
{
    v[0] = st_writes; v[1] = st_frames_in; v[2] = st_underruns; v[3] = st_peak; v[4] = st_blocked; v[5] = wpos - rpos;
    v[6] = st_omin; v[7] = st_omax; v[8] = st_late; v[9] = st_gmax;
    st_writes = st_frames_in = st_underruns = st_peak = st_blocked = 0; st_omin = 0xffffffffu; st_omax = st_late = st_gmax = 0;
}

static void *audio_thread(void *arg)
{
    static int16_t outbuf[4][GRAIN * 2] __attribute__((aligned(64)));   /* rotating: a single reused buffer crackled on the device (test tone, Vita test 21) */
    unsigned bi = 0;
    /* the grain must be delivered on time or the hardware replays stale samples (crackle): run above the game threads (default 160) */
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);
    uint32_t pos = 0, pacc = 0;                             /* 16.16 fractional position between ring frames rpos and rpos+1 */
    const uint32_t step = (uint32_t)(((uint64_t)src_rate << 16) / 48000);
    for (;;) {
        int16_t *out = outbuf[bi++ & 3];
        for (int i = 0; i < GRAIN; i++) {
            uint32_t avail = wpos - rpos;
            /* the playback head runs in real time even while starved (a real AudioTrack's does); the mixer paces itself on it */
            if (!paused) { pacc += step; while (pacc >= 0x10000) { pacc -= 0x10000; played++; } }
            if (!paused && avail >= 2) {
                const int16_t *a = &ring[(rpos % RING_FRAMES) * 2], *b = &ring[((rpos + 1) % RING_FRAMES) * 2];
                int f = (int)(pos & 0xffff);
                out[i * 2]     = (int16_t)(a[0] + (((int)b[0] - a[0]) * f >> 16));
                out[i * 2 + 1] = (int16_t)(a[1] + (((int)b[1] - a[1]) * f >> 16));
                pos += step;
                while (pos >= 0x10000) { pos -= 0x10000; rpos++; }
            } else { out[i * 2] = out[i * 2 + 1] = 0; if (!paused && started) st_underruns++; }
        }
        /* test_tone.txt: a clean 440 Hz sine for the first ~4 s replaces the game audio, to tell the Vita output path from the content */
        static uint64_t t_prev;
        uint64_t t0 = sceKernelGetProcessTimeWide();
        sceAudioOutOutput(port, out);
        { uint64_t t1 = sceKernelGetProcessTimeWide(); uint32_t us = (uint32_t)(t1 - t0), gap = t_prev ? (uint32_t)(t1 - t_prev) : 21333; t_prev = t1;
          if (us < st_omin) st_omin = us; if (us > st_omax) st_omax = us; if (gap > 26000) st_late++; if (gap > st_gmax) st_gmax = gap; }
        /* diagnostics: with audio_dump.txt, the first ~12 s of what is really sent to the Vita (48 kHz stereo) go to audio_out.raw */
        static int16_t *cap; static uint32_t capn; static int cap_state;
        if (cap_state == 0) { FILE *t = fopen(DATA_DIR "/audio_dump.txt", "r"); if (t) { fclose(t); cap = malloc(2400000 * 2); } cap_state = cap ? 1 : 2; }
        if (cap_state == 1 && wpos > 40000) {
            memcpy(cap + capn, out, GRAIN * 4); capn += GRAIN * 2;
            if (capn + GRAIN * 2 > 2400000) { FILE *f = fopen(DATA_DIR "/audio_out.raw", "wb"); if (f) { fwrite(cap, 2, capn, f); fclose(f); } sm_log("audio: audio_out.raw written\n"); free(cap); cap_state = 2; }
        }
    }
    return NULL;
}

/* test_tone.txt: before the game starts, four 2 s sine beeps through different Vita output set-ups (pitch tells which):
 *   1  440 Hz  MAIN port, 1024-frame grain, one buffer          (what the game path uses)
 *   2  660 Hz  MAIN port, 1024-frame grain, four rotating 64-byte aligned buffers
 *   3  880 Hz  MAIN port, 256-frame grain, four rotating buffers
 *   4 1100 Hz  BGM port, 1024-frame grain, four rotating buffers */
static void tone_variant(int type, int grain, int nbuf, float hz, int n)
{
    int p = sceAudioOutOpenPort(type, grain, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    sm_log("audio: tone variant %d: port %d (type %d grain %d buffers %d %d Hz)\n", n, p, type, grain, nbuf, (int)hz);
    if (p < 0) return;
    int16_t *b = memalign(64, (size_t)grain * 4 * nbuf);
    uint32_t t = 0;
    for (int g = 0; g < 48000 * 2 / grain; g++) {
        int16_t *o = b + (size_t)(g % nbuf) * grain * 2;
        for (int i = 0; i < grain; i++) { int16_t v = (int16_t)(8000 * __builtin_sinf(6.2831853f * hz * (float)(t++) / 48000.0f)); o[i * 2] = o[i * 2 + 1] = v; }
        sceAudioOutOutput(p, o);
    }
    memset(b, 0, (size_t)grain * 4 * nbuf);
    for (int g = 0; g < 48000 / grain; g++) sceAudioOutOutput(p, b);
    sceAudioOutReleasePort(p);
    free(b);
}
static void tone_test(void)
{
    FILE *t = fopen(DATA_DIR "/test_tone.txt", "r"); if (!t) return; fclose(t);
    tone_variant(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 1, 440.0f, 1);
    tone_variant(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 4, 660.0f, 2);
    tone_variant(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 256, 4, 880.0f, 3);
    tone_variant(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 4, 1100.0f, 4);
}

void audio_init(int rate, int channels)
{
    if (started) return;
    tone_test();
    src_rate = rate > 0 ? rate : 44100; src_ch = channels > 0 ? channels : 2;
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    sm_log("audio: port %d, source %d Hz, %d channel(s)\n", port, src_rate, src_ch);
    if (port < 0) return;
    started = 1;
    pthread_t th; pthread_attr_t at;
    pthread_attr_init(&at); pthread_attr_setstacksize(&at, 128 * 1024);
    pthread_create(&th, &at, audio_thread, NULL);
}

void audio_set_paused(int p) { paused = p; }
unsigned audio_frames_played(void) { return played; }

/* Diagnostics: the first blocks are described in the log; with ux0:data/spymouse/audio_dump.txt present, the first ~8 s of the
 * raw PCM as the game wrote it (16-bit, interleaved, source rate) go to audio_dump.raw for offline analysis. */
static FILE *dump_f; static int dump_checked; static uint32_t dump_bytes; static int described;
/* the Java-side state the native audio code keeps next to its JNI ids (base 0x2f5650 in libSpyMouse.so) */
void audio_dump_state(const char *why)
{
    const volatile uint32_t *w = (const volatile uint32_t *)(SO_BASE + 0x2f5650);
    sm_log("audio: state (%s): %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", why,
           w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13]);
}
static void audio_diag(const int16_t *pcm, int n)
{
    static int dumped; if (!dumped && n && pcm[0] | pcm[1] | pcm[4]) { dumped = 1; audio_dump_state("first sound"); }
    if (!dump_checked) {
        dump_checked = 1;
        FILE *t = fopen(DATA_DIR "/audio_dump.txt", "r");
        if (t) { fclose(t); dump_f = fopen(DATA_DIR "/audio_dump.raw", "wb"); }
    }
    if (dump_f) {
        if (dump_bytes < 8u * 1024 * 1024) { fwrite(pcm, 2, n, dump_f); dump_bytes += n * 2; }
        else { fclose(dump_f); dump_f = NULL; sm_log("audio: dump written (%u bytes, %d Hz, %d ch)\n", dump_bytes, src_rate, src_ch); }
    }
    if (described < 12) {
        long long sq = 0; int pk = 0, zc = 0, nz = 0;
        for (int i = 0; i < n; i++) {
            int v = pcm[i]; if (v) nz++;
            int a = v < 0 ? -v : v; if (a > pk) pk = a;
            sq += (long long)v * v;
            if (i && ((pcm[i - 1] < 0) != (v < 0))) zc++;
        }
        if (nz) {                                         /* describe only blocks that are not silence */
            described++;
            sm_log("audio: block %d: %d shorts, peak %d, rms %d, zero-crossings %d%%, first: %d %d %d %d %d %d %d %d\n", described, n, pk,
                   (int)__builtin_sqrt((double)sq / n), n > 1 ? zc * 100 / (n - 1) : 0, pcm[0], pcm[1], pcm[2], pcm[3], pcm[4], pcm[5], pcm[6], pcm[7]);
        }
    }
}

/* nshorts samples, interleaved when src_ch > 1; blocks while the ring is full */
int audio_write(const int16_t *pcm, int nshorts)
{
    if (!started) audio_init(44100, 2);
    if (!started) return nshorts;
    audio_diag(pcm, nshorts);
    { static int nw; if (nw < 40 && sceKernelGetProcessTimeLow() > 11000000u) { nw++; sm_log("audio: write %d shorts thread %x\n", nshorts, sceKernelGetThreadId()); } }
    int frames = nshorts / src_ch;
    st_writes++; st_frames_in += frames;
    for (int i = 0; i < frames; i++) {
        while (wpos - rpos >= RING_FRAMES - 2) { st_blocked++; sceKernelDelayThread(2000); }
        int16_t *d = &ring[(wpos % RING_FRAMES) * 2];
        d[0] = pcm[i * src_ch];
        d[1] = src_ch > 1 ? pcm[i * src_ch + 1] : pcm[i];
        int pk = d[0] < 0 ? -d[0] : d[0]; if ((uint32_t)pk > st_peak) st_peak = pk;
        wpos++;
    }
    return nshorts;
}
