#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "ea_keyboard_delegates.h"

/* ------------------------------------------------------------------ virtual */

void VirtualKeyboardAndroidDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env; (void)obj; (void)clazz;
    trace("VirtualKeyboardAndroidDelegate constructed (never visible on this device)");
}

/*
 * The one method here whose answer matters.
 *
 * The engine asks this to decide whether the on-screen keyboard is covering the
 * view, and the Vita port - which runs this build - answers JNI_TRUE. That is
 * not what it looks like: on the Vita the IME genuinely is what text entry goes
 * through. This port has no IME and no touch screen, so the honest answer is
 * "there is no keyboard up", and saying otherwise would have the engine
 * reserving screen space for a panel that never appears.
 */
jboolean VirtualKeyboardAndroidDelegate::IsVisible(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return JNI_FALSE;
}

/*
 * The rest are notifications, not questions: the engine tells the delegate what
 * it has decided and reads nothing back. Accepting them and doing nothing is
 * the whole contract on a device with no keyboard to configure.
 */
void VirtualKeyboardAndroidDelegate::OnPhysicalKeyboardVisibilityChanged(
    JNIEnv *env, jobject obj, jboolean visible)
{
    (void)env; (void)obj; (void)visible;
}

void VirtualKeyboardAndroidDelegate::SetEnterKeyLabel(JNIEnv *env, jobject obj, jint label)
{
    (void)env; (void)obj; (void)label;
}

void VirtualKeyboardAndroidDelegate::SetLayout(JNIEnv *env, jobject obj, jint layout)
{
    (void)env; (void)obj; (void)layout;
}

void VirtualKeyboardAndroidDelegate::SetShiftEnabled(JNIEnv *env, jobject obj, jboolean enabled)
{
    (void)env; (void)obj; (void)enabled;
}

void VirtualKeyboardAndroidDelegate::Shutdown(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
}

void VirtualKeyboardAndroidDelegate::UserSetVisible(JNIEnv *env, jobject obj, jboolean visible)
{
    (void)env; (void)obj; (void)visible;
}

const ManagedMethod VirtualKeyboardAndroidDelegateMethods[] = {
    ManagedMethod::RegisterNonVirtual<&VirtualKeyboardAndroidDelegate::ctor>(
        VirtualKeyboardAndroidDelegate::clazz, "<init>", "()V"),
    ManagedMethod::Register<&VirtualKeyboardAndroidDelegate::IsVisible>(
        VirtualKeyboardAndroidDelegate::clazz, "IsVisible", "()Z"),
    ManagedMethod::Register<&VirtualKeyboardAndroidDelegate::OnPhysicalKeyboardVisibilityChanged>(
        VirtualKeyboardAndroidDelegate::clazz,
        "OnPhysicalKeyboardVisibilityChanged", "(Z)V"),
    ManagedMethod::Register<&VirtualKeyboardAndroidDelegate::SetEnterKeyLabel>(
        VirtualKeyboardAndroidDelegate::clazz, "SetEnterKeyLabel", "(I)V"),
    ManagedMethod::Register<&VirtualKeyboardAndroidDelegate::SetLayout>(
        VirtualKeyboardAndroidDelegate::clazz, "SetLayout", "(I)V"),
    ManagedMethod::Register<&VirtualKeyboardAndroidDelegate::SetShiftEnabled>(
        VirtualKeyboardAndroidDelegate::clazz, "SetShiftEnabled", "(Z)V"),
    ManagedMethod::Register<&VirtualKeyboardAndroidDelegate::Shutdown>(
        VirtualKeyboardAndroidDelegate::clazz, "Shutdown", "()V"),
    ManagedMethod::Register<&VirtualKeyboardAndroidDelegate::UserSetVisible>(
        VirtualKeyboardAndroidDelegate::clazz, "UserSetVisible", "(Z)V"),
    {NULL},
};

Class VirtualKeyboardAndroidDelegate::clazz = {
    .classpath        = "com/ea/blast/VirtualKeyboardAndroidDelegate",
    .classname        = "VirtualKeyboardAndroidDelegate",
    .managed_methods  = VirtualKeyboardAndroidDelegateMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(VirtualKeyboardAndroidDelegate),
};

static const int registered_virtual =
    ClassRegistry::register_class(VirtualKeyboardAndroidDelegate::clazz);


/* ------------------------------------------------------------- easp virtual */

void EaspVirtualKeyboardAndroidDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env; (void)obj; (void)clazz;
    trace("EaspVirtualKeyboardAndroidDelegate constructed (text entry, no-op)");
}

jboolean EaspVirtualKeyboardAndroidDelegate::IsVisible(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return JNI_FALSE;
}

void EaspVirtualKeyboardAndroidDelegate::OnPhysicalKeyboardVisibilityChanged(
    JNIEnv *env, jobject obj, jboolean v) { (void)env; (void)obj; (void)v; }
