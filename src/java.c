/* What the Java half of the Android app did for the game, answered natively.
 * java_dispatch() gets every Java method libSpyMouse.so calls (class, name, signature from jni.c). Known ones are implemented
 * from the dex signatures (docs/dex_dump.txt); anything else is logged once and returns zero/NULL.
 * Assets: AssetManager.open/list read from DATA_DIR/assets (the APK's assets folder, copied by the user). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "sm.h"
#include "jni_priv.h"

static JObj *g_assets, *g_activity;
JObj *g_video_obj;
volatile int g_video_done;

static JV zero(void) { JV r; memset(&r, 0, sizeof r); return r; }
static JV ret_i(int v) { JV r = zero(); r.i = v; return r; }
static JV ret_f(float v) { JV r = zero(); r.f = v; return r; }
static JV ret_l(void *v) { JV r = zero(); r.l = v; return r; }
static JV ret_s(const char *s) { return ret_l(jni_string(s)); }

static int is(const JObj *m, const char *cls, const char *name)
{
    return m->cls && !strcmp(m->cls->name, cls) && !strcmp(m->name, name);
}
static void asset_path(char *out, size_t n, const char *rel)
{
    while (rel[0] == '/') rel++;
    snprintf(out, n, DATA_DIR "/assets/%s", rel);
}

JV java_dispatch(const JObj *m, JObj *thiz, const JV *a)
{
    const char *c = m->cls ? m->cls->name : "?";
    static const char *const quiet[] = { "read", "skip", "close", "available", "openFd", "GetPrimaryExternalStorageDirectory",
                                         "GetPrimaryExternalStorageState", NULL };
    int noisy = 0;
    for (int i = 0; quiet[i]; i++) if (!strcmp(m->name, quiet[i])) noisy = 1;
    if (!noisy) sm_log("java: %s.%s%s\n", c, m->name, m->sig);

    /* --- app object, assets --- */
    if (is(m, "com/ea/blast/MainActivity", "GetInstance")) { if (!g_activity) g_activity = jni_object("com/ea/blast/MainActivity"); return ret_l(g_activity); }
    if (!strcmp(m->name, "getAssets")) { if (!g_assets) g_assets = jni_object("android/content/res/AssetManager"); return ret_l(g_assets); }
    if (!strcmp(c, "android/content/res/AssetManager")) {
        if (!strcmp(m->name, "open")) {
            char p[512];
            asset_path(p, sizeof p, ((JObj *)a[0].l)->name);
            FILE *f = fopen(p, "rb");
            sm_log("java: asset open %s%s\n", p, f ? "" : "  (MISSING)");
            if (!f) { sm_log("java: asset not found: %s\n", p); jni_throw("java/io/FileNotFoundException"); return ret_l(NULL); }
            JObj *in = jni_object("java/io/InputStream");
            in->priv = f;
            return ret_l(in);
        }
        if (!strcmp(m->name, "list")) {
            char p[512]; asset_path(p, sizeof p, ((JObj *)a[0].l)->name);
            DIR *d = opendir(p);
            int n = 0;
            JObj *names[512];
            if (d) { struct dirent *e; while ((e = readdir(d)) && n < 512) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names[n++] = jni_string(e->d_name); closedir(d); }
            JObj *arr = jni_array("Ljava/lang/String;", n, sizeof(void *));
            for (int i = 0; i < n; i++) ((JObj **)arr->data)[i] = names[i];
            return ret_l(arr);
        }
        if (!strcmp(m->name, "openFd")) { jni_throw("java/io/FileNotFoundException"); return ret_l(NULL); }
    }
    if (!strcmp(c, "java/io/InputStream") && thiz && thiz->priv) {
        FILE *f = thiz->priv;
        if (!strcmp(m->name, "read") && !strcmp(m->sig, "([BII)I")) {
            JObj *arr = a[0].l;
            int off = a[1].i, len = a[2].i;
            if (!arr || off < 0 || len < 0 || off + len > arr->len) return ret_i(-1);
            size_t n = fread((uint8_t *)arr->data + off, 1, len, f);
            return ret_i(n ? (int)n : (len ? -1 : 0));
        }
        if (!strcmp(m->name, "read") && !strcmp(m->sig, "()I")) { int ch = fgetc(f); return ret_i(ch); }
        if (!strcmp(m->name, "available")) { long p = ftell(f); fseek(f, 0, SEEK_END); long e = ftell(f); fseek(f, p, SEEK_SET); return ret_i((int)(e - p)); }
        if (!strcmp(m->name, "close")) { fclose(f); thiz->priv = NULL; return zero(); }
        if (!strcmp(m->name, "skip")) { long p = ftell(f); fseek(f, (long)a[0].j, SEEK_CUR); JV r = zero(); r.j = ftell(f) - p; return r; }
    }

    /* --- AudioTrack (MODE_STREAM): the EA audio core writes mixed PCM here; audio.c plays it and blocks when full --- */
    if (!strcmp(c, "android/media/AudioTrack")) {
        const char *n = m->name;
        if (!strcmp(n, "write") && a[0].l) {
            JObj *arr = a[0].l; int off = a[1].i, len = a[2].i;
            if (!strcmp(m->sig, "([SII)I")) return ret_i(audio_write((const int16_t *)arr->data + off, len));
            if (!strcmp(m->sig, "([BII)I")) { audio_write((const int16_t *)((const uint8_t *)arr->data + off), len / 2); return ret_i(len); }
        }
        if (!strcmp(n, "play")) { audio_set_paused(0); return zero(); }
        if (!strcmp(n, "pause") || !strcmp(n, "stop")) { audio_set_paused(1); return zero(); }
        if (!strcmp(n, "getPlayState")) return ret_i(3);                     /* PLAYSTATE_PLAYING */
        if (!strcmp(n, "getState")) return ret_i(1);                         /* STATE_INITIALIZED */
        if (!strcmp(n, "getPlaybackHeadPosition")) return ret_i((int)audio_frames_played());
        if (!strcmp(n, "getMinBufferSize")) return ret_i(8192);
        if (!strcmp(n, "setStereoVolume") || !strcmp(n, "setVolume")) return ret_i(0);
    }

    /* --- video player: no playback on the Vita yet; a video "ends" at once so splash screens move on --- */
    if (!strcmp(c, "com/ea/VideoPlayer/PlayerAndroid")) {
        if (!strcmp(m->name, "play")) { g_video_obj = thiz; g_video_done = 1; return zero(); }
        if (!strcmp(m->name, "isPlaying")) return ret_i(0);
    }

    /* --- storage --- */
    if (!strcmp(c, "com/ea/EAMIO/StorageDirectory")) {
        if (!strcmp(m->name, "GetPrimaryExternalStorageState")) return ret_s("mounted");
        if (!strcmp(m->name, "GetPrimaryExternalStorageDirectory") || !strcmp(m->name, "GetDedicatedDirectory") ||
            !strcmp(m->name, "GetInternalStorageDirectory")) return ret_s(DATA_DIR "/files");
    }

    /* --- display / system --- */
    if (!strcmp(c, "com/ea/blast/DisplayAndroidDelegate")) {
        if (!strcmp(m->name, "GetDefaultWidth")) return ret_i(960);
        if (!strcmp(m->name, "GetDefaultHeight")) return ret_i(544);
        if (!strcmp(m->name, "GetDpiX") || !strcmp(m->name, "GetDpiY")) return ret_f(220.0f);
        if (!strcmp(m->name, "GetStdOrientation")) {
            int (*f)(void *, void *) = elf_sym("Java_com_ea_blast_DisplayAndroidDelegate_NativeGetOrientationNormal");
            return ret_i(f ? f(jni_env(), jni_class(c)) : 0);
        }
    }
    if (!strcmp(c, "com/ea/blast/SystemAndroidDelegate")) {
        const char *n = m->name;
        if (!strcmp(n, "GetLanguage")) return ret_s("en");
        if (!strcmp(n, "GetLocale")) return ret_s("en_US");
        if (!strcmp(n, "GetDeviceModel") || !strcmp(n, "GetDeviceName")) return ret_s("PSVita");
        if (!strcmp(n, "GetManufacturer")) return ret_s("Sony");
        if (!strcmp(n, "GetApiLevel")) return ret_s("10");
        if (!strcmp(n, "GetPlatformVersion")) return ret_s("2.3.3");
        if (!strcmp(n, "GetApplicationVersion")) return ret_s("1.1.1");
        if (!strcmp(n, "GetApplicationVersionCode")) return ret_i(1);
        if (!strcmp(n, "GetChipset") || !strcmp(n, "GetFirmware")) return ret_s("vita");
        if (!strcmp(n, "GetProcessorArchitecture")) return ret_s("armeabi");
        if (!strcmp(n, "GetHardwareFloatingPointSupport")) return ret_s("VFPv2");
        if (!strcmp(n, "GetDeviceUniqueId")) return ret_s("0000000000000001");
        if (!strcmp(n, "GetTouchScreenCount")) return ret_i(1);
        if (!strcmp(n, "GetAccelerometerCount") || !strcmp(n, "GetGyroscopeCount")) return ret_i(0);
        if (!strncmp(n, "Get", 3) && strlen(n) > 5 && !strcmp(n + strlen(n) - 5, "Count")) return ret_i(0);
        if (!strcmp(n, "GetDeviceSubscriberID") || !strcmp(n, "GetPhoneNumber")) return ret_s("");
        if (!strcmp(n, "IntentView")) return ret_i(0);
    }
    if (!strcmp(m->name, "IsAppInstalledOnSdCard") || !strcmp(m->name, "isPlaying")) return ret_i(0);
    return zero();                                       /* everything else: logged above, NULL/0 */
}
