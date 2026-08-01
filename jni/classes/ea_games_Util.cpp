#include <stdio.h>
#include <limits.h>

#include "platform.h"
#include "so_util.h"
#include "jni.h"
#include "jni_internals.h"
#include "trace.h"
#include "fix_path.h"
#include "ea_games_Util.h"

/*
 * Util.isContentReady - the reason the engine drew 600 empty frames.
 *
 * What was observed: the loader reached M5 with 600 NativeOnDrawFrame calls and
 * assets=0 textures=2 draws=0. The log had the matching pair repeated once per
 * frame, from the first frame to the last:
 *
 *     FindClass: no fake class registered for 'com/ea/games/Util'
 *     GetStaticMethodID: NULL class dereference ... 'isContentReady()Z'
 *
 * That is a poll, not a one-off probe. The engine asks every frame whether its
 * content has arrived and does nothing else until the answer is yes. It never
 * crashed - it just never started, which is why the frame counter alone cannot
 * tell a running game from a parked one.
 *
 * Answering JNI_TRUE unconditionally is correct here rather than a shortcut.
 * On Android this class lives in the launcher and reports on the DLC download;
 * a PortMaster install has the extracted asset tree in place before the process
 * starts, so there is no state to model and nothing that could later become
 * false. Forwarding to something in the .so - the way EAIO.Startup does - is
 * not available: the game exports no Java_com_ea_games_Util_* symbol, because
 * on Android this class is plain Java, not a native declaration.
 *
 * The Vita port answers the same way (falsojni_impl.c: isContentReady returns
 * JNI_TRUE) and runs this exact build.
 */
static jboolean Util_isContentReady(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    /* Logged once. The engine polls this every frame and a per-call trace would
     * bury the rest of the run in 600 identical lines - which is exactly how
     * the failure above stayed invisible for an iteration. */
    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("Util.isContentReady -> true (assets are on disk, no DLC fetch)");
    }

    return JNI_TRUE;
}

/*
 * How much memory the engine believes it has, in megabytes.
 *
 * ()J is read straight out of the binary: the literal sits at .rodata 0x98e4e0
 * with "()J" at 0x98e4f0, three words later.
 *
 * 256 is what the Vita port returns and it is also true of the target: the R36S
 * has 1 GB with the CFW and framebuffer already in it, and no port should try
 * to claim all of it. If the engine ever does scale a pool from this number,
 * the log line makes that visible in one run.
 */
static jlong Util_getTotalMemory(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    static bool announced = false;
    if (!announced) {
        announced = true;
        trace("Util.getTotalMemory -> 256 MB");
    }
    return 256;
}

/*
 * Where the content lives.
 *
 * The engine's VFS mounts what this returns, and a NULL does not fail loudly:
 * the mount root becomes the four characters "(nil)", every lookup underneath
 * it misses, and the result reads as an empty content tree rather than as a
 * method nobody implemented. That is the same shape of failure the
 * GetAppDataDirectoryDelegate comment records, and it is why this is worth
 * answering before any evidence that the engine asks for it - the evidence, if
 * it were missing, would be an asset counter stuck at zero with no line in the
 * log to explain it.
 *
 * The Vita port returns DATA_PATH_INT (vita-ref/loader/falsojni_impl.c), which
 * is its data directory with "assets/" appended. The equivalent here is the
 * assets directory of the tree the player pointed us at - the extracted
 * bundle's own root, one level above published/.
 */
static jstring Util_getContentPath(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    static String *cached = NULL;
    static char    path[PATH_MAX];
    if (!cached) {
        snprintf(path, sizeof(path), "%s/assets", io_game_dir());
        cached = new String(path);
        trace("Util.getContentPath -> '%s'", path);
    }
    return (jstring)cached;
}

/*
 * The Android package name, as this build's own manifest spells it.
 *
 * The Vita port answers "com.ea.games.meinfiltrator", the release it was built
 * against. The APK this port pins is the gamepad build and its manifest says
 * com.ea.games.meinfiltrator_gamepad; reporting our own is the answer that
 * stays true if the engine ever compares it against something read from the
 * bundle.
 */
static jstring Util_getPackage(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    static String *cached = NULL;
    if (!cached) {
        cached = new String("com.ea.games.meinfiltrator_gamepad");
        trace("Util.getPackage -> com.ea.games.meinfiltrator_gamepad");
    }
    return (jstring)cached;
}

const ManagedMethod UtilClassMethods[] = {
    ManagedMethod::RegisterStatic<&Util_getTotalMemory>(
        Util::clazz, "getTotalMemory", "()J"),
    ManagedMethod::RegisterStatic<&Util_isContentReady>(
        Util::clazz, "isContentReady", "()Z"),
    ManagedMethod::RegisterStatic<&Util_getContentPath>(
        Util::clazz, "getContentPath", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Util_getPackage>(
        Util::clazz, "getPackage", "()Ljava/lang/String;"),
    {NULL},
};

Class Util::clazz = {
    .classpath        = "com/ea/games/Util",
    .classname        = "Util",
    .managed_methods  = UtilClassMethods,
    .native_methods   = {NULL},
    .fields           = {NULL},
    .instance_size    = 0,
};

static const int registered = ClassRegistry::register_class(Util::clazz);
