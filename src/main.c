/* Spy Mouse HD for PS Vita: loads the Android armeabi libSpyMouse.so and runs it against Vita shims.
 * Boot sequence so far: log -> clocks -> ELF load/relocate -> init array -> vitaGL -> JNI_OnLoad. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <malloc.h>
#include <dirent.h>
#include <psp2/touch.h>
#include <sys/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <vitaGL.h>
#include "sm.h"
#include "jni_priv.h"

extern volatile unsigned gl_draws, gl_clears;       /* glwrap.c */
extern volatile int gl_log;                         /* glwrap.c: budget of GL calls to trace */
extern GLuint gl_last_tex;                          /* glwrap.c: the game's most recently uploaded texture */
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;     /* the game's frame function runs on the main thread */
int _newlib_heap_size_user = 208 * 1024 * 1024;    /* the rest of the 365 MB (ATTRIBUTE2=12) goes to vitaGL and the stacks */

static FILE *logf_;
int sm_verbose;                                     /* ux0:data/spymouse/verbose.txt exists: log every file open, GL call, Java call ... */
/* Quiet by default. Every flushed log line is a memory-card write; the verbose categories were thousands of lines during loading
 * (Vita test 17: "slow loading everything"). Lines get a timestamp (seconds since start). */
void sm_log(const char *fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    if (!sm_verbose) {
        static const char *const noisy[] = { "file:", "tr:", "gl:", "java:", "JNI:", "fread returned", "touch:", "pthread_create",
                                             "elf: hooked", "init ", "overlay:", NULL };
        for (int i = 0; noisy[i]; i++) if (!strncmp(buf, noisy[i], strlen(noisy[i]))) return;
    }
    static uint64_t t0; static int nl;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!t0) t0 = now;
    char line[560];
    snprintf(line, sizeof line, "[%3u.%03u] %s", (unsigned)((now - t0) / 1000000), (unsigned)((now - t0) / 1000 % 1000), buf);
    sceClibPrintf("%s", line);
    if (logf_) {
        fputs(line, logf_);
        if (sm_verbose || (++nl & 15) == 0 || !strncmp(buf, "UNIMPL", 6) || !strncmp(buf, "MALLOC", 6) || !strncmp(buf, "frame", 5) || !strncmp(buf, "start:", 6)) fflush(logf_);
    }
}
void sm_unimpl(const char *name)
{
    static char seen[256][48]; static int n;
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], name)) return;
    if (n < 256) snprintf(seen[n++], 48, "%s", name);
    sm_log("UNIMPLEMENTED import called: %s\n", name);
}

/* Debug overlay: after the game's frame, draw a red and a blinking green square with plain vitaGL calls. If they appear while the
 * game's own draws stay black, the problem is the game's GL state / textures; if nothing appears, presentation is broken. */
