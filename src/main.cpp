/*
 * Mass Effect Infiltrator loader — entry point.
 *
 * This file used to be a NativeActivity driver: open an APK, map the
 * game's .so out of it, build an ANativeActivity and hand it to
 * ANativeActivity_onCreate, then watch the engine run its own frame loop.
 *
 * None of that applies here, and the run log said so in one line:
 *
 *     FATAL: cannot open APK '/game': Operation not supported
 *
 * because the harness passes a *directory*. That error is only the surface of
 * it. Mass Effect Infiltrator is not a NativeActivity game at all:
 *
 *   - it has no DT_NEEDED on libandroid.so and exports no
 *     ANativeActivity_onCreate; its DT_NEEDED list is exactly
 *     libc / libstdc++ / libm / liblog / libGLESv1_CM
 *   - it exports JNI_OnLoad plus 68 Java_* entry points and expects a Java
 *     layer to call them, frame by frame. That layer is this file.
 *   - EA shipped it with the assets outside the package; what circulates is
 *     the extracted tree, so there is no zip to open in the first place.
 *
 * So the driver is inverted with respect to the other two ports: *we* own the
 * frame loop and call into the engine, instead of feeding lifecycle commands
 * to an engine that owns its own.
 *
 * The call order below is not deduced from the symbol names - it is copied
 * from masseffect_main() in the Vita port (vita-ref/loader/main.c, MIT), which
 * runs this exact build. Two of its choices are worth naming because the
 * obvious alternative is wrong:
 *
 *   - the entry point is NativeOnCreate. The .so also exports runEntryPoint,
 *     which reads like the real one; the working port never calls it.
 *   - NativeOnSurfaceChanged is never called either. The renderer takes its
 *     resolution once, at surface-created time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "khronos/gles2.h"

#include "jni.h"
#include "classes/android_assets.h"
#include "classes/ea_EASPHandler.h"
#include "classes/media_AudioTrack.h"

#include "app_exit.h"
#include "crash.h"
#include "cursor_draw.h"
#include "emulator_control.h"
#include "fb_probe.h"
#include "fix_path.h"
#include "input_bridge.h"
#include "patch.h"
#include "trace.h"

extern "C" long android_io_assets_opened(void);
extern "C" long android_gl_textures_uploaded(void);
extern "C" long android_gl_draw_calls(void);
extern "C" long android_gl_decoded_mib(void);

/*
 * The library, and where it lives inside the game tree.
 *
 * "armeabi", not "armeabi-v7a": this is a 2011 Xperia Play build, from before
 * the v7a split was routine, and it is genuinely ARMv5TE+VFPv2 code. The
 * loader's own search path derives the ABI directory from the host
 * architecture and would look under armeabi-v7a, so the game directory is
 * pointed at with the ABI folder already included - see so_set_options()
 * below.
 */
static const char *kNativeLib    = "libMassEffect.so";
static const char *kNativeLibDir = "lib/armeabi";

/* The R36S panel. Fixed: the engine asks for the surface size once and the
 * Vita port never calls NativeOnSurfaceChanged, so there is no resize path to
 * exercise. */
static const int kWidth  = 640;
static const int kHeight = 480;

/* Defined in symtab_glprobe.cpp: maps the engine's fixed logical panel onto the
 * device's real drawable (letterbox/stretch), identity on a matching 640x480. */
extern "C" void viewport_scale_init(int phys_w, int phys_h);

/*
 * Walk the module's dynamic symbol table and name every import that nothing
 * answers.
 *
 * The loader itself does not do this: it points unresolved jump slots at a
 * stub that aborts the first time the game calls one, which surfaces a single
 * missing symbol per run, from inside a crash, with no stack. Auditing up
 * front lists all of them at once.
 *
 * Undefined *weak* symbols are not failures: resolving them to zero is what a
 * real dynamic linker does, and the game tests them for null before use.
 */
static int report_unresolved_symbols(so_module *mod)
{
    int missing = 0;

    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf_Sym *sym = &mod->dynsym[i];
        if (sym->st_shndx != SHN_UNDEF)
            continue;

        const char *name = mod->dynstr + sym->st_name;
        if (!name || !*name)
            continue;

        if (so_resolve_link(mod, name))
            continue;

        if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
            trace("weak import left null: %s", name);
            continue;
        }

        fprintf(stderr, "unresolved symbol: %s\n", name);
        missing++;
    }

    fflush(stderr);
    return missing;
}

