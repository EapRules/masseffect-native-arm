#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/ea/blast/SystemAndroidDelegate — the device capability profile.
 *
 * This is the single class the engine asks "what hardware am I on?". Every one
 * of the 28 getters below returns a *String*, which EAMCore::System parses and
 * stores under the property names that sit next to the method names in .rodata
 * (0x3da628..0x3da878): "sys.accelerometerCount", "sys.touchScreenCount",
 * "sys.dev.manufacturer", and so on. EAMCore::ModuleRegistry then instantiates
 * one Android module per piece of hardware the profile claims exists -
 * AccelerometerAndroid, EAMCore::DisplayAndroid, EAMCore::TouchScreenAndroid,
 * EAMCore::KeyboardAndroidXperiaPlay (all named in .rodata at 0x3f5da8).
 *
 * The names and signatures are transcribed from the binary, not guessed: the
 * 28 method names run consecutively from 0x3da3d0 to 0x3da620, "<init>" is at
 * 0x3da620 with "()V" at 0x3da110, and every getter shares the single
 * "()Ljava/lang/String;" literal at 0x3d5288 - there is no other copy of that
 * string in the file.
 *
 * Instance methods, not static - same shape as GetAppDataDirectoryDelegate,
 * whose header records what the log says when that is got wrong.
 */

/*
 * name -> value, in the order the engine looks them up.
 *
 * The values mirror the Vita port's jni_specific.h (which is a working
 * configuration for this exact binary) except where noted. Divergence here is
 * not cosmetic: each count is a module the engine will build and then assume
 * exists, so lowering one to match the R36S's real hardware removes a module
 * and can strand a consumer that never checks - which is precisely the bug
 * this class exists to fix.
 *
 * The two that are load-bearing rather than informational:
 *
 *   GetManufacturer / GetDeviceName - "sony" and "R800" are compared inside
 *   the engine (the two literals sit together at .rodata 0x3f5e30, immediately
 *   before "EAMCore::KeyboardAndroidXperiaPlay"). Reporting an Xperia Play is
 *   what selects the physical-button input module instead of the on-screen
 *   one, which is what an R36S wants. GetDeviceModel is *not* compared - the
 *   Vita port reports "Vita" there and still gets the Xperia Play path - so
 *   that one says what the machine actually is.
 *
 *   GetTouchScreenCount - kept at 1 although an R36S has no touch screen. The
 *   menus are driven by touch events; the loader synthesises them from the
 *   D-pad. Reporting 0 would delete the module that receives them.
 */
#define SYSTEM_ANDROID_DELEGATE_PROPERTIES(X)                                  \
    X(IsBatteryStateAvailable,   "0")                                          \
    X(GetChipset,                "0")                                          \
    X(GetManufacturer,           "sony")                                       \
    X(GetDeviceModel,            "R36S")                                       \
    X(GetDeviceName,             "R800")                                       \
    X(GetPhoneNumber,            "0")                                          \
    X(GetDeviceUniqueId,         "-1")                                         \
    X(GetDisplayCount,           "1")                                          \
    X(GetLocationAvailable,      "true")                                       \
    X(GetApiLevel,               "19")                                         \
    X(GetPlatformRawName,        "Android")                                    \
    X(GetPlatformStdName,        "Android")                                    \
    X(GetPlatformVersion,        "4.4.4")                                      \
    X(GetPhysicalKeyboardCount,  "1")                                          \
    X(GetProcessorArchitecture,  "armeabi-v7a")                                \
    X(GetLanguage,               "en")                                         \
    X(GetLocale,                 "en")                                         \
    X(GetTotalRAM,               "-1")                                         \
    X(GetVirtualKeyboardCount,   "1")                                          \
    X(GetApplicationVersion,     "1")                                          \
    X(GetDeviceSubscriberID,     "0")                                          \
    X(GetFirmware,               "1")                                          \
    X(GetHardwareFloatingPointSupport, "true")

/*
 * The device counts, which the engine reads as ints and not as Strings.
 *
 * These nine used to live in the list above, registered "()Ljava/lang/String;"
 * like everything else, and the run log said what that cost once per name:
 *
 *     Class SystemAndroidDelegate does not have method GetTouchScreenCount()I.
 *
 * That line was dismissed as a harmless probe - the engine asking for an int
 * first and falling back to the String - because most of the other getters do
 * behave that way. These do not: no String lookup for them ever appears, so the
 * NULL jmethodID was the only answer the engine got, jni.cpp's null guard
 * returned 0, and EAMCore built no module for any piece of hardware whose count
 * came back zero.
 *
 * That is directly observable at runtime, which is how it was finally settled.
 * GameApplication::Init stores seven subsystem modules and the loader now dumps
 * them (see src/main.cpp); before this change:
 *
 *     +0x6c id=1000 Touchscreen         NULL
 *     +0x70 id=1200 Vibrator            NULL
 *     +0x74 id=700  VirtualKeyboard     ok
 *     +0x68 id=600  PhysicalKeyboard    ok
 *
 * The two keyboards are the tell: their counts are read as Strings, they were
 * found, and their modules exist. Touchscreen's count is read as an int, it was
 * not found, and its module does not - which is what ViewInteractive's
 * constructor dereferences at +0x6c, one frame into the real boot.
 *
 * Both spellings are registered for each name. A successful lookup prints
 * nothing, so "the engine never asks for these as Strings" is not something the
 * log can establish - only the failures are visible.
 */
#define SYSTEM_ANDROID_DELEGATE_COUNTS(X)                                      \
    X(GetAccelerometerCount, 1)                                                \
    X(GetCameraCount,        0)                                                \
    X(GetCompassCount,       0)                                                \
    X(GetGyroscopeCount,     1)                                                \
    X(GetMicrophoneCount,    1)                                                \
    X(GetTouchPadCount,      1)                                                \
    X(GetTouchScreenCount,   1)                                                \
    X(GetTrackBallCount,     0)                                                \
    X(GetVibratorCount,      0)

class SystemAndroidDelegate : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static void ctor(JNIEnv *env, jobject obj, jclass clazz);

    /*
     * The one property the engine reads as an int rather than as a String.
     *
     * Every other getter here is asked for twice - first as ()I, which misses,
     * then as ()Ljava/lang/String;, which is the real one - so the ()I lines in
     * the log are that probe and are harmless. This one is not: the log shows
     * GetApplicationVersionCode asked for as ()I and never as a String, so a
     * String entry could not answer it.
     *
     * 42 is what the Vita port returns for this exact binary.
     */
    static jint GetApplicationVersionCode(JNIEnv *env, jobject obj);

#define SYSTEM_ANDROID_DELEGATE_DECLARE(name, value)                           \
    static jstring name(JNIEnv *env, jobject obj);
    SYSTEM_ANDROID_DELEGATE_PROPERTIES(SYSTEM_ANDROID_DELEGATE_DECLARE)
#undef SYSTEM_ANDROID_DELEGATE_DECLARE

    /* Each count in both spellings - see the comment on the list above. */
#define SYSTEM_ANDROID_DELEGATE_DECLARE_COUNT(name, value)                     \
    static jint    name(JNIEnv *env, jobject obj);                             \
    static jstring name##AsString(JNIEnv *env, jobject obj);
    SYSTEM_ANDROID_DELEGATE_COUNTS(SYSTEM_ANDROID_DELEGATE_DECLARE_COUNT)
#undef SYSTEM_ANDROID_DELEGATE_DECLARE_COUNT
};