static int overlay_on;                              /* ux0:data/spymouse/overlay.txt exists */
static void debug_overlay(int frame)
{
    if (!overlay_on) return;
    static const GLfloat red[8]   = { 10, 10, 110, 10, 10, 110, 110, 110 };
    static const GLfloat green[8] = { 130, 10, 230, 10, 130, 110, 230, 110 };
    glViewport(0, 0, 960, 544);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrthof(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_VERTEX_ARRAY);
    glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
    glVertexPointer(2, GL_FLOAT, 0, red);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if ((frame / 30) & 1) {
        glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
        glVertexPointer(2, GL_FLOAT, 0, green);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    /* textured tests: A = the game's own last uploaded texture, untouched (default parameters as the game left them);
     * B = my own 4x4 test texture with explicit LINEAR/CLAMP parameters */
    static GLuint test_tex;
    if (!test_tex) {
        static const uint32_t px[16] = { 0xff0080ff, 0xffff8000, 0xff0080ff, 0xffff8000, 0xffff8000, 0xff0080ff, 0xffff8000, 0xff0080ff,
                                         0xff0080ff, 0xffff8000, 0xff0080ff, 0xffff8000, 0xffff8000, 0xff0080ff, 0xffff8000, 0xff0080ff };
        glGenTextures(1, &test_tex);
        glBindTexture(GL_TEXTURE_2D, test_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        sm_log("overlay: test texture id %u\n", test_tex);
    }
    static const GLfloat tc[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    static const GLfloat quadA[8] = { 250, 10, 350, 10, 250, 110, 350, 110 };
    static const GLfloat quadB[8] = { 370, 10, 470, 10, 370, 110, 470, 110 };
    glEnable(GL_TEXTURE_2D); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    if (gl_last_tex) {
        static const GLfloat tcA[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };       /* the game stores rows bottom-first for its y-up projection */
        glTexCoordPointer(2, GL_FLOAT, 0, tcA);
        glBindTexture(GL_TEXTURE_2D, gl_last_tex);
        glVertexPointer(2, GL_FLOAT, 0, quadA);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    glBindTexture(GL_TEXTURE_2D, test_tex);
    glVertexPointer(2, GL_FLOAT, 0, quadB);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

/* data check: list zero-byte files under files/ (a copy that lost file contents makes the game read empty layouts and crash) */
static void scan_dir(const char *dir, int *files, int *zero)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[512]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(p, files, zero);
        else { (*files)++; if (st.st_size == 0) { (*zero)++; if (*zero <= 80) sm_log("data check: ZERO-BYTE file %s\n", p); } }
    }
    closedir(d);
}

static void *game_thread(void *p)
{
    struct { void *fn, *env; } *a = p;
    sm_log("game thread: runEntryPoint\n");
    int (*run)(void *, void *) = a->fn;
    int r = run(a->env, jni_class("com/ea/SpyMouse/SpyMouse"));
    sm_log("game thread: runEntryPoint returned %d\n", r);
    return NULL;
}

void sm_imports_init(void);
typedef int (*jni_onload_t)(void *vm, void *reserved);

int main(void)
{
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(DATA_DIR "/files", 0777);
    logf_ = fopen(DATA_DIR "/log.txt", "w");
    { FILE *v = fopen(DATA_DIR "/verbose.txt", "r"); if (v) { fclose(v); sm_verbose = 1; } }
    sm_log("Spy Mouse HD / PS Vita loader\n");
    /* opt-in: stat-ing all 5000+ files on the memory card takes about a minute; create ux0:data/spymouse/check.txt to run it */
    { FILE *c = fopen(DATA_DIR "/check.txt", "r");
      if (c) { fclose(c); int nf = 0, nz = 0; scan_dir(DATA_DIR "/files", &nf, &nz); sm_log("data check: %d files under files/, %d zero-byte\n", nf, nz); } }
    overlay_on = ({ FILE *o = fopen(DATA_DIR "/overlay.txt", "r"); int on = o != NULL; if (o) fclose(o); on; });     /* test squares: opt-in */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sm_imports_init();
    {   /* the loader patches fixed addresses of one exact build: refuse anything else instead of crashing somewhere inside it */
        struct stat st;
        if (stat(SO_PATH, &st)) { sm_log("MISSING " SO_PATH " - copy libSpyMouse.so from the Android APK (lib/armeabi/); see README\n"); sceKernelDelayThread(3000000); return 1; }
        static const char *const need[] = { DATA_DIR "/assets/GameApplication.ini", DATA_DIR "/files/res_960x640", DATA_DIR "/files/res" };
        for (int i = 0; i < 3; i++) {
            struct stat s2;
            if (stat(need[i], &s2)) { sm_log("MISSING %s - the game data is incomplete; copy assets/ from the APK and files/ (res, res_960x640) from Android/data/com.ea.spymouse_row (see README)\n", need[i]); sceKernelDelayThread(3000000); return 1; }
        }
        if (st.st_size != SO_SIZE) { sm_log("WRONG libSpyMouse.so: %ld bytes, expected %d (Spy Mouse HD 1.x armeabi from the EA APK)\n", (long)st.st_size, SO_SIZE); sceKernelDelayThread(3000000); return 1; }
    }
    if (elf_load(SO_PATH, sm_resolve)) { sm_log("load failed\n"); sceKernelDelayThread(3000000); return 1; }
    elf_run_init();

    sm_log("vglInitExtended -> %d\n", (int)vglInitExtended(0, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_NONE));

    jni_onload_t onload = (jni_onload_t)elf_sym("JNI_OnLoad");
    if (onload) { int v = onload(jni_vm(), NULL); sm_log("JNI_OnLoad -> %#x\n", v); }
    else sm_log("no JNI_OnLoad\n");

    /* The Java start order is a best reading of the EA "Blast" Android framework (signatures from classes.dex, see
     * docs/dex_dump.txt); the real order is confirmed on the first run from the log. */
    void *env = jni_env();
    JObj *act = jni_object("com/ea/blast/MainActivity");
    JObj *assets = jni_object("android/content/res/AssetManager");
    typedef void (*fn_v)(void *, void *);
    typedef void (*fn_eio)(void *, void *, void *, void *, void *);
    typedef void (*fn_ii)(void *, void *, int, int);
    typedef void (*fn_ptr)(void *, void *, int, int, int, float, float) AAPCS;
    #define NATIVE(var, name) void *var = elf_sym(name); if (!var) sm_log("missing export %s\n", name)
    NATIVE(n_thread, "Java_com_ea_EAThread_EAThread_Init");
    NATIVE(n_eaio, "Java_com_ea_EAIO_EAIO_StartupNativeImpl");
    NATIVE(n_stor, "Java_com_ea_EAMIO_StorageDirectory_StartupNativeImpl");
    NATIVE(n_rwfs, "Java_com_ea_rwfilesystem_rwfilesystem_StartupNativeImpl");
    NATIVE(n_create, "Java_com_ea_blast_MainActivity_NativeOnCreate");
    NATIVE(n_surf, "Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated");
    NATIVE(n_chg, "Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceChanged");
    NATIVE(n_draw, "Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame");
    void *cls = jni_class("com/ea/blast/MainActivity");
    if (n_thread) { sm_log("start: EAThread.Init\n"); ((fn_v)n_thread)(env, jni_class("com/ea/EAThread/EAThread")); }
    if (n_eaio) { sm_log("start: EAIO.StartupNativeImpl\n"); ((fn_eio)n_eaio)(env, jni_class("com/ea/EAIO/EAIO"), assets, jni_string(DATA_DIR "/files"), jni_string(DATA_DIR "/files")); }
    if (n_rwfs) { sm_log("start: rwfilesystem.StartupNativeImpl\n"); ((fn_eio)n_rwfs)(env, jni_class("com/ea/rwfilesystem/rwfilesystem"), assets, jni_string(DATA_DIR "/files"), jni_string(DATA_DIR "/files")); }
    if (n_stor) { sm_log("start: StorageDirectory.StartupNativeImpl\n"); ((fn_v)n_stor)(env, jni_class("com/ea/EAMIO/StorageDirectory")); }
    /* the other framework modules the Android Activity starts; their natives keep a JavaVM* that is NULL until these run
     * (Vita test 10: a null-VM crash when the splash video code asked for a JNIEnv) */
    NATIVE(n_vid, "Java_com_ea_VideoPlayer_PlayerAndroid_StartupNativeImpl");
    NATIVE(n_amix, "Java_com_ea_EAMAudio_EAMAudioCoreWrapper_NativeStartup");
    if (n_vid) { sm_log("start: PlayerAndroid.StartupNativeImpl\n"); ((fn_v)n_vid)(env, jni_class("com/ea/VideoPlayer/PlayerAndroid")); }
    if (n_amix) { sm_log("start: EAMAudioCoreWrapper.NativeStartup\n"); ((fn_v)n_amix)(env, jni_class("com/ea/EAMAudio/EAMAudioCoreWrapper")); }
    /* Audio: AndroidEAAudioCore.Init(AudioTrack, a, b, c) allocates a short[b * 512] mix buffer (guest 0x20d054), so b is most likely
     * the channel count (512 frames per block); a is taken to be the sample rate. The guest then calls AudioTrack.write via JNI;
     * java.c turns that into audio_write(). */
    /* The EA mixer is hand-written ARM VFP *short-vector* code (vmsr fpscr with LEN > 0, e.g. guest 0x20f0c8 vmul.f32 s8, s8, s0). The
     * Vita's CPU raises an undefined instruction exception for it (Vita test 18), so tools/vfpvec.py rewrites the 70 vector
     * instructions into scalar sequences (applied in elfload.c). ux0:data/spymouse/noaudio.txt switches the audio core off. */
    NATIVE(n_ainit, "Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init");
    int audio_enabled = 1;
    { FILE *af = fopen(DATA_DIR "/noaudio.txt", "r"); if (af) { fclose(af); audio_enabled = 0; } }
    if (n_ainit && audio_enabled) {
        typedef void (*fn_ainit)(void *, void *, void *, int, int, int);
        /* guesses: Init(track, 44100, 2, 4096), played back as 44100 Hz stereo. ux0:data/spymouse/audio.cfg ("a b c rate channels",
         * five integers) overrides them without a rebuild. */
        int ia = 44100, ib = 2, ic = 44100, rate = 44100, ch = 2;
        { FILE *cf = fopen(DATA_DIR "/audio.cfg", "r");
          if (cf) { int v[5]; if (fscanf(cf, "%d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) { ia = v[0]; ib = v[1]; ic = v[2]; rate = v[3]; ch = v[4]; } fclose(cf); } }
        audio_init(rate, ch);
        sm_log("start: AndroidEAAudioCore.Init(track, %d, %d, %d), playback %d Hz %d ch\n", ia, ib, ic, rate, ch);
        ((fn_ainit)n_ainit)(env, jni_class("com/ea/EAAudioCore/AndroidEAAudioCore"), jni_object("android/media/AudioTrack"), ia, ib, ic);
        { extern void audio_dump_state(const char *); audio_dump_state("after Init"); }
    }
    if (n_create) { sm_log("start: MainActivity.NativeOnCreate\n"); ((fn_v)n_create)(env, act); }
    (void)cls;
    /* Android lifecycle: onCreate, onResume, surface created/changed, onWindowFocusChanged(true). NativeOnResume and the focus
     * callback post the 0x80006 / 0x20006 events that activate the app (guest 0x1ba830, 0x1ba8cc); without them the game idles. */
    NATIVE(n_resume, "Java_com_ea_blast_MainActivity_NativeOnResume");
    NATIVE(n_focus, "Java_com_ea_blast_MainActivity_NativeOnWindowFocusChanged");
    if (n_resume) { sm_log("start: MainActivity.NativeOnResume\n"); ((fn_v)n_resume)(env, act); }
    JObj *rend = jni_object("com/ea/blast/AndroidRenderer");
    if (n_surf) { sm_log("start: NativeOnSurfaceCreated\n"); ((fn_v)n_surf)(env, rend); }
    if (n_chg) { sm_log("start: NativeOnSurfaceChanged 960x544\n"); ((fn_ii)n_chg)(env, rend, 960, 544); }

    if (n_focus) { sm_log("start: MainActivity.NativeOnWindowFocusChanged(true)\n"); ((void (*)(void *, void *, int))n_focus)(env, act, 1); }

    /* On Android a Java thread runs SpyMouse.runEntryPoint(), which is the game's main loop and never returns; the GL thread only
     * calls NativeOnDrawFrame. Same split here. */
    NATIVE(n_entry, "Java_com_ea_SpyMouse_SpyMouse_runEntryPoint");
    if (n_entry) {
        pthread_t th; pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 2 * 1024 * 1024);
        struct { void *fn, *env; } *arg = malloc(sizeof *arg);
        arg->fn = n_entry; arg->env = env;
        if (pthread_create(&th, &at, game_thread, arg)) sm_log("could not start the game thread\n");
        else sm_log("start: game thread (runEntryPoint)\n");
    }

    /* Touch: the game takes touches through TouchSurfaceAndroid.NativeOnPointerEvent(eventType, moduleId, pointerId, x, y)
     * (guest 0x1bbf70: the first int picks the event, the second is stored in the event at +16, the third at +40). Event ids come from
     * the NativeGetIdRawPointer* getters (0x6000d down, 0x4000d move, 0x8000d up), the module id from ModuleCatalog (touch screen
     * 1000). The front panel is 1920x1088; the game's surface is 960x544. */
    NATIVE(n_ptr, "Java_com_ea_blast_TouchSurfaceAndroid_NativeOnPointerEvent");
    JObj *tsurf = jni_object("com/ea/blast/TouchSurfaceAndroid");
    int ev_down = 0x6000d, ev_move = 0x4000d, ev_up = 0x8000d, mod_touch = 1000;
    {
        int (*g)(void *, void *);
        void *tc = jni_class("com/ea/blast/TouchSurfaceAndroid");
        if ((g = elf_sym("Java_com_ea_blast_TouchSurfaceAndroid_NativeGetIdRawPointerDown"))) ev_down = g(env, tc);
        if ((g = elf_sym("Java_com_ea_blast_TouchSurfaceAndroid_NativeGetIdRawPointerMove"))) ev_move = g(env, tc);
        if ((g = elf_sym("Java_com_ea_blast_TouchSurfaceAndroid_NativeGetIdRawPointerUp"))) ev_up = g(env, tc);
        if ((g = elf_sym("Java_com_ea_blast_ModuleCatalog_NativeGetModuleTypeIdTouchScreen"))) mod_touch = g(env, jni_class("com/ea/blast/ModuleCatalog"));
        sm_log("touch: events down=%#x move=%#x up=%#x module=%d\n", ev_down, ev_move, ev_up, mod_touch);
    }
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    struct { int used, id; float x, y; } tp[6];
    memset(tp, 0, sizeof tp);

    SceCtrlData pad;
    for (int frame = 0;; frame++) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) break;     /* START+SELECT quits */
        if (n_ptr) {
            SceTouchData td;
            int seen[6] = { 0 };
            sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
            for (int i = 0; i < td.reportNum && i < 6; i++) {
                int id = td.report[i].id, s = -1;
                float x = td.report[i].x * 0.5f, y = td.report[i].y * 0.5f;
                for (int k = 0; k < 6; k++) if (tp[k].used && tp[k].id == id) s = k;
                if (s < 0) {
                    for (int k = 0; k < 5 && s < 0; k++) if (!tp[k].used) s = k;       /* slot 5 belongs to the button pointer */
                    if (s < 0) continue;
                    tp[s].used = 1; tp[s].id = id; tp[s].x = x; tp[s].y = y; seen[s] = 1;
                    sm_log("touch: down slot %d at %.0f,%.0f\n", s, x, y);
                    ((fn_ptr)n_ptr)(env, tsurf, ev_down, mod_touch, s, x, y);
                } else {
                    seen[s] = 1;
                    if (x != tp[s].x || y != tp[s].y) { tp[s].x = x; tp[s].y = y; ((fn_ptr)n_ptr)(env, tsurf, ev_move, mod_touch, s, x, y); }
                }
            }
            for (int k = 0; k < 6; k++)
                if (tp[k].used && !seen[k]) {
                    sm_log("touch: up slot %d at %.0f,%.0f\n", k, tp[k].x, tp[k].y);
                    ((fn_ptr)n_ptr)(env, tsurf, ev_up, mod_touch, k, tp[k].x, tp[k].y);
                    tp[k].used = 0;
                }
        }
        if (frame == 10 || frame == 700) { gl_log = 300; sm_log("--- GL trace of frame %d ---\n", frame); }
        if (n_draw) ((fn_v)n_draw)(env, rend);
        if (g_video_done) {                          /* a video "finished": tell the game, as MediaPlayer's onCompletion would */
            g_video_done = 0;
            void *oc = elf_sym("Java_com_ea_VideoPlayer_PlayerAndroid_OnCompletionNativeImpl");
            if (oc && g_video_obj) { sm_log("video: play() -> OnCompletion\n"); ((fn_v)oc)(env, g_video_obj); }
        }
        debug_overlay(frame);
        vglSwapBuffers(GL_FALSE);
        if (frame < 5 || frame % 300 == 0)
        {
            struct mallinfo mi = mallinfo();
            unsigned io[5], tx[3];
            sm_io_stats(io); sm_tex_stats(tx);
            sm_log("frame %d  [gl] draws=%u clears=%u glGetError=%#x  [mem] heap in use %d KB\n", frame, gl_draws, gl_clears,
                   glGetError(), mi.uordblks / 1024);
            sm_log("   since last: fopen %u (%u ms), read %u KB (%u ms), %u cached misses; %u texture uploads (%u ms, %u KB)\n",
                   io[0], io[1], io[2], io[3], io[4], tx[0], tx[1], tx[2]);
            unsigned au[10];
            audio_stats(au);
            sm_log("   [audio] %u writes, %u frames in, %u underrun frames, peak %u, %u blocked polls, ring fill %u frames\n",
                   au[0], au[1], au[2], au[3], au[4], au[5]);
            sm_log("   [audio] output call %u..%u us, %u late grains (>26 ms apart), max gap %u us\n", au[6], au[7], au[8], au[9]);
        }
    }
    (void)sizeof(fn_ptr);
    sm_log("done\n");
    sceKernelExitProcess(0);
    return 0;
}