/*
 * The loader's post-relocation hook (see so_util.h).
 *
 * "The ELF mapped and relocated" and "every import found an implementation"
 * are two different facts and are reported as two, in that order. Collapsing
 * them - printing the milestone line only on full success - is what the first
 * run of this driver did, and the log it produced said "module never loaded"
 * about a module that had in fact mapped, relocated and then died inside a
 * static constructor calling one of 205 unbound imports. Naming them is the
 * whole point of being here rather than after so_initialize().
 */
/*
 * The loaded game, for the fake JNI classes that have to call back into it.
 *
 * Several of the Java classes this port fakes are not really Java at all: their
 * methods are declared native on Android, so calling them through JNI lands
 * back in this same .so. com/ea/EAIO/EAIO.Startup is the first one the engine
 * reaches. Those forwarders need the module, and they run long after main()
 * has handed control to the game, so a lookup is cheaper than threading a
 * pointer through the class registry.
 *
 * Matching by DT_SONAME - which is what jni_resolve_native() does - is not an
 * option here: this build has none, and the loader logs it as
 * "Linking <no DT_SONAME>".
 */
static so_module *g_module = NULL;

so_module *masseffect_module(void) { return g_module; }

extern "C" int so_after_relocate(so_module *mod)
{
    g_module = mod;
    trace("module loaded");

    /*
     * Arm the fault handler here, not after so_load_module() returns.
     *
     * so_initialize() runs the game's 209 static constructors immediately
     * after this hook, and jni_resolve_native() runs right after those. A
     * fault in either used to be reported by qemu as a bare "uncaught target
     * signal 11" with no pc, no offset and no stack, because the handler was
     * still not installed - the whole first half of the boot was a blind spot
     * exactly where the engine's own code starts running.
     */
    crash_report_init(mod, kNativeLib);

    int missing = report_unresolved_symbols(mod);
    if (missing == 0) {
        /*
         * Rewrite the engine's asset I/O dispatch before any of its code runs.
         * This is the window the Vita port uses too - relocated, not yet
         * initialised - and it is the only one: so_initialize() below starts
         * the static constructors, and the .text is mprotect'd back to
         * read-only once the load finishes.
         */
        so_patch_binary(mod);

        return 0;
    }

    fatal("%d import(s) of %s have no implementation (listed above).\n"
          "       Running the game now would fault on the first call to any\n"
          "       of them, from inside a static constructor, with nothing but\n"
          "       an address to go on.",
          missing, mod->soname ? mod->soname : "the module");
    return 1;
}

/*
 * Make the back buffer's contents undefined after a swap, if the driver was
 * preserving them.
 *
 * The software cursor (android/cursor_draw.cpp) is painted straight into the
 * default framebuffer just before the swap, with glClear plus a scissor, and
 * nothing puts back what was underneath. That is only invisible while the game
 * repaints the whole screen every frame. It does: with MASSEFFECT_CURSOR_DIAG=1
 * the probe reports `glClear mask=0x4000 colour=yes scissor=off box=0,0 640x480`
 * on every frame of the title screen.
 *
 * So a trail of cursors means the frame that comes back from the swap chain is
 * not blank - the driver handed back a buffer with an older frame still in it,
 * and the game's clear happens per frame, not per buffer. EGL calls that
 * EGL_BUFFER_PRESERVED, and it is a per-surface attribute with no portable
 * default: Mesa hands back a destroyed buffer, several Mali drivers preserve.
 * That is exactly the shape of "reproduces on the console, never under the
 * emulator".
 *
 * This queries the real value and only changes it when it is PRESERVED, so on a
 * driver that already destroys (the emulator) it does nothing at all. Both the
 * before and after values are traced, which is the evidence for whether this
 * was the cause - if the console reports DESTROYED to begin with, the theory is
 * wrong and the log says so without needing another trip.
 *
 * MASSEFFECT_CURSOR_RESTORE=0 leaves the surface alone. Worth keeping, because
 * a preserved back buffer is a legitimate thing for an engine to rely on: if
 * this build ever drew incrementally, destroying the buffer would break it, and
 * that would show up as a flickering or half-drawn frame rather than a trail.
 */
