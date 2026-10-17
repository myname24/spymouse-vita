/* A fake JavaVM / JNIEnv for libSpyMouse.so. Classes, methods, fields and strings are small records; Java calls from the game go
 * through java_invoke() -> java_dispatch() (java.c), which logs and answers them. Unimplemented slots log their index
 * (gen/jni_slots.inc). Table indices follow the JNI specification: 4 GetVersion, 6 FindClass, 15..17 exceptions, 21..25 refs,
 * 31 GetObjectClass, 33 GetMethodID, 34..63 Call<T>Method (variadic, V, A per type), 64..93 CallNonvirtual, 94 GetFieldID,
 * 113 GetStaticMethodID, 114..143 CallStatic, 144 GetStaticFieldID, 167..170 string UTF, 171..214 arrays, 215 RegisterNatives,
 * 219 GetJavaVM, 228 ExceptionCheck. Types in each family: Object, Boolean, Byte, Char, Short, Int, Long, Float, Double, Void. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "sm.h"
#include "jni_priv.h"

#include "gen/jni_slots.inc"

static void *table[233];
static void *env_ptr = table;                       /* JNIEnv* = pointer to the function table pointer */
static void *vm_table[8];
static void *vm_ptr = vm_table;
static JObj *pending;                               /* pending exception, if any */

static JObj *mk(int kind, const char *name, const char *sig, JObj *cls)
{
    JObj *o = calloc(1, sizeof *o);
    o->kind = kind; o->cls = cls;
    if (name) snprintf(o->name, sizeof o->name, "%s", name);
    if (sig) snprintf(o->sig, sizeof o->sig, "%s", sig);
    return o;
}
static JObj *intern(int kind, const char *name, const char *sig, JObj *cls)
{
    static JObj *tab[4096]; static int n;
    for (int i = 0; i < n; i++)
        if (tab[i]->kind == kind && tab[i]->cls == cls && !strcmp(tab[i]->name, name) && (!sig || !strcmp(tab[i]->sig, sig))) return tab[i];
    JObj *o = mk(kind, name, sig, cls);
    if (n < 4096) tab[n++] = o;
    return o;
}
JObj *jni_class(const char *name) { return intern(K_CLASS, name, NULL, NULL); }
JObj *jni_object(const char *cls) { return mk(K_OBJECT, cls, NULL, jni_class(cls)); }
JObj *jni_string(const char *s) { return mk(K_STRING, s ? s : "", NULL, jni_class("java/lang/String")); }
JObj *jni_array(const char *elem, int len, int esz)
{
    JObj *o = mk(K_ARRAY, "array", elem, NULL);
    o->len = len; o->data = calloc(len ? len : 1, esz);
    return o;
}
void jni_throw(const char *cls) { pending = jni_object(cls); }

