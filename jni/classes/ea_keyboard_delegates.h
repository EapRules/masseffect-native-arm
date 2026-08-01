#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * The two keyboard delegates com/ea/blast asks for, in one file because they
 * are one subsystem: the engine builds both during platform startup and routes
 * text entry between them.
 *
 * Neither exists on this device in any real sense - an R36S has a D-pad and
 * face buttons and nothing else - but "no such class" and "a keyboard that is
 * never visible" are very different answers. A missing class makes NewObject
 * return NULL, and the engine keeps the NULL as its delegate; every method it
 * later calls on it goes through jni.cpp's null guard and answers 0, which for
 * IsVisible()Z means "the virtual keyboard is up" is never contradicted and for
 * a void method means the engine's own state is never updated.
 *
 * The method names and descriptors are transcribed from the run log, where
 * jni.cpp prints exactly what GetMethodID was asked for, rather than guessed:
 * lookup is an exact strcmp on name *and* descriptor, so a plausible-looking
 * signature fails identically to a class that was never registered.
 */

/* com/ea/blast/VirtualKeyboardAndroidDelegate — the on-screen keyboard. */
class VirtualKeyboardAndroidDelegate : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static void    ctor(JNIEnv *env, jobject obj, jclass clazz);
    static jboolean IsVisible(JNIEnv *env, jobject obj);
    static void    OnPhysicalKeyboardVisibilityChanged(JNIEnv *env, jobject obj, jboolean visible);
    static void    SetEnterKeyLabel(JNIEnv *env, jobject obj, jint label);
    static void    SetLayout(JNIEnv *env, jobject obj, jint layout);
    static void    SetShiftEnabled(JNIEnv *env, jobject obj, jboolean enabled);
    static void    Shutdown(JNIEnv *env, jobject obj);
    static void    UserSetVisible(JNIEnv *env, jobject obj, jboolean visible);
};

/*
 * com/ea/easp/VirtualKeyboardAndroidDelegate — EASP's own keyboard delegate.
 *
 * A different class from com/ea/blast/VirtualKeyboardAndroidDelegate above,
 * in a different package, with four extra methods. Registering the blast one
 * did nothing for this: FindClass failed on the easp path and every lookup
 * under it came back NULL, including the SetCursor(II)V the device log flagged.
 *
 * The names come from the run log, which prints exactly what GetMethodID was
 * asked for, in order. This class is text entry for EASP's login and store
 * forms - screens this port never reaches with the network killed - so every
 * body is a traced no-op and IsVisible answers false.
 */
class EaspVirtualKeyboardAndroidDelegate : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static void     ctor(JNIEnv *env, jobject obj, jclass clazz);
    static jboolean IsVisible(JNIEnv *env, jobject obj);
    static void     OnPhysicalKeyboardVisibilityChanged(JNIEnv *env, jobject obj, jboolean v);
    static void     SetEnterKeyLabel(JNIEnv *env, jobject obj, jint label);
    static void     SetLayout(JNIEnv *env, jobject obj, jint layout);
    static void     SetShiftEnabled(JNIEnv *env, jobject obj, jboolean enabled);
    static void     Shutdown(JNIEnv *env, jobject obj);
    static void     UserSetVisible(JNIEnv *env, jobject obj, jboolean visible);
    static void     SetText(JNIEnv *env, jobject obj, jstring text);
    static void     SetMaxTextLength(JNIEnv *env, jobject obj, jint max);
    static void     OnUpdate(JNIEnv *env, jobject obj);
    static void     SetCursor(JNIEnv *env, jobject obj, jint start, jint end);
};

/* com/ea/blast/PhysicalKeyboardAndroidDelegate — the Xperia Play slider. */
class PhysicalKeyboardAndroidDelegate : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static void     ctor(JNIEnv *env, jobject obj, jclass clazz);
    static jboolean IsNavigationVisible(JNIEnv *env, jobject obj);
};