static void configure_swap_behavior(void)
{
    /* Two spellings of the same rollback: MASSEFFECT_EGL_PRESERVE=1 reads
     * better for "leave the driver's preservation alone", CURSOR_RESTORE=0
     * matches the other cursor toggles. Either one backs this out. */
    const char *off = getenv("MASSEFFECT_CURSOR_RESTORE");
    const char *keep = getenv("MASSEFFECT_EGL_PRESERVE");
    if ((off && *off && *off == '0') || (keep && *keep && *keep != '0')) {
        trace("swap behavior: left as the driver set it "
              "(MASSEFFECT_CURSOR_RESTORE=0)");
        return;
    }

    /* Spelled out rather than taken from glad_egl.h, which is not included
     * here; the names below are prefixed to avoid colliding with it. */
    enum {
        kEglDraw             = 0x3059,
        kEglSwapBehavior     = 0x3093,
        kEglBufferPreserved  = 0x3094,
        kEglBufferDestroyed  = 0x3095,
    };
    using GetCurrentDisplay = void *(*)(void);
    using GetCurrentSurface = void *(*)(int);
    using QuerySurface = unsigned int (*)(void *, void *, int, int *);
    using SurfaceAttrib = unsigned int (*)(void *, void *, int, int);

    GetCurrentDisplay get_display =
        (GetCurrentDisplay)SDL_GL_GetProcAddress("eglGetCurrentDisplay");
    GetCurrentSurface get_surface =
        (GetCurrentSurface)SDL_GL_GetProcAddress("eglGetCurrentSurface");
    QuerySurface query =
        (QuerySurface)SDL_GL_GetProcAddress("eglQuerySurface");
    SurfaceAttrib attrib =
        (SurfaceAttrib)SDL_GL_GetProcAddress("eglSurfaceAttrib");

    if (!get_display || !get_surface || !query || !attrib) {
        trace("swap behavior: EGL entry points unavailable; leaving the "
              "surface as-is");
        return;
    }

    void *display = get_display();
    void *surface = get_surface(kEglDraw);
    if (!display || !surface) {
        trace("swap behavior: no current EGL surface; leaving it as-is");
        return;
    }

    int before = 0;
    if (!query(display, surface, kEglSwapBehavior, &before)) {
        trace("swap behavior: eglQuerySurface failed; leaving it as-is");
        return;
    }

    const char *name = before == kEglBufferPreserved ? "PRESERVED"
                     : before == kEglBufferDestroyed ? "DESTROYED"
                     : "unknown";
    if (before != kEglBufferPreserved) {
        trace("swap behavior: %s (0x%04x) - nothing to change; the cursor "
              "trail is not this", name, (unsigned int)before);
        return;
    }

    unsigned int ok = attrib(display, surface, kEglSwapBehavior,
                             kEglBufferDestroyed);
    int after = 0;
    query(display, surface, kEglSwapBehavior, &after);
    trace("swap behavior: was PRESERVED, requested DESTROYED -> %s (0x%04x)%s",
          after == kEglBufferDestroyed ? "DESTROYED" :
          after == kEglBufferPreserved ? "still PRESERVED" : "unknown",
          (unsigned int)after,
          ok ? "" : " (eglSurfaceAttrib refused it)");
}