static int    j_GetVersion(void *e) { return 0x00010006; }
static void  *j_FindClass(void *e, const char *n) { sm_log("JNI: FindClass %s\n", n); return jni_class(n); }
static void  *j_GetObjectClass(void *e, JObj *o) { return o && o->cls ? o->cls : jni_class("java/lang/Object"); }
static void  *j_Ref(void *e, void *o) { return o; }
static void   j_Unref(void *e, void *o) { }
static int    j_IsSame(void *e, void *a, void *b) { return a == b; }
static void  *j_Null(void *e) { return NULL; }
static void   j_Void(void *e) { }
static int    j_Zero(void *e) { return 0; }
static void  *j_PopLocalFrame(void *e, void *r) { return r; }
static void  *j_ExceptionOccurred(void *e) { return pending; }
static void   j_ExceptionClear(void *e) { pending = NULL; }
static int    j_ExceptionCheck(void *e) { return pending != NULL; }
static void  *j_GetMethodID(void *e, JObj *c, const char *n, const char *s) { return intern(K_METHOD, n, s, c); }
static void  *j_GetFieldID(void *e, JObj *c, const char *n, const char *s) { sm_log("JNI: GetFieldID %s.%s %s\n", c ? c->name : "?", n, s); return intern(K_FIELD, n, s, c); }
static void  *j_NewStringUTF(void *e, const char *s) { return jni_string(s); }
static int    j_GetStringUTFLength(void *e, JObj *o) { return (int)strlen(o->name); }
static const char *j_GetStringUTFChars(void *e, JObj *o, uint8_t *copy) { if (copy) *copy = 0; return o ? o->name : ""; }
static void   j_ReleaseStringUTFChars(void *e, JObj *o, const char *c) { }
static int    j_RegisterNatives(void *e, JObj *c, const void *m, int n)
{
    const char *const *p = m;
    for (int i = 0; i < n; i++) sm_log("JNI: RegisterNatives %s.%s%s -> %p\n", c ? c->name : "?", p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
    return 0;
}
static int    j_GetJavaVM(void *e, void **vm) { *vm = &vm_ptr; return 0; }     /* JavaVM* = address of the pointer to the table */

/* ---- arrays ---- */
static int    j_GetArrayLength(void *e, JObj *a) { return a ? a->len : 0; }
static void  *j_NewObjectArray(void *e, int len, JObj *cls, JObj *init)
{
    JObj *a = jni_array(cls ? cls->name : "Ljava/lang/Object;", len, sizeof(void *));
    JObj **d = a->data;
    for (int i = 0; i < len; i++) d[i] = init;
    return a;
}
static void  *j_GetObjectArrayElement(void *e, JObj *a, int i) { return a && i >= 0 && i < a->len ? ((JObj **)a->data)[i] : NULL; }
static void   j_SetObjectArrayElement(void *e, JObj *a, int i, JObj *v) { if (a && i >= 0 && i < a->len) ((JObj **)a->data)[i] = v; }
#define ARR(T, ESZ, DESC) \
    static void *na_##T(void *e, int len) { return jni_array(DESC, len, ESZ); } \
    static void *gae_##T(void *e, JObj *a, uint8_t *copy) { if (copy) *copy = 0; return a->data; } \
    static void  rae_##T(void *e, JObj *a, void *p, int mode) { } \
    static void  gar_##T(void *e, JObj *a, int s, int n, void *buf) { memcpy(buf, (uint8_t *)a->data + (size_t)s * ESZ, (size_t)n * ESZ); } \
    static void  sar_##T(void *e, JObj *a, int s, int n, const void *buf) { memcpy((uint8_t *)a->data + (size_t)s * ESZ, buf, (size_t)n * ESZ); }
ARR(Z, 1, "Z") ARR(B, 1, "B") ARR(C, 2, "C") ARR(S, 2, "S") ARR(I, 4, "I") ARR(J, 8, "J") ARR(F, 4, "F") ARR(D, 8, "D")

/* ---- Call<T>Method: argument unpacking from "..." / va_list / jvalue[] using the method signature ---- */
static void unpack_va(const char *sig, va_list ap, JV *out)
{
    int n = 0;
    for (const char *p = sig + 1; *p && *p != ')' && n < 16; n++) {
        switch (*p) {
        case 'J': out[n].j = va_arg(ap, int64_t); p++; break;
        case 'D': out[n].d = va_arg(ap, double); p++; break;
        case 'F': out[n].f = (float)va_arg(ap, double); p++; break;
        case 'L': out[n].l = va_arg(ap, void *); while (*p != ';') p++; p++; break;
        case '[': out[n].l = va_arg(ap, void *); while (*p == '[') p++; if (*p == 'L') while (*p != ';') p++; p++; break;
        default:  out[n].i = va_arg(ap, int); p++; break;      /* Z B C S I */
        }
    }
}
static JV java_invoke(JObj *m, JObj *thiz, const JV *a)
{
    if (!m || m->kind != K_METHOD) { JV z; memset(&z, 0, sizeof z); return z; }
    return java_dispatch(m, thiz, a);
}

#define RET_I  intptr_t, (intptr_t)r.l,
#define BODY_V(OBJ, M) JV a[16]; va_list ap; va_start(ap, M); unpack_va(M->sig, ap, a); va_end(ap); JV r = java_invoke(M, OBJ, a);
#define BODY_VL(OBJ, M) JV a[16]; unpack_va(M->sig, ap, a); JV r = java_invoke(M, OBJ, a);
#define BODY_A(OBJ, M) JV r = java_invoke(M, OBJ, a);
#define DEF_FAM(NAME, RT, CONV, ATTR) \
    ATTR static RT NAME##_v(void *e, JObj *o, JObj *m, ...) { BODY_V(o, m) return CONV; } \
    ATTR static RT NAME##_V(void *e, JObj *o, JObj *m, va_list ap) { BODY_VL(o, m) return CONV; } \
    ATTR static RT NAME##_A(void *e, JObj *o, JObj *m, const JV *a) { BODY_A(o, m) return CONV; } \
    ATTR static RT NAME##_nv(void *e, JObj *o, void *c, JObj *m, ...) { BODY_V(o, m) return CONV; } \
    ATTR static RT NAME##_nV(void *e, JObj *o, void *c, JObj *m, va_list ap) { BODY_VL(o, m) return CONV; } \
    ATTR static RT NAME##_nA(void *e, JObj *o, void *c, JObj *m, const JV *a) { BODY_A(o, m) return CONV; } \
    ATTR static RT NAME##_sv(void *e, void *c, JObj *m, ...) { BODY_V(NULL, m) return CONV; } \
    ATTR static RT NAME##_sV(void *e, void *c, JObj *m, va_list ap) { BODY_VL(NULL, m) return CONV; } \
    ATTR static RT NAME##_sA(void *e, void *c, JObj *m, const JV *a) { BODY_A(NULL, m) return CONV; }
DEF_FAM(ci, intptr_t, (intptr_t)r.l, )
DEF_FAM(cj, int64_t, r.j, )
DEF_FAM(cf, float, r.f, AAPCS)
DEF_FAM(cd, double, r.d, AAPCS)

/* AllocObject / NewObject (variadic, V, A): an object of the class; constructors have no behaviour yet */
static void *j_AllocObject(void *e, JObj *c) { return jni_object(c ? c->name : "java/lang/Object"); }
static void *j_NewObject(void *e, JObj *c, JObj *m, ...) { sm_log("JNI: NewObject %s%s\n", c ? c->name : "?", m ? m->sig : ""); return j_AllocObject(e, c); }
static void *j_NewObjectV(void *e, JObj *c, JObj *m, va_list ap) { return j_NewObject(e, c, m); }
static void *j_NewObjectA(void *e, JObj *c, JObj *m, const JV *a) { return j_NewObject(e, c, m); }

/* DestroyJavaVM, AttachCurrentThread, DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon */
static int v_Zero(void *vm) { return 0; }
static int v_Attach(void *vm, void **env, void *args) { *env = &env_ptr; return 0; }       /* JNIEnv* = address of the table pointer */
static int v_GetEnv(void *vm, void **env, int ver) { *env = &env_ptr; return 0; }

void *jni_vm(void)
{
    static int done;
    if (!done) {
        done = 1;
        memcpy(table, jslots, sizeof table);
        static void *const fam[10][9] = {
#define ROW(P) { P##_v, P##_V, P##_A, P##_nv, P##_nV, P##_nA, P##_sv, P##_sV, P##_sA }
            ROW(ci), ROW(ci), ROW(ci), ROW(ci), ROW(ci), ROW(ci), ROW(cj), ROW(cf), ROW(cd), ROW(ci) };
        for (int t = 0; t < 10; t++) for (int k = 0; k < 3; k++) {
            table[34 + 3 * t + k] = fam[t][k]; table[64 + 3 * t + k] = fam[t][3 + k]; table[114 + 3 * t + k] = fam[t][6 + k];
        }
        table[4] = j_GetVersion;   table[6] = j_FindClass;     table[15] = j_ExceptionOccurred; table[16] = j_Void;
        table[17] = j_ExceptionClear; table[19] = j_Zero;      table[20] = j_PopLocalFrame;
        table[21] = j_Ref;         table[22] = j_Unref;        table[23] = j_Unref;      table[24] = j_IsSame;
        table[25] = j_Ref;         table[26] = j_Zero;         table[31] = j_GetObjectClass;
        table[27] = j_AllocObject; table[28] = j_NewObject;    table[29] = j_NewObjectV; table[30] = j_NewObjectA;
        table[33] = j_GetMethodID; table[94] = j_GetFieldID;   table[113] = j_GetMethodID;
        table[144] = j_GetFieldID; table[167] = j_NewStringUTF; table[168] = j_GetStringUTFLength;
        table[169] = j_GetStringUTFChars; table[170] = j_ReleaseStringUTFChars;
        table[171] = j_GetArrayLength; table[172] = j_NewObjectArray;
        table[173] = j_GetObjectArrayElement; table[174] = j_SetObjectArrayElement;
        void *(*const na[8])(void *, int) = { na_Z, na_B, na_C, na_S, na_I, na_J, na_F, na_D };
        void *(*const gae[8])(void *, JObj *, uint8_t *) = { gae_Z, gae_B, gae_C, gae_S, gae_I, gae_J, gae_F, gae_D };
        void (*const rae[8])(void *, JObj *, void *, int) = { rae_Z, rae_B, rae_C, rae_S, rae_I, rae_J, rae_F, rae_D };
        void (*const gar[8])(void *, JObj *, int, int, void *) = { gar_Z, gar_B, gar_C, gar_S, gar_I, gar_J, gar_F, gar_D };
        void (*const sar[8])(void *, JObj *, int, int, const void *) = { sar_Z, sar_B, sar_C, sar_S, sar_I, sar_J, sar_F, sar_D };
        for (int t = 0; t < 8; t++) {
            table[175 + t] = na[t]; table[183 + t] = gae[t]; table[191 + t] = rae[t]; table[199 + t] = gar[t]; table[207 + t] = sar[t];
        }
        table[215] = j_RegisterNatives; table[217] = j_Zero;   table[218] = j_Zero;
        table[219] = j_GetJavaVM;  table[226] = j_Ref;         table[227] = j_Unref;     table[228] = j_ExceptionCheck;
        vm_table[3] = v_Zero; vm_table[4] = v_Attach; vm_table[5] = v_Zero; vm_table[6] = v_GetEnv; vm_table[7] = v_Attach;
    }
    return &vm_ptr;
}
void *jni_env(void) { jni_vm(); return &env_ptr; }
