#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/ea/EAMIO/StorageDirectory — where EA's I/O layer believes storage is.
 *
 * Java_com_ea_EAMIO_StorageDirectory_StartupNativeImpl resolves four methods on
 * this class and calls one of them immediately. Read out of the disassembly at
 * +0x367cd8:
 *
 *     bl EA::Jni::Delegate::Init("com/ea/EAMIO/StorageDirectory", ctx)
 *     bl GetStaticMethodId("GetInternalStorageDirectory", ...)       -> [ctx+20]
 *     bl GetStaticMethodId("GetPrimaryExternalStorageDirectory", ..) -> [ctx+24]
 *     bl GetStaticMethodId("GetPrimaryExternalStorageState", ...)    -> [ctx+28]
 *     bl GetStaticMethodId("GetDedicatedDirectory", ...)
 *     bl CallStaticObjectMethod(...)
 *     bl EA::IO::GetPathFromJString(result, [ctx+32])
 *
 * The last two lines are why a missing class here is not cosmetic: the result
 * of GetDedicatedDirectory is converted to a PathString8 and kept for the rest
 * of the run. With the class absent the jmethodID was NULL, jni.cpp's null
 * guard answered 0, and the stored path was a null pointer that the engine
 * then formatted into every name it built:
 *
 *     statx(AT_FDCWD, "(nil)/(nil)/GameSkeleton/", ...) = -1 ENOENT
 *
 * "(nil)" is printf rendering a NULL %s - the same signature the
 * GetAppDataDirectoryDelegate comment describes. Two frames later the engine's
 * System::IsAlive() went false and LoopLocked tail-called UserExit, which is a
 * deliberate shutdown, not a crash; the SIGSEGV that followed was
 * ModelCache::Shutdown tearing down an engine that had never finished starting.
 */
class StorageDirectory : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    static void ctor(JNIEnv *env, jobject obj, jclass clazz);

    static jstring GetInternalStorageDirectory(JNIEnv *env, jclass clazz);
    static jstring GetPrimaryExternalStorageDirectory(JNIEnv *env, jclass clazz);
    static jstring GetPrimaryExternalStorageState(JNIEnv *env, jclass clazz);
    static jstring GetDedicatedDirectory(JNIEnv *env, jclass clazz);
};
