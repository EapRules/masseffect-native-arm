#include <stdlib.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "ea_DisplayAndroidDelegate.h"

/*
 * The panel the engine is told about: the real landscape framebuffer.
 *
 * This file used to report 480x640 - the panel in Android's "natural portrait
 * orientation" - on the theory that the engine performs the landscape swap
 * itself, the way the Vita port reports its 960x544 surface as 544x960. That
 * was wrong for this engine, and it is what shipped to the first hardware
 * test: the image came back shifted left and up with about 480 of the 640
 * columns filled and the rest black.
 *
 * The engine does not swap. It passes this value straight to glViewport, which
 * src/symtab_glprobe.cpp logs:
 *
 *     reported 480x640  ->  GL viewport: x=0 y=0 width=480 height=640
 *     reported 640x480  ->  GL viewport: x=0 y=0 width=640 height=480
 *
 * Both measured under the harness, whose drawable is 640x480, and the second
 * is the one that matches it. Only one distinct viewport is issued in a whole
 * 600-frame run, so there is no second render target this would disturb.
 *
 * The lesson worth keeping: the old comment described a swap that was never
 * observed, and reading like a measurement is what made it survive. The two
 * lines above are the measurement.
 */
static const int kDefaultWidth  = 640;
static const int kDefaultHeight = 480;

/*
 * 229 dpi: the panel is 3.5" diagonal at 640x480, so sqrt(640^2 + 480^2) / 3.5.
 * The engine scales UI metrics by this, so a made-up round number would make
 * text and touch targets the wrong physical size. (The Vita port reports 200,
 * which is likewise an approximation of *its* real 220.)
 */
static const float kDefaultDpi = 229.0f;

static int env_int(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return fallback;
    int n = atoi(v);
    return n > 0 ? n : fallback;
}

void DisplayAndroidDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env; (void)obj; (void)clazz;
    trace("DisplayAndroidDelegate constructed (%dx%d @ %.0f dpi)",
          env_int("MASSEFFECT_SCREEN_W", kDefaultWidth),
          env_int("MASSEFFECT_SCREEN_H", kDefaultHeight),
          (double)kDefaultDpi);
}

jint DisplayAndroidDelegate::GetDefaultWidth(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return env_int("MASSEFFECT_SCREEN_W", kDefaultWidth);
}

jint DisplayAndroidDelegate::GetDefaultHeight(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return env_int("MASSEFFECT_SCREEN_H", kDefaultHeight);
}

jfloat DisplayAndroidDelegate::GetDpiX(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return (jfloat)env_int("MASSEFFECT_SCREEN_DPI", (int)kDefaultDpi);
}

jfloat DisplayAndroidDelegate::GetDpiY(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return (jfloat)env_int("MASSEFFECT_SCREEN_DPI", (int)kDefaultDpi);
}

/*
 * Orientation 0 - the display is in its default orientation and stays there.
 *
 * This is a handheld with a fixed landscape panel and no rotation sensor worth
 * the name, so there is no honest second value to return. The engine asks once
 * at startup and then trusts it.
 */
jint DisplayAndroidDelegate::GetStdOrientation(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return 0;
}

/*
 * Ignored on purpose. The engine sets orientation when it thinks the device
 * rotated; nothing here can rotate, and pretending to accept it is the same
 * answer as accepting it - the next GetStdOrientation still says 0.
 */
void DisplayAndroidDelegate::SetStdOrientation(JNIEnv *env, jobject obj, jint orientation)
{
    (void)env; (void)obj;
    trace("DisplayAndroidDelegate.SetStdOrientation(%d) ignored - "
          "this panel does not rotate", (int)orientation);
}

jint DisplayAndroidDelegate::GetDisplayOrientationLock(JNIEnv *env, jobject obj)
{
    return GetStdOrientation(env, obj);
}

const ManagedMethod DisplayAndroidDelegateMethods[] = {
    ManagedMethod::RegisterNonVirtual<&DisplayAndroidDelegate::ctor>(
        DisplayAndroidDelegate::clazz, "<init>", "()V"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDisplayOrientationLock>(
        DisplayAndroidDelegate::clazz, "GetDisplayOrientationLock", "()I"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDefaultWidth>(
        DisplayAndroidDelegate::clazz, "GetDefaultWidth", "()I"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDefaultHeight>(
        DisplayAndroidDelegate::clazz, "GetDefaultHeight", "()I"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDpiX>(
        DisplayAndroidDelegate::clazz, "GetDpiX", "()F"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetDpiY>(
        DisplayAndroidDelegate::clazz, "GetDpiY", "()F"),
    ManagedMethod::Register<&DisplayAndroidDelegate::GetStdOrientation>(
        DisplayAndroidDelegate::clazz, "GetStdOrientation", "()I"),
    ManagedMethod::Register<&DisplayAndroidDelegate::SetStdOrientation>(
        DisplayAndroidDelegate::clazz, "SetStdOrientation", "(I)V"),
    {NULL},
};

Class DisplayAndroidDelegate::clazz = {
    .classpath        = "com/ea/blast/DisplayAndroidDelegate",
    .classname        = "DisplayAndroidDelegate",
    .managed_methods  = DisplayAndroidDelegateMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(DisplayAndroidDelegate),
};

static const int registered =
    ClassRegistry::register_class(DisplayAndroidDelegate::clazz);
