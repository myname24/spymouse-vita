/* GLES 1.1 entry points with scalar float arguments. The guest passes them in core registers (softfp); vitaGL expects VFP
 * registers, so each gets an AAPCS wrapper. tools/gen.py finds the "w_gl..." names here and binds them in gen/gen_syms.c;
 * every other gl* import goes straight to vitaGL, or to a logging stub when vitaGL lacks it. */
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>
#include "sm.h"

/* texture upload trace: the first uploads with sizes and the GL error after each */
extern volatile int sm_trace;                       /* imports.c: set while the game's retry loop is traced */
GLuint gl_last_tex;                                 /* the texture most recently uploaded by the game (main.c's overlay draws it untouched) */
static GLuint cur_tex;                              /* currently bound GL_TEXTURE_2D */
static GLboolean cmask[4] = { 1, 1, 1, 1 };         /* current glColorMask, see w_glClear */

/* The game sets a texture's filter/wrap parameters BEFORE uploading its pixels; vitaGL resets them to the defaults (mipmapped
 * minification filter, no mipmaps = incomplete = white quads) when the image arrives. Remember what the game set per texture and
 * apply it again right after each upload (Vita test 14: game textures white, a test texture with parameters after upload fine). */
typedef struct { GLint min, mag, ws, wt, gen; } TexParams;
static TexParams tps[4096];
static void tp_record(GLenum p, GLint v)
{
    if (cur_tex >= 4096) return;
    switch (p) { case 0x2801: tps[cur_tex].min = v; break; case 0x2800: tps[cur_tex].mag = v; break;
                 case 0x2802: tps[cur_tex].ws = v; break;  case 0x2803: tps[cur_tex].wt = v; break;
                 case 0x8191: tps[cur_tex].gen = v; break; }            /* GL_GENERATE_MIPMAP (GLES1 auto-mipmaps) */
}
static int is_mip_filter(GLint f) { return f >= 0x2700 && f <= 0x2703; }
/* After an upload: restore the game's parameters. Level 0 with a mipmapped minification filter and no mipmaps would be incomplete
 * (white): generate the mipmaps when the game asked for GL_GENERATE_MIPMAP, otherwise fall back to the matching plain filter.
 * A texture the game never gave a filter gets GL_LINEAR (the GLES default would be incomplete too). */
static void tp_reapply(int lv)
{
    if (cur_tex >= 4096) return;
    const TexParams *t = &tps[cur_tex];
    GLint min = t->min ? t->min : 0x2601, mag = t->mag ? t->mag : 0x2601;
    if (lv == 0 && is_mip_filter(min)) {
        if (t->gen) glGenerateMipmap(GL_TEXTURE_2D);
        else min = (min == 0x2700 || min == 0x2702) ? 0x2600 : 0x2601;
    }
    glTexParameteri(GL_TEXTURE_2D, 0x2801, min);
    glTexParameteri(GL_TEXTURE_2D, 0x2800, mag);
    if (t->ws) glTexParameteri(GL_TEXTURE_2D, 0x2802, t->ws);
    if (t->wt) glTexParameteri(GL_TEXTURE_2D, 0x2803, t->wt);
}
static volatile uint32_t st_tex_n, st_tex_us, st_tex_kb;
void sm_tex_stats(unsigned *v) { v[0] = st_tex_n; v[1] = st_tex_us / 1000; v[2] = st_tex_kb; st_tex_n = st_tex_us = st_tex_kb = 0; }
void w_glTexImage2D(GLenum tg, GLint lv, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum ty, const void *px)
{
    static int n, ne;
    glGetError();                                       /* clear stale errors so the one after the call is this call's */
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glTexImage2D(tg, lv, ifmt, w, h, b, fmt, ty, px);
    st_tex_n++; st_tex_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0); st_tex_kb += (uint32_t)((uint64_t)w * h * (fmt == GL_RGB ? 3 : 4) / 1024);
    if (tg == GL_TEXTURE_2D) tp_reapply(lv);
    if (tg == GL_TEXTURE_2D && lv == 0 && w >= 32 && h >= 32) gl_last_tex = cur_tex;
    GLenum e = glGetError();
    if ((e && ne < 40) || (sm_trace && n < 30)) {
        if (e) ne++; else n++;
        sm_log("tr: glTexImage2D tg=%#x lv=%d ifmt=%#x %dx%d fmt=%#x ty=%#x px=%p err=%#x  [vgl free: ram %uK vram %uK ext %uK]\n",
               tg, lv, ifmt, w, h, fmt, ty, px, e, (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024),
               (unsigned)(vglMemFree(VGL_MEM_EXTERNAL) / 1024));
    }
}
void w_glGenTextures(GLsizei n, GLuint *t)
{
    static int c;
    glGenTextures(n, t);
    if ((n > 0 && t[0] == 0 && c < 20) || (sm_trace && c < 20)) { c++; sm_log("tr: glGenTextures %d -> %u\n", n, n > 0 ? t[0] : 0); }
}

