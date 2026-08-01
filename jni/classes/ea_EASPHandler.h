#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/ea/easp/EASPHandler — the receiver the EASP layer keeps a global ref to.
 *
 * Java_com_ea_easp_EASPHandler_initJNI does not merely store its jobject
 * argument, which is what the Vita port's sentinel (0x42424242) assumes. Read
 * out of the disassembly at +0x64f8d0:
 *
 *     ldr pc, [r3, #84]    -> NewGlobalRef(env, obj)
 *     ldr pc, [r3, #124]   -> GetObjectClass(env, ref)
 *     ldr pc, [ip, #132]   -> GetMethodID(clazz, "setLogEnabled", "(Z)V")
 *
 * GetObjectClass reaches through the pointer to ask the object for its class,
 * so a made-up address faults there - inside this loader, with the engine's
 * return address in lr, which reads as a bug in our JNI when it is only a
 * receiver that was never an object. FalsoJNI gets away with the sentinel
 * because it dispatches on the value rather than on the object.
 *
 * setLogEnabled is the only method it looks up, and EASP's logging is off in
 * this port, so the body is empty on purpose.
 */
class EASPHandler : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static void setLogEnabled(JNIEnv *env, jobject obj, jboolean enabled);
};
