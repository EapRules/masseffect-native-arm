#include <stdio.h>
#include <limits.h>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "crash.h"
#include "fix_path.h"
#include "ea_EAMIO_StorageDirectory.h"

/*
 * All three directory getters name the writable directory, not the game tree.
 *
 * On a phone these are three different places - /data/data/<pkg> for internal,
 * /sdcard for primary external, /sdcard/Android/data/<pkg>/files for the
 * app-dedicated one - and the engine picks between them by what it is storing.
 * What they have in common is that every one of them is somewhere the app may
 * write, and that is the property the engine actually depends on.
 *
 * They used to answer with the game directory, which is the player's own copy
 * and is routinely read-only (the harness mounts it that way deliberately).
 * That is not a soft failure: EASP's SP::Tracking::TrackingImpl builds its
 * module data directory under this root, the mkdir fails, and the error path it
 * takes formats "%s module data directory %s failed to create" with 0x1 for the
 * second %s - so the process dies inside the complaint. It took hooking
 * VprintfCore to see that, because the crash reporter's stack scan pointed at
 * the wrong frame twice.
 *
 * Assets do not come through here. They resolve via appbundle:/ and via
 * com/ea/blast/GetAppDataDirectoryDelegate, both of which still name the game
 * tree, so pointing storage elsewhere costs nothing on the read side.
 *
 * Cached: DeleteLocalRef is a no-op in this port, so a fresh String per call
 * would leak one every time the engine builds a path.
 */
static jstring cached_storage_dir(const char *which)
{
    static String *cached = NULL;
    if (!cached)
        cached = new String(io_writable_dir());

    trace("StorageDirectory.%s -> '%s'", which, io_writable_dir());
    return (jstring)cached;
}

/*
 * The delegate is constructed before anything is called on it, so the class
 * needs an instance and a <init>()V - iface_NewObjectV refuses a class whose
 * instance_size is 0. There is no state to build.
 */
void StorageDirectory::ctor(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)env;
    (void)obj;
    (void)clazz;
}

jstring StorageDirectory::GetInternalStorageDirectory(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return cached_storage_dir("GetInternalStorageDirectory");
}

jstring StorageDirectory::GetPrimaryExternalStorageDirectory(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;

    /* One-shot: the engine calls this once per frame and the interesting fact
     * is which state machine does it, not that it happened. */
    static bool traced = false;
    if (!traced) {
        traced = true;
        crash_report_backtrace("GetPrimaryExternalStorageDirectory");
    }

    return cached_storage_dir("GetPrimaryExternalStorageDirectory");
}

/*
 * The app-dedicated directory - on Android /sdcard/Android/data/<pkg>/files,
 * the one directory the app owns outright and writes into.
 *
 * This pointed at <game>/assets/files for one iteration, because the APK ships
 * an empty assets/files/GameSkeleton/ and the engine builds that name off this
 * value:
 *
 *     statx(AT_FDCWD, "/game/GameSkeleton/", ...) = -1 ENOENT
 *     mkdir("/game/GameSkeleton", 0777)           = -1 EROFS
 *
 * Making the lookup resolve got the engine past that check, but it was the
 * wrong reading: GameSkeleton is not an asset the player supplies, it is a
 * directory the launcher creates, and the APK ships it empty for exactly that
 * reason. It belongs with the rest of the writable data, where the engine can
 * create it itself - Directory::EnsureExists() does precisely that once the
 * root is writable.
 */
jstring StorageDirectory::GetDedicatedDirectory(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return cached_storage_dir("GetDedicatedDirectory");
}

/*
 * android.os.Environment.MEDIA_MOUNTED, spelled exactly as the platform does.
 *
 * The engine compares this against the literal rather than interpreting it, and
 * every other value in that enum ("removed", "unmounted", "shared", ...) means
 * "there is no external storage" - which on Android is the branch that refuses
 * to load content.
 */
jstring StorageDirectory::GetPrimaryExternalStorageState(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    static String *cached = NULL;
    if (!cached) {
        cached = new String("mounted");
        trace("StorageDirectory.GetPrimaryExternalStorageState -> mounted");
    }
    return (jstring)cached;
}

const ManagedMethod StorageDirectoryMethods[] = {
    ManagedMethod::RegisterNonVirtual<&StorageDirectory::ctor>(
        StorageDirectory::clazz, "<init>", "()V"),
    ManagedMethod::RegisterStatic<&StorageDirectory::GetInternalStorageDirectory>(
        StorageDirectory::clazz,
        "GetInternalStorageDirectory", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&StorageDirectory::GetPrimaryExternalStorageDirectory>(
        StorageDirectory::clazz,
        "GetPrimaryExternalStorageDirectory", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&StorageDirectory::GetPrimaryExternalStorageState>(
        StorageDirectory::clazz,
        "GetPrimaryExternalStorageState", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&StorageDirectory::GetDedicatedDirectory>(
        StorageDirectory::clazz,
        "GetDedicatedDirectory", "()Ljava/lang/String;"),
    {NULL},
};

Class StorageDirectory::clazz = {
    .classpath        = "com/ea/EAMIO/StorageDirectory",
    .classname        = "StorageDirectory",
    .managed_methods  = StorageDirectoryMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = sizeof(StorageDirectory),
};

static const int registered =
    ClassRegistry::register_class(StorageDirectory::clazz);