/* counters for the main loop's [gl] line */
volatile unsigned gl_draws, gl_clears;

/* call trace: main.c sets gl_log to a budget of calls to record (a black screen with 16 draws per frame needs the state the game sets) */
volatile int gl_log;
#define GLL(...) do { if (gl_log > 0) { gl_log--; sm_log(__VA_ARGS__); } } while (0)

/* Client array compatibility: GLES1 allows GL_SHORT / GL_BYTE / GL_FIXED vertex and texcoord arrays (the game's quads use GL_SHORT
 * texcoords). Remember what the game set; at draw time convert such arrays to floats (unnormalised, as GLES1 reads them) and point
 * vitaGL at the converted copy. */
typedef struct { const void *p; GLint size; GLenum type; GLsizei stride; } Arr;
static Arr varr, tarr;
static float vconv[8192], tconv[8192];
static int esz(GLenum t) { switch (t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; default: return 4; } }
static int convert(const Arr *a, int n, float *out)
{
    if (!a->p || (uintptr_t)a->p < 0x10000 || n <= 0 || n * a->size > 8192) return 0;
    int es = esz(a->type), stride = a->stride ? a->stride : a->size * es;
    for (int i = 0; i < n; i++) for (int c = 0; c < a->size; c++) {
        const uint8_t *q = (const uint8_t *)a->p + (size_t)i * stride + c * es;
        float v;
        switch (a->type) {
        case GL_BYTE: v = *(const int8_t *)q; break;
        case GL_UNSIGNED_BYTE: v = *q; break;
        case GL_SHORT: v = *(const int16_t *)q; break;
        case GL_UNSIGNED_SHORT: v = *(const uint16_t *)q; break;
        case 0x140C: { int32_t x; memcpy(&x, q, 4); v = x / 65536.0f; break; }     /* GL_FIXED */
        default: v = 0;
        }
        out[i * a->size + c] = v;
    }
    return 1;
}
static void fix_arrays(int n)
{
    if (varr.type != GL_FLOAT && varr.type && convert(&varr, n, vconv)) glVertexPointer(varr.size, GL_FLOAT, 0, vconv);
    if (tarr.type != GL_FLOAT && tarr.type && convert(&tarr, n, tconv)) {
        GLL("gl:   texcoords (type %#x) -> float: %g %g | %g %g | %g %g | %g %g\n", tarr.type, tconv[0], tconv[1], tconv[2], tconv[3], tconv[4], tconv[5], tconv[6], tconv[7]);
        glTexCoordPointer(tarr.size, GL_FLOAT, 0, tconv);
    }
}
static void dump_verts(int n)
{
    if (gl_log > 0 && varr.type == GL_FLOAT && varr.p && n >= 4 && varr.size >= 2) {
        const GLfloat *v = varr.p; int s = varr.size;
        GLL("gl:   verts: (%g %g) (%g %g) (%g %g) (%g %g)\n", v[0], v[1], v[s], v[s + 1], v[2 * s], v[2 * s + 1], v[3 * s], v[3 * s + 1]);
    }
}
void w_glDrawArrays(GLenum m, GLint f, GLsizei c)
{
    gl_draws++; GLL("gl: DrawArrays mode=%#x first=%d count=%d\n", m, f, c);
    dump_verts(f + c); fix_arrays(f + c);
    glDrawArrays(m, f, c);
}
void w_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i)
{
    gl_draws++; GLL("gl: DrawElements mode=%#x count=%d type=%#x idx=%p\n", m, c, t, i);
    if ((uintptr_t)i >= 0x10000 && (t == GL_UNSIGNED_SHORT || t == GL_UNSIGNED_BYTE)) {
        unsigned mx = 0;
        for (int k = 0; k < c; k++) { unsigned v = t == GL_UNSIGNED_SHORT ? ((const uint16_t *)i)[k] : ((const uint8_t *)i)[k]; if (v > mx) mx = v; }
        dump_verts(mx + 1); fix_arrays(mx + 1);
    }
    glDrawElements(m, c, t, i);
}
/* texture parameter calls happen when textures are created, long before the frame traces: log the first 80 */
static volatile int ntp;
void w_glTexParameteri(GLenum t, GLenum p, GLint v)
{
    if (ntp < 400) { ntp++; sm_log("gl: TexParameteri tex=%u %#x %#x %d\n", cur_tex, t, p, v); }
    if (t == GL_TEXTURE_2D) tp_record(p, v);
    glTexParameteri(t, p, v);
}
void w_glTexEnvi(GLenum t, GLenum p, GLint v) { if (ntp < 400) { ntp++; sm_log("gl: TexEnvi %#x %#x %#x\n", t, p, v); } glTexEnvi(t, p, v); }
void w_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    GLL("gl: ColorMask %d %d %d %d\n", r, g, b, a);
    cmask[0] = r; cmask[1] = g; cmask[2] = b; cmask[3] = a;
    glColorMask(r, g, b, a);
}
void w_glActiveTexture(GLenum t) { GLL("gl: ActiveTexture %#x\n", t); glActiveTexture(t); }
void w_glClientActiveTexture(GLenum t) { GLL("gl: ClientActiveTexture %#x\n", t); glClientActiveTexture(t); }
/* glColorMask is not honoured by vitaGL's glClear. The game ends every frame with glColorMask(0,0,0,1); glClear(COLOR);
 * glColorMask(1,1,1,1) (clear only alpha), which on the Vita wipes the finished picture to black before it is displayed. So a clear
 * while RGB writes are masked off must not clear colour at all. */