void EaspVirtualKeyboardAndroidDelegate::SetEnterKeyLabel(JNIEnv *env, jobject obj, jint label)
{ (void)env; (void)obj; (void)label; }
void EaspVirtualKeyboardAndroidDelegate::SetLayout(JNIEnv *env, jobject obj, jint layout)
{ (void)env; (void)obj; (void)layout; }
void EaspVirtualKeyboardAndroidDelegate::SetShiftEnabled(JNIEnv *env, jobject obj, jboolean e)
{ (void)env; (void)obj; (void)e; }
void EaspVirtualKeyboardAndroidDelegate::Shutdown(JNIEnv *env, jobject obj)
{ (void)env; (void)obj; }
void EaspVirtualKeyboardAndroidDelegate::UserSetVisible(JNIEnv *env, jobject obj, jboolean v)
{ (void)env; (void)obj; (void)v; }
void EaspVirtualKeyboardAndroidDelegate::SetText(JNIEnv *env, jobject obj, jstring text)
{ (void)env; (void)obj; (void)text; }
void EaspVirtualKeyboardAndroidDelegate::SetMaxTextLength(JNIEnv *env, jobject obj, jint max)
{ (void)env; (void)obj; (void)max; }
void EaspVirtualKeyboardAndroidDelegate::OnUpdate(JNIEnv *env, jobject obj)
{ (void)env; (void)obj; }

/*
 * The one the device log named. Traced rather than silent: the game calling it
 * is the only evidence we have about when it thinks a text field is focused,
 * and that is worth seeing if the pads ever behave oddly around a menu.
 */
void EaspVirtualKeyboardAndroidDelegate::SetCursor(JNIEnv *env, jobject obj,
                                                   jint start, jint end)
{
    (void)env; (void)obj;
    static int lines = 0;
    if (lines++ < 8)
        trace("EASP keyboard SetCursor(%d, %d)", (int)start, (int)end);
}

const ManagedMethod EaspVirtualKeyboardAndroidDelegateMethods[] = {
    ManagedMethod::RegisterNonVirtual<&EaspVirtualKeyboardAndroidDelegate::ctor>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "<init>", "()V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::IsVisible>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "IsVisible", "()Z"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::OnPhysicalKeyboardVisibilityChanged>(
        EaspVirtualKeyboardAndroidDelegate::clazz,
        "OnPhysicalKeyboardVisibilityChanged", "(Z)V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::SetEnterKeyLabel>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "SetEnterKeyLabel", "(I)V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::SetLayout>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "SetLayout", "(I)V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::SetShiftEnabled>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "SetShiftEnabled", "(Z)V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::Shutdown>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "Shutdown", "()V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::UserSetVisible>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "UserSetVisible", "(Z)V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::SetText>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "SetText", "(Ljava/lang/String;)V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::SetMaxTextLength>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "SetMaxTextLength", "(I)V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::OnUpdate>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "OnUpdate", "()V"),
    ManagedMethod::Register<&EaspVirtualKeyboardAndroidDelegate::SetCursor>(
        EaspVirtualKeyboardAndroidDelegate::clazz, "SetCursor", "(II)V"),
    {NULL},
};

Class EaspVirtualKeyboardAndroidDelegate::clazz = {
    .classpath        = "com/ea/easp/VirtualKeyboardAndroidDelegate",
    .classname        = "VirtualKeyboardAndroidDelegate",
    .managed_methods  = EaspVirtualKeyboardAndroidDelegateMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(EaspVirtualKeyboardAndroidDelegate),
};

static const int registered_easp_virtual =
    ClassRegistry::register_class(EaspVirtualKeyboardAndroidDelegate::clazz);

/* ----------------------------------------------------------------- physical */

void PhysicalKeyboardAndroidDelegate::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env; (void)obj; (void)clazz;
    trace("PhysicalKeyboardAndroidDelegate constructed (R36S buttons, no navigation bar)");
}

/*
 * Android's navigation bar - the soft back/home strip. There is none here: the
 * port owns the whole framebuffer. JNI_FALSE is also what the Vita port
 * answers.
 */
jboolean PhysicalKeyboardAndroidDelegate::IsNavigationVisible(JNIEnv *env, jobject obj)
{
    (void)env; (void)obj;
    return JNI_FALSE;
}

const ManagedMethod PhysicalKeyboardAndroidDelegateMethods[] = {
    ManagedMethod::RegisterNonVirtual<&PhysicalKeyboardAndroidDelegate::ctor>(
        PhysicalKeyboardAndroidDelegate::clazz, "<init>", "()V"),
    ManagedMethod::Register<&PhysicalKeyboardAndroidDelegate::IsNavigationVisible>(
        PhysicalKeyboardAndroidDelegate::clazz, "IsNavigationVisible", "()Z"),
    {NULL},
};

Class PhysicalKeyboardAndroidDelegate::clazz = {
    .classpath        = "com/ea/blast/PhysicalKeyboardAndroidDelegate",
    .classname        = "PhysicalKeyboardAndroidDelegate",
    .managed_methods  = PhysicalKeyboardAndroidDelegateMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(PhysicalKeyboardAndroidDelegate),
};

static const int registered_physical =
    ClassRegistry::register_class(PhysicalKeyboardAndroidDelegate::clazz);
