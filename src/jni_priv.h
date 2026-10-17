/* Shared by jni.c (the JNIEnv table) and java.c (what the Java side of the Android app did). */
#ifndef JNI_PRIV_H
#define JNI_PRIV_H
#include <stdint.h>

enum { K_CLASS = 1, K_METHOD, K_FIELD, K_STRING, K_OBJECT, K_ARRAY };

typedef struct JObj {
    int kind;
    char name[96];            /* class/method/field name, string contents, or the class name of an object */
    char sig[96];             /* method/field signature; for K_ARRAY the element descriptor ("B", "I", "Ljava/lang/String;") */
    struct JObj *cls;
    void *priv;               /* java.c: FILE* of an InputStream, ... */
    int len;                  /* arrays: element count */
    void *data;               /* arrays: element storage */
} JObj;

typedef union { int32_t i; int64_t j; float f; double d; void *l; } JV;      /* a jvalue / a call result */

JObj *jni_class(const char *name);
JObj *jni_object(const char *cls);
JObj *jni_string(const char *s);
JObj *jni_array(const char *elem, int len, int esz);
void  jni_throw(const char *cls);                   /* set the pending exception (ExceptionCheck returns true until cleared) */

/* a video "played": java.c sets these on PlayerAndroid.play(); main.c then calls OnCompletionNativeImpl on the next frame */
extern JObj *g_video_obj;
extern volatile int g_video_done;

/* java.c: the emulated Java methods. Returns a zeroed result for anything unknown (and logs it). */
JV java_dispatch(const JObj *m, JObj *thiz, const JV *args);
#endif
