#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/ea/games/Util — EA's downloadable-content gate.
 *
 * On Android this is a helper class in the launcher APK that answers whether
 * the ~300 MB asset payload has finished downloading and unpacking. Unlike
 * com/ea/EAIO/EAIO its methods are *not* native, so there is no implementation
 * inside libMassEffect.so to forward to: it has to be answered here.
 *
 * Our assets are already on disk before the process starts, so the answer is
 * unconditionally yes.
 *
 * ---------------------------------------------------------------------------
 * Why this file replaced jni/classes/eamobile_Query.*
 *
 * It arrived from the Dead Space port registered as "com/eamobile/Query", and
 * that classpath does not exist in this game - `grep -c -a -F` over
 * libMassEffect.so finds zero occurrences of it and exactly one of
 * "com/ea/games/Util", at .rodata 0x98e4c8, immediately followed by its method
 * names: getTotalMemory ()J, installWallpaper, isContentReady, getContentPath,
 * getPackage.
 *
 * A class registered under a name nobody asks for is indistinguishable from no
 * class at all, and it fails the same silent way: FindClass returns NULL,
 * GetStaticMethodID returns NULL without complaint because its own diagnostic
 * is guarded on a non-NULL class, and CallStaticBooleanMethod's null-guard
 * answers 0. The engine read that as "content not ready" and polled it once
 * per frame for all 600 frames without ever starting.
 */
class Util : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};