int main(int argc, char **argv)
{
    /*
     * Unbuffered from the first line, because the log is the only diagnostic
     * that leaves the console and a crash must not take it with it.
     *
     * Doing it here rather than with stdbuf is the difference between working
     * and not: stdbuf injects a 64-bit libstdbuf.so via LD_PRELOAD, which a
     * 32-bit process like this one cannot load at all.
     */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <masseffect-directory>\n"
                "\n"
                "The directory is your own copy of the game - the extracted\n"
                "tree with lib/armeabi/ and assets/published/ in it. It is\n"
                "never bundled with this port.\n",
                argv[0]);
        return 2;
    }

    const char *game_dir = argv[1];

    /* Before anything of the game's runs: the libc path thunks translate
     * "appbundle:/..." against this and the engine opens its first file from
     * inside a static initialiser. */
    io_set_game_dir(game_dir);

    char lib_dir[PATH_MAX];
    char lib_path[PATH_MAX];
    snprintf(lib_dir,  sizeof(lib_dir),  "%s/%s", game_dir, kNativeLibDir);
    snprintf(lib_path, sizeof(lib_path), "%s/%s", lib_dir, kNativeLib);

    struct stat st;
    if (stat(lib_path, &st) != 0) {
        fatal("'%s' does not exist.\n"
              "       This does not look like an extracted Mass Effect Infiltrator tree.",
              lib_path);
        return 1;
    }
    trace("native library found: %s (%lld bytes)", lib_path, (long long)st.st_size);

    /*
     * Bring GL up before the module is linked, not after: the GL import table
     * is filled in by asking the driver for each entry point, and there is no
     * driver to ask until a context is current. Relocating first would bind
     * every gl* call to null.
     *
     * A failure here is deliberately *not* fatal. The point of separating the
     * milestones is that "does every import resolve?" and "is there a usable
     * GLES context?" are different questions with different answers; aborting
     * on the second one would make the first unanswerable from the log. The
     * run continues and the missing context shows up later, at the milestone
     * that is actually about it.
     */
    SDL_Window   *window = NULL;
    SDL_GLContext gl     = NULL;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        trace("SDL_Init(video+gamecontroller) failed: %s", SDL_GetError());
    } else {
        /*
         * Mass Effect Infiltrator is the other half of the IronMonkey family:
         * pure GLES2, 177 shader-pipeline imports and no fixed function at
         * all. So the context request mirrors the sibling port's - ask for
         * ES 2.0 first, which is what the Mali blob on the console and Mesa
         * under the harness both speak natively. The sibling port's ES1-vs-desktop
         * dance does not apply here and was removed; see that port for why it
         * existed.
         */
        /* See configure_swap_behavior(). */
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

        window = SDL_CreateWindow("Mass Effect Infiltrator",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  kWidth, kHeight, SDL_WINDOW_OPENGL);
        if (!window) {
            trace("SDL_CreateWindow failed: %s", SDL_GetError());
        } else {
            gl = SDL_GL_CreateContext(window);
            if (!gl) {
                trace("no native GLES 2.0 context (%s) - falling back to a desktop "
                      "core-ish 2.1 profile, whose GLSL 1.20 is close enough "
                      "for Mesa to accept ES shaders via ARB_ES2_compatibility",
                      SDL_GetError());

                /* The window was created for an ES pixel format; a profile
                 * change needs a fresh one or SDL keeps the old config. */
                SDL_DestroyWindow(window);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                    SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

                window = SDL_CreateWindow("Mass Effect Infiltrator",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          kWidth, kHeight, SDL_WINDOW_OPENGL);
                if (!window)
                    trace("SDL_CreateWindow (compat) failed: %s", SDL_GetError());
                else if (!(gl = SDL_GL_CreateContext(window)))
                    trace("no fixed-function context at all: %s", SDL_GetError());
            }
        }
    }

    if (gl) {
        load_gles1_funcs();
        /* The sibling port never called this - fixed function, zero shader imports.
         * Mass Effect Infiltrator is the opposite: its 177 GL imports are all
         * ES2 pipeline calls, and every one resolves from this table. */
        load_gles2_funcs();

        configure_swap_behavior();

        int window_w = 0, window_h = 0, drawable_w = 0, drawable_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
        trace("window geometry: logical=%dx%d drawable=%dx%d",
              window_w, window_h, drawable_w, drawable_h);

        /* Remap the engine's fixed 640x480 onto this panel (identity on the
         * R36S, letterbox/stretch on a wider one like the RG34XXSP). */
        viewport_scale_init(drawable_w, drawable_h);

        const GLubyte *(*get_string)(GLenum) =
            (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
        if (get_string) {
            const char *ver = (const char *)get_string(0x1F02 /* GL_VERSION  */);
            const char *rnd = (const char *)get_string(0x1F01 /* GL_RENDERER */);
            trace("GL_VERSION=%s | GL_RENDERER=%s", ver ? ver : "?", rnd ? rnd : "?");
        }
    }

    /*
     * The fake JavaVM has to exist before the module is linked: the engine's
     * static initialisers run during so_load_module() and some of them reach
     * for the VM.
     */
    JavaVM *vm  = NULL;
    JNIEnv *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK || !vm || !env) {
        fatal("could not create the JNI environment.");
        return 1;
    }

    /*
     * Map, relocate and link the module.
     *
     * so_load_module() takes a bare soname and searches "<alt>/<abi>/<name>"
     * and "lib/<abi>/<name>", with <abi> derived from the host - armeabi-v7a
     * here. This game ships under lib/armeabi, so neither pattern matches and
     * the alternative search path is set to the ABI directory itself, which
     * makes the loader's own second attempt ("lib/armeabi-v7a/...") the one
     * that misses and the direct one the one that hits.
     *
     * The zip argument is NULL and stays NULL: every DT_NEEDED this module
     * declares is answered by this loader (see so_builtin_libs in symtab.cpp),
     * so nothing is ever looked for inside an archive.
     *
     * The VM argument is NULL on purpose, and it is not an oversight:
     * so_load_module() calls JNI_OnLoad itself when a module exports one, and
     * for this game that would run it in the wrong place. The working boot
     * sequence puts JNI_OnLoad first on the game thread, ahead of
     * NativeOnCreate - passing NULL here leaves that call to us, below.
     */
    so_set_options(NULL, lib_dir);

    so_module *mod = so_load_module(kNativeLib, NULL, NULL);
    if (!mod) {
        /* so_after_relocate() already said which imports were missing, if
         * that is what stopped it; anything else is a mapping failure. */
        fatal("could not load '%s' from '%s'.", kNativeLib, lib_dir);
        fflush(NULL);
        _exit(1);
    }

    trace("so_load_module returned (text_base=%p size=%zu)",
          (void *)mod->text_base, (size_t)mod->text_size);

    /* ---------------------------------------------------------------- *
     * The boot sequence, in the order the Vita port uses.
     *
     * It runs on this thread rather than on a dedicated one. The Vita port
     * spawns a pthread purely to get a 1 MB stack, which is larger than its
     * default; on Linux the main thread already has 8 MB, so the thread would
     * buy nothing and only add a place for a crash to lose its backtrace.
     * ---------------------------------------------------------------- */
    auto JNI_OnLoad =
        (int (*)(JavaVM *, void *))so_symbol(mod, "JNI_OnLoad");
    auto NativeOnCreate =
        (void (*)(void))so_symbol(mod, "Java_com_ea_blast_MainActivity_NativeOnCreate");
    auto NativeOnSurfaceCreated =
        (void (*)(void))so_symbol(mod, "Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated");
    auto NativeOnVisibilityChanged =
        (void (*)(JNIEnv *, void *, int, int))so_symbol(mod, "Java_com_ea_blast_KeyboardAndroid_NativeOnVisibilityChanged");
    auto NativeOnDrawFrame =
        (void (*)(void))so_symbol(mod, "Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame");

    if (!JNI_OnLoad || !NativeOnCreate || !NativeOnSurfaceCreated || !NativeOnDrawFrame) {
        fatal("%s is missing one of the entry points the port drives:\n"
              "       JNI_OnLoad=%p NativeOnCreate=%p NativeOnSurfaceCreated=%p\n"
              "       NativeOnDrawFrame=%p",
              kNativeLib, (void *)JNI_OnLoad, (void *)NativeOnCreate,
              (void *)NativeOnSurfaceCreated, (void *)NativeOnDrawFrame);
        fflush(NULL);
        _exit(1);
    }

    trace("-> JNI_OnLoad at +0x%08lx",
          (unsigned long)((uintptr_t)JNI_OnLoad - mod->text_base));
    JNI_OnLoad(vm, NULL);
    trace("JNI_OnLoad returned");

    /*
     * The four subsystem startups, between JNI_OnLoad and NativeOnCreate.
     *
     * Leaving these out is not "skipping optional initialisation": JNI_OnLoad
     * itself is 28 bytes long and does exactly one thing,
     *
     *     JNI_OnLoad:  ldr r3, [pc, r3]   ; a GOT slot -> the .bss word at +0xaa111c
     *                  str r0, [r3]       ; = the JavaVM
     *                  mov r0, #0x10004   ; JNI_VERSION_1_4
     *
     * and that word is not the one the engine's I/O layer reads.
     * EA::IO::AutoJNIEnv - the object every FileStream::Open builds before it
     * touches a file - loads its VM from a *different* .bss word, +0xb19078,
     * and calls GetEnv through its vtable:
     *
     *     AutoJNIEnv::AutoJNIEnv:  ldr r3, [r5, #4]   ; the VM at +0xb19078
     *                              ldr r3, [r3]       ; <- SIGSEGV, r3 == 0
     *                              ldr pc, [r3, #24]  ; JavaVM::GetEnv
     *
     * Nothing sets +0xb19078 except EA::IO::AssetManagerJNI::Startup, which is
     * the body of Java_com_ea_EAIO_EAIO_StartupNativeImpl and which begins by
     * calling env->GetJavaVM() into it. So the port either makes this call or
     * the first asset open faults on a null VM, one frame in - and the report
     * points at the engine's I/O code, which is not where the mistake is.
     *
     * The set and the order are the Vita port's game_thread()
     * (vita-ref/loader/main.c): EASP, EAThread, EAIO, EAMIO, then onCreate.
     */
    {
        auto EASPHandler_initJNI = (void (*)(JNIEnv *, jobject))
            so_symbol(mod, "Java_com_ea_easp_EASPHandler_initJNI");
        auto EAThread_Init = (void (*)(JNIEnv *))
            so_symbol(mod, "Java_com_ea_EAThread_EAThread_Init");
        auto EAIO_Startup = (void (*)(JNIEnv *, jobject, jobject, jstring, jstring))
            so_symbol(mod, "Java_com_ea_EAIO_EAIO_StartupNativeImpl");
        auto EAMIO_Startup = (void (*)(JNIEnv *))
            so_symbol(mod, "Java_com_ea_EAMIO_StorageDirectory_StartupNativeImpl");

        /* A real EASPHandler, not the Vita port's 0x42424242: initJNI calls
         * GetObjectClass on this argument, which dereferences it. See
         * jni/classes/ea_EASPHandler.h. */
        if (EASPHandler_initJNI) {
            EASPHandler_initJNI(env, (jobject)new EASPHandler());
            trace("EASPHandler_initJNI returned");
        } else {
            trace("no EASPHandler_initJNI export");
        }

        if (EAThread_Init) {
            EAThread_Init(env);
            trace("EAThread_Init returned");
        } else {
            trace("no EAThread_Init export");
        }

        if (EAIO_Startup) {
            /*
             * Startup reads three of its arguments and keeps them:
             *
             *   assets      stored as-is, and later used as the receiver of
             *               open()/openFd()/list(). The Vita passes a sentinel
             *               because its FalsoJNI dispatches on value; this
             *               port's jni.cpp resolves methods through the
             *               receiver's class, so a sentinel would fault the
             *               first time the engine read an asset through it.
             *               A real AssetManager instance goes in instead - the
             *               same object MainActivity.getAssets() hands back.
             *
             *   filesDir    strncpy'd into two 512-byte buffers, the second of
             *               which gets "/tmp" appended: the engine's scratch
             *               directory.
             *
             *   extStorage  strncpy'd into a third.
             *
             * All three are the game root, which is what the Vita port passes
             * (DATA_PATH for both strings) and what fix_path() already knows
             * how to translate.
             */
            jstring data_path = env->NewStringUTF(io_game_dir());
            jobject assets    = (jobject)new AssetManager();

            EAIO_Startup(env, (jobject)0x42424242, assets, data_path, data_path);
            trace("EAIO_StartupNativeImpl returned (root=%s)", io_game_dir());
        } else {
            trace("no EAIO_StartupNativeImpl export");
        }

        if (EAMIO_Startup) {
            EAMIO_Startup(env);
            trace("EAMIO_StorageDirectory_StartupNativeImpl returned");
        } else {
            trace("no EAMIO StorageDirectory startup export");
        }
    }

    /*
     * Bring the audio core up, between JNI_OnLoad and NativeOnCreate.
     *
     * This was left out at first, on the reasoning that audio is polish and the
     * port could reach a frame without it. That was wrong, and the run log said
     * so once the crash reports became readable: the engine opens
     * published/sounds/soundBase.sb and then faults on `ldr r1, [r3]` with
     * r3 = 0 - an object whose vtable word is zero, i.e. one that was never
     * constructed. Init is what constructs it. Skipping it does not disable
     * audio, it leaves the engine holding a half-built object that it has no
     * reason to check before using.
     *
     * The arguments come from the Vita port, which runs this same build:
     * a 1 MiB buffer, stereo, 44100 Hz. Two of its choices are not copied.
     *
     * It passes 0x42424242 for `this` and 0x69696969 for the AudioTrack -
     * sentinels its fake JNI recognises by value. Ours resolves methods through
     * a class registry and calls _getClass() on the receiver, so a sentinel
     * would fault the moment the engine called anything on it. A real instance
     * of the AudioTrack fake class goes in instead, which is also what makes
     * SDL produce sound rather than merely not crash.
     *
     * `this` stays a sentinel: the method is static in Java and the engine
     * never dereferences it, so a made-up object would be less honest about
     * that than a value nobody can mistake for one.
     */
    {
        auto EAAudioCore_Init = (void (*)(JNIEnv *, void *, jobject, int, int, int))
            so_symbol(mod, "Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init");

        if (!EAAudioCore_Init) {
            trace("no AndroidEAAudioCore_Init export - audio core left down");
        } else {
            /* streamType 3 = STREAM_MUSIC, channelConfig 3 = CHANNEL_STEREO,
             * audioFormat 2 = ENCODING_PCM_16BIT, mode 0 = MODE_STREAM. */
            jobject track = (jobject)new AudioTrack(3, 44100, 3,
                                                    ENCODING_PCM_16BIT,
                                                    1024 * 1024, MODE_STREAM);
            EAAudioCore_Init(env, (void *)0x42424242, track,
                             1024 * 1024, 2, 44100);
            trace("EAAudioCore init done (1 MiB buffer, stereo, 44100 Hz)");
        }
    }

    NativeOnCreate();
    trace("NativeOnCreate returned");

    NativeOnSurfaceCreated();
    trace("surface created");

    /* 0x42424242 is the Vita port's placeholder jobject: the engine stores the
     * pointer and never dereferences it, and a recognisable value makes it
     * obvious in a fault address if that assumption is ever wrong. */
    if (NativeOnVisibilityChanged) {
        NativeOnVisibilityChanged(env, (void *)0x42424242, 600, 1);
        trace("visibility changed");
    }

    android_input_init(mod, env, kWidth, kHeight);
    emulator_control_init();

    /*
     * The orientation handshake the engine cannot complete on its own.
     *
     * GameApplication::OnUpdate runs a boot state machine gated on this one
     * .bss word. At +0x40ba60 it reads the word and, if it is not zero, goes
     * back to waiting instead of calling im::GetEAMTRunLoop() - the run loop
     * that actually starts the game:
     *
     *     ldrsb r5, [r3]           ; r3 = text_base + 0xaabd90
     *     cmp   r5, #0
     *     bne   40b97c             ; -> keep waiting
     *     bl    im::GetEAMTRunLoop()
     *
     * Every pc-relative literal in that function resolves to this same word.
     * On Android the activity sets it when it requests an orientation change
     * and the Java side clears it once the surface has come back at the new
     * geometry; nothing in this port does that, so the engine waits forever -
     * which is why it ran its whole frame budget without drawing anything.
     *
     * Clearing it from the frame loop is what the Vita port does
     * (vita-ref/loader/main.c: `if (*waitForOrientation == 1) {
     * *waitForOrientation = 0; }`), and it names 0x00aabd90 for this exact
     * 1.0.58 build. This panel does not rotate, so acknowledging immediately is
     * not a shortcut: the rotation the engine waits on has already happened by
     * definition.
     */
    volatile int32_t *wait_for_orientation =
        (volatile int32_t *)(mod->text_base + 0x00aabd90);
    trace("orientation handshake word at +0x00aabd90 (currently %d)",
          (int)*wait_for_orientation);

    /*
     * The engine's subsystem-module table, dumped once it exists.
     *
     * GameApplication::Init fills eight slots by calling ISystem::GetModule()
     * for a fixed list of ids, and stores every answer without checking it
     * except Display's. A NULL in any of the others is a fault much later, in
     * whatever code first reaches for that subsystem, with nothing in the
     * report tying it back to a module that was never built.
     *
     * The ids come from the Register*Module() functions
     * (EA::Blast::RegisterTouchscreenModule and friends, each passing its id to
     * the ModuleRegistryEntry constructor), so the mapping below is
     * transcribed, not inferred from the order of the stores - and that
     * distinction cost an iteration: the `str` at the top of each block in Init
     * writes the *previous* call's result, so reading them in program order
     * pairs every id with the wrong slot and shifts the whole table by one.
     *
     * The app pointer lives at +0xaabdb4, next to the orientation word.
     */
    struct ModuleSlot { unsigned off; int id; const char *name; };
    static const ModuleSlot kModuleSlots[] = {
        {0x60,  100, "Accelerometer"},
        {0x64,  300, "Device"},
        {0x7c,  400, "Display"},
        {0x70, 1200, "Vibrator"},
        {0x74,  700, "VirtualKeyboard"},
        {0x68,  600, "PhysicalKeyboard"},
        {0x6c, 1000, "Touchscreen"},
        {0x78, 1700, "WebBrowserLauncher"},
    };
    void *const *game_application_slot =
        (void *const *)(mod->text_base + 0x00aabdb4);


    /*
     * A bounded run. MASSEFFECT_FRAME_LIMIT stops the process after that many
     * frames so an automated run terminates on a fact rather than on a
     * stopwatch; unset (the normal case for a player) means run forever.
     */
    const char *limit_env   = getenv("MASSEFFECT_FRAME_LIMIT");
    const long  frame_limit = limit_env ? atol(limit_env) : 0;

    long frames = 0;
    while (frame_limit == 0 || frames < frame_limit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (!android_input_event(&ev))
                goto done;
        }

        /*
         * The game's own exit request, checked next to the event drain rather
         * than trusted to it: android_app_request_exit() pushes SDL_QUIT, but
         * a full queue drops it, and a dropped exit is the freeze all over
         * again. Read before the draw, so the frame after finish() is never
         * issued against a world the engine has already released.
         */
        if (android_app_exit_requested())
            goto done;

        if (!emulator_control_tick(frames))
            goto done;
        android_input_tick();
        android_input_autopilot_tick(frames);

        /* The first frame gets its own line. "surface created" followed by
         * nothing was ambiguous for several sessions: it could mean the fault
         * was in NativeOnVisibilityChanged, in the engine's first frame, or in
         * our own loop before reaching it. Three possibilities, one line to
         * tell them apart. */
        /* Entering *and* returning, for the first few. "frames=1" as the last
         * line cannot distinguish "the second frame never started" from "the
         * second frame never came back", and those are different bugs. */
        if (frames < 5)
            trace("-> NativeOnDrawFrame #%ld", frames + 1);

        NativeOnDrawFrame();
        frames++;

        if (frames <= 5)
            trace("<- NativeOnDrawFrame #%ld returned", frames);

        if (window) {
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(window, &w, &h);
            android_fb_probe(frames, w, h);
            android_input_autopilot_sample(frames);
            android_cursor_draw(w, h);
            emulator_control_after_draw(frames, w, h);
            SDL_GL_SwapWindow(window);
        }

        /* Acknowledge the orientation request, after the swap - which is where
         * the Vita port does it, and the order is not incidental: the
         * acknowledgement means "the surface has come back at the new
         * geometry", which is only true once the frame has been presented. */
        if (*wait_for_orientation == 1) {
            *wait_for_orientation = 0;
            if (frames <= 5)
                trace("orientation handshake acknowledged at frame %ld", frames);
        }

        /* Once, as soon as GameApplication exists. */
        static bool modules_dumped = false;
        if (!modules_dumped && *game_application_slot) {
            const char *app = (const char *)*game_application_slot;
            modules_dumped = true;
            trace("GameApplication at %p, subsystem modules:", (const void *)app);
            for (const ModuleSlot &s : kModuleSlots) {
                void *m = *(void *const *)(app + s.off);
                trace("  +0x%02x id=%-4d %-20s %s", s.off, s.id, s.name,
                      m ? "ok" : "NULL <- nothing registered this id");
            }
        }

        /* One line per frame would drown the log; the harness reads the last
         * one, so the cadence only has to be fine enough to be current. */
        /* Dense at the start, sparse after. The harness reads the last
         * frames= line, so the cadence only has to be current - but a run that
         * dies at frame 3 used to print nothing at all, which read as "never
         * drew a frame" when the truth was "drew three". */
        if (frames <= 5 || frames % 10 == 0)
            trace("frames=%ld", frames);

        /*
         * Whether the engine is still doing anything.
         *
         * "It froze" and "it is drawing a screen that does not change" look the
         * same from outside and are different bugs, and a frame counter cannot
         * tell them apart: this loop keeps calling NativeOnDrawFrame whatever
         * the engine decides to do inside it. Draws standing still is an engine
         * that stopped rendering; textures standing still while the game is
         * loading a level is a loader that stopped making progress, which is
         * the one thing a "it hangs on load" report cannot say on its own.
         *
         * Deliberately not behind an environment flag and deliberately not on
         * the "frames=" line the harness parses: the whole value is that the
         * next log from the console answers this without anyone having to know
         * to ask for it.
         */
        if (frames % 100 == 0)
            trace("activity f=%ld draws=%ld assets=%ld textures=%ld decoded=%ldMiB",
                  frames, android_gl_draw_calls(), android_io_assets_opened(),
                  android_gl_textures_uploaded(), android_gl_decoded_mib());
    }

done:
    emulator_control_shutdown(frames);
    trace("frames=%ld", frames);
    trace("summary assets=%ld textures=%ld draws=%ld",
          android_io_assets_opened(), android_gl_textures_uploaded(),
          android_gl_draw_calls());
    trace("autopilot keys=%ld scenes=%ld",
          android_input_autopilot_keys(), android_input_autopilot_scenes());
    /* Why the loop stopped. "run finished" alone reads the same for a frame
     * limit, a window close and the game's own Exit, and only the last of
     * those means the player is done. */
    trace("run finished: %ld frames (%s)", frames,
          android_app_exit_requested() ? "the game asked to exit"
                                       : "the loader stopped driving it");

    /*
     * The engine started threads of its own and they are still running.
     * Tearing the window and the GL context down from here would pull them out
     * from under those threads and produce a segfault that has nothing to do
     * with why the loader stopped. Leave it to the kernel.
     */
    fflush(NULL);
    _exit(0);
}