void w_glClear(GLbitfield m)
{
    gl_clears++; GLL("gl: Clear %#x (colour mask %d%d%d%d)\n", m, cmask[0], cmask[1], cmask[2], cmask[3]);
    if ((m & GL_COLOR_BUFFER_BIT) && !cmask[0] && !cmask[1] && !cmask[2]) m &= ~(GLbitfield)GL_COLOR_BUFFER_BIT;
    if (m) glClear(m);
}
void w_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { GLL("gl: Viewport %d %d %d %d\n", x, y, w, h); glViewport(x, y, w, h); }
void w_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { GLL("gl: Scissor %d %d %d %d\n", x, y, w, h); glScissor(x, y, w, h); }
void w_glEnable(GLenum c) { GLL("gl: Enable %#x\n", c); glEnable(c); }
void w_glDisable(GLenum c) { GLL("gl: Disable %#x\n", c); glDisable(c); }
void w_glBlendFunc(GLenum s, GLenum d) { GLL("gl: BlendFunc %#x %#x\n", s, d); glBlendFunc(s, d); }
void w_glMatrixMode(GLenum m) { GLL("gl: MatrixMode %#x\n", m); glMatrixMode(m); }
void w_glLoadIdentity(void) { GLL("gl: LoadIdentity\n"); glLoadIdentity(); }
void w_glLoadMatrixf(const GLfloat *m) { GLL("gl: LoadMatrixf %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]); glLoadMatrixf(m); }
void w_glBindTexture(GLenum t, GLuint n) { GLL("gl: BindTexture %#x %u\n", t, n); if (t == GL_TEXTURE_2D) cur_tex = n; glBindTexture(t, n); }
void w_glVertexPointer(GLint s, GLenum t, GLsizei st, const void *p) { GLL("gl: VertexPointer size=%d type=%#x stride=%d ptr=%p\n", s, t, st, p); varr = (Arr){ p, s, t, st }; glVertexPointer(s, t, st, p); }
void w_glTexCoordPointer(GLint s, GLenum t, GLsizei st, const void *p) { GLL("gl: TexCoordPointer size=%d type=%#x stride=%d ptr=%p\n", s, t, st, p); tarr = (Arr){ p, s, t, st }; glTexCoordPointer(s, t, st, p); }
void w_glColorPointer(GLint s, GLenum t, GLsizei st, const void *p) { GLL("gl: ColorPointer size=%d type=%#x stride=%d ptr=%p\n", s, t, st, p); glColorPointer(s, t, st, p); }
void w_glEnableClientState(GLenum c) { GLL("gl: EnableClientState %#x\n", c); glEnableClientState(c); }
void w_glDisableClientState(GLenum c) { GLL("gl: DisableClientState %#x\n", c); glDisableClientState(c); }
void w_glDepthMask(GLboolean f) { GLL("gl: DepthMask %d\n", f); glDepthMask(f); }
void w_glBindBuffer(GLenum t, GLuint b) { GLL("gl: BindBuffer %#x %u\n", t, b); glBindBuffer(t, b); }

AAPCS void w_glAlphaFunc(GLenum f, float r) { glAlphaFunc(f, r); }
AAPCS void w_glClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
AAPCS void w_glClearDepthf(float d) { glClearDepthf(d); }
AAPCS void w_glColor4f(float r, float g, float b, float a) { GLL("gl: Color4f %g %g %g %g\n", r, g, b, a); glColor4f(r, g, b, a); }
AAPCS void w_glDepthRangef(float n, float f) { glDepthRangef(n, f); }
AAPCS void w_glFogf(GLenum p, float v) { glFogf(p, v); }
AAPCS void w_glFrustumf(float l, float r, float b, float t, float n, float f) { GLL("gl: Frustumf %g %g %g %g %g %g\n", l, r, b, t, n, f); glFrustumf(l, r, b, t, n, f); }
AAPCS void w_glLightf(GLenum l, GLenum p, float v) { glLightfv(l, p, &v); }       /* vitaGL has only the vector forms */
AAPCS void w_glLightModelf(GLenum p, float v) { glLightModelfv(p, &v); }
AAPCS void w_glLineWidth(float w) { glLineWidth(w); }
AAPCS void w_glMaterialf(GLenum f, GLenum p, float v) { glMaterialf(f, p, v); }
AAPCS void w_glMultiTexCoord4f(GLenum t, float s, float u, float r, float q) { (void)r; (void)q; glMultiTexCoord2f(t, s, u); }
AAPCS void w_glNormal3f(float x, float y, float z) { glNormal3f(x, y, z); }
AAPCS void w_glOrthof(float l, float r, float b, float t, float n, float f) { GLL("gl: Orthof %g %g %g %g %g %g\n", l, r, b, t, n, f); glOrthof(l, r, b, t, n, f); }
AAPCS void w_glPointSize(float s) { glPointSize(s); }
AAPCS void w_glPolygonOffset(float f, float u) { glPolygonOffset(f, u); }
AAPCS void w_glRotatef(float a, float x, float y, float z) { GLL("gl: Rotatef %g %g %g %g\n", a, x, y, z); glRotatef(a, x, y, z); }
AAPCS void w_glScalef(float x, float y, float z) { GLL("gl: Scalef %g %g %g\n", x, y, z); glScalef(x, y, z); }
AAPCS void w_glTexEnvf(GLenum t, GLenum p, float v) { glTexEnvf(t, p, v); }
AAPCS void w_glTexParameterf(GLenum t, GLenum p, float v)
{
    if (ntp < 400) { ntp++; sm_log("gl: TexParameterf tex=%u %#x %#x %g\n", cur_tex, t, p, v); }
    if (t == GL_TEXTURE_2D) tp_record(p, (GLint)v);
    glTexParameterf(t, p, v);
}
AAPCS void w_glTranslatef(float x, float y, float z) { GLL("gl: Translatef %g %g %g\n", x, y, z); glTranslatef(x, y, z); }
