/*
 * Binary patches applied to libMassEffect.so after relocation.
 *
 * Ported from so_patch() in the Vita loader (vita-ref/loader/patch.c, MIT).
 * The offsets are safe to reuse verbatim because it is byte-for-byte the same
 * library - sha1 ea58b733d3d267ab639431b50539542faa43f0d0, which the harness
 * re-checks on every run before it starts anything.
 *
 * ---------------------------------------------------------------------------
 * What these three words actually do, verified against the disassembly rather
 * than taken on faith from the reference:
 *
 * The engine has two ways to read an asset. One opens a file descriptor and
 * uses read/lseek. The other goes back out through JNI - MainActivity.
 * GetInstance().getAssets(), then AssetManager.open() returning a
 * java.io.InputStream it reads through. Which one it uses is decided per path
 * by a prefix test against the string "appbundle:/", and the result is cached
 * in a per-stream flag at object offset 0xa4.
 *
 *   0x0022bf24  the prefix test, one of two identical copies:
 *      22bf28  ldr   ip, [pc, #516]     ; GOT base   -> 0x0043e8d0
 *      22bf2c  ldr   lr, [pc, #516]     ; GOT offset -> -0x64800
 *      22bf4c  add   r3, ip, lr         ; = 0x003da0d0, the literal
 *                                       ;   "appbundle:/" in .rodata
 *      22bf50  strlen loop over it
 *      22bf64  bl    0x22a8d0           ; compare(path, len)
 *      22bf68  subs  r8, r0, #0
 *      22bf6c  bne   0x22bf90           ; taken = "not an appbundle path"
 *      22bf70  mov   r3, #1
 *      22bf74  str   r3, [r4, #0xa4]    ; taken = "use the JNI reader"
 *
 *   0x0022b200  the reader, dispatching on that flag:
 *      22b204  ldr   r3, [r0, #0xa4]
 *      22b210  cmp   r3, #1
 *      22b214  beq   0x22b270           ; -> the JNI path
 *      22b218  ldr   r0, [r0, #16]      ; -> the fd path
 *      22b22c  bl    lseek@plt
 *
 * Turning the two `bne` into an unconditional `b` with the same displacement
 * makes the prefix test look permanently failed, so the 0xa4 flag is never
 * set; NOPing the `beq` makes the reader ignore the flag even if something
 * else sets it. Both halves are needed - the flag is written from more places
 * than the two patched here, and the reference patches both for that reason.
 *
 * ---------------------------------------------------------------------------
 * What these patches did NOT fix, corrected.
 *
 * An earlier version of this comment claimed they were the fix for a SIGSEGV
 * at pc = 0 that the run log showed alongside
 *
 *   FindClass: no fake class registered for 'com/ea/blast/MainActivity'
 *   GetStaticMethodID ... 'GetInstance' -> NULL
 *   GetMethodID ... 'getAssets' -> NULL
 *
 * That was wrong, and it was measured to be wrong twice. The crash was an
 * unthunked gettimeofday overflowing a bionic struct timeval on the stack (see
 * src/symtab_time.cpp); with these three patches applied and that thunk absent
 * the crash is bit-identical, and with the thunk present and so_patch_binary()
 * disabled the run reaches the same milestone. The fake classes were not the
 * cause either - jni.cpp null-checks every Call*Method, so a NULL jmethodID
 * returns 0 rather than jumping to it.
 *
 * What they actually buy is visible one milestone later, and it is not
 * cosmetic. With the JNI reader out of the way the engine opens its data with
 * open(), and the first thing the strace shows once it starts loading is
 *
 *   openat(AT_FDCWD, "appbundle:/EAMCore.ini", O_RDONLY)
 *
 * i.e. a real syscall this port can answer, which src/symtab_io.cpp then
 * redirects into the mounted tree. Without these patches that same read goes
 * back out through AssetManager.open() and would need a fake MainActivity, a
 * fake java.io.InputStream over a real FILE*, and a fake AssetFileDescriptor -
 * a few hundred lines of Java emulation whose entire job would be to hand the
 * engine bytes off a filesystem it can already read itself. That is why the
 * Vita port does it this way, and why these stay.
 *
 * ---------------------------------------------------------------------------
 * There is a fourth patch in the reference, a hook at 0x00320624 that delays a
 * thread which otherwise corrupts memory and faults somewhere different every
 * run. Deliberately not applied: it is a fix for a symptom we have not
 * observed, it would sit between us and the first non-deterministic crash we
 * do see, and applying it now would mean never learning whether this loader
 * needs it. If a crash starts moving around between runs, that is where to
 * look.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arm32_encodings.h"
#include "platform.h"
#include "so_util.h"
#include "trace.h"
#include "vfp_vector_patch.h"
#include "patch.h"

extern uintptr_t so_alloc_arena(so_module *so, uintptr_t range,
                                uintptr_t dst, size_t size);

/* ARM A32, all condition-AL:
 *   B <same target as the BNE at 0x22bf6c / 0x22bbe8>   (imm24 = 7)
 *   MOV r0, r0                                          (the canonical NOP;
 *   the architectural NOP hint is ARMv6K+ and this is an ARMv5TE binary) */
static const uint32_t kBranchAlways = 0xea000007;
static const uint32_t kNop          = 0xe1a00000;

struct TextPatch {
    uint32_t    offset;   /* from text_base, i.e. the link-time vaddr */
    uint32_t    expect;   /* the instruction that must be there now */
    uint32_t    value;
    const char *why;
};

static const TextPatch kPatches[] = {
    {0x0022bf6c, 0x1a000007, kBranchAlways, "appbundle:/ prefix test (copy 1) always fails"},
    {0x0022bbe8, 0x1a000007, kBranchAlways, "appbundle:/ prefix test (copy 2) always fails"},
    {0x0022b214, 0x0a000015, kNop,          "asset reader ignores the JNI-IO flag"},
};

/*
 * EASTL's UTF-16 string object in this build. The two pointers are the visible
 * range; the remaining words carry allocator/storage state and must be cloned
 * when a temporary view is passed back into the engine.
 */
struct EngineUtf16StringStorage {
    const uint16_t *begin;
    const uint16_t *end;
    void *word_08;
    void *word_0c;
    void *word_10;
};

using MountProvider = void (*)(void *, void *,
                               const EngineUtf16StringStorage *,
                               const EngineUtf16StringStorage *);

static MountProvider g_mount_provider;

/*
 * The game mounts its extracted content root at "/" from the call at
 * +0x1c3d7c. Its own VFS lookup, however, discards the leading separator and
 * starts at the first named component. It only selects a provider after that
 * component is found, so a provider attached to the root node can never answer
 * "/published/...". Every such lookup deliberately returned an empty handle,
 * which the resource loader later consumed as a non-empty vector.
 *
 * Mount the same backend once more at the first component it actually owns.
 * This is not a path rewrite: the source becomes "<content-root>/published"
 * and the virtual mount becomes "/published". The engine then builds its
 * normal directory nodes from the real extracted tree. Measured before making
 * this permanent: the previously null level index, layouts, subtitles, splash
 * and model providers all resolved, and the run advanced from one frame to
 * 120/120.
 *
 * This wrapper is reached from one patched BL call-site only. Hooking the
 * shared mount routine itself would require removing and reinstalling an
 * instruction hook while worker threads are live.
 */
extern "C" void mount_content_root(void *resolver, void *backend,
                                   const EngineUtf16StringStorage *source,
                                   const EngineUtf16StringStorage *mount)
{
    g_mount_provider(resolver, backend, source, mount);

    if (!source || !source->begin || !source->end ||
        source->end < source->begin)
        return;

    uint16_t source_text[256];
    size_t source_units = (size_t)(source->end - source->begin);
    static const uint16_t kPublishedName[] = {
        'p', 'u', 'b', 'l', 'i', 's', 'h', 'e', 'd'
    };
    if (source_units + sizeof(kPublishedName) / sizeof(kPublishedName[0]) + 1 >
        sizeof(source_text) / sizeof(source_text[0]))
        return;

    memcpy(source_text, source->begin, source_units * sizeof(uint16_t));
    if (source_units && source_text[source_units - 1] != '/')
        source_text[source_units++] = '/';
    memcpy(source_text + source_units, kPublishedName,
           sizeof(kPublishedName));
    source_units += sizeof(kPublishedName) / sizeof(kPublishedName[0]);

    static const uint16_t kPublishedMount[] = {
        '/', 'p', 'u', 'b', 'l', 'i', 's', 'h', 'e', 'd'
    };
    EngineUtf16StringStorage source_copy = *source;
    EngineUtf16StringStorage mount_copy = *mount;
    source_copy.begin = source_text;
    source_copy.end = source_text + source_units;
    mount_copy.begin = kPublishedMount;
    mount_copy.end =
        kPublishedMount +
        sizeof(kPublishedMount) / sizeof(kPublishedMount[0]);

    g_mount_provider(resolver, backend, &source_copy, &mount_copy);
    trace("mounted extracted content at /published");
}

/*
 * Patch one ARM BL without losing its return address.
 *
 * hook_address() intentionally emits B because it replaces function entries.
 * This target is a call-site, so it needs BL. The two-word arena trampoline can
 * still jump to a host address outside ARM's 24-bit branch range while LR
 * remains +0x1c3d80, the instruction after the original call.
 */
static bool patch_mount_call(so_module *mod)
{
    static const uint32_t kCallOffset = 0x001c3d7c;
    static const uint32_t kExpectedCall = 0xeb05265c;
    uint32_t *call = (uint32_t *)(mod->text_base + kCallOffset);
    if (*call != kExpectedCall) {
        warning("content mount patch at +0x%08x: expected %08x, found %08x - skipped\n",
                kCallOffset, kExpectedCall, *call);
        return false;
    }

    uintptr_t trampoline_addr =
        so_alloc_arena(mod, B_RANGE, B_OFFSET((uintptr_t)call),
                       2 * sizeof(uint32_t));
    if (!trampoline_addr) {
        warning("content mount patch: no ARM trampoline space - skipped\n");
        return false;
    }

    uint32_t trampoline[2] = {
        LDR_OFFS(PC, PC, -4),
        (uint32_t)(uintptr_t)&mount_content_root,
    };
    memcpy((void *)trampoline_addr, trampoline, sizeof(trampoline));
    *call = BL((uintptr_t)call, trampoline_addr);
    __builtin___clear_cache((char *)trampoline_addr,
                            (char *)(trampoline_addr + sizeof(trampoline)));

    g_mount_provider =
        (MountProvider)(mod->text_base + 0x0030d6f4);
    trace("patched +0x%08x: mount extracted content at /published",
          kCallOffset);
    return true;
}

/*
 * kill_network - ported from vita-ref/loader/patch/kill_network.c (MIT).
 *
 * EA shut the Origin servers behind this game down years ago, and the engine
 * does not treat that as a soft failure. im::easp::OriginInit_MainThread runs
 * during Application::OnCreate and walks straight into
 *
 *     im::easp::Init -> EASPWrapper::Update -> ChainedRequestManager
 *       -> OriginImpl::StartOrigin -> Connect::AutoLogin
 *       -> OriginProxy::StartOrigin -> ProtoHttpCreate      <- SIGSEGV
 *
 * which is where this port died once the writable storage directory was finally
 * in place. That chain is what the reference neutralises, and the offsets below
 * are its own, for this exact 1.0.58 build. Every one was re-checked against
 * this binary's symbol table and lands on an exact function start - unlike the
 * three appbundle offsets above, which came from a different game and are
 * skipped at runtime.
 *
 * Neutralising rather than removing the calls is deliberate, and is what the
 * reference does: the engine checks these return values and has a no-network
 * path behind each. SocketCreate/SocketControl answering -1 is "no socket",
 * which it already knows how to handle; ProtoSSLConnect answering 0 is a
 * connection that never completes; ShowOriginUI doing nothing keeps a login
 * overlay off a screen with no keyboard to type into.
 *
 * The two GalaxyAtWar hooks are not network calls but the widget that draws
 * their result - a live service with nothing left to answer. Left in, it builds
 * a panel out of a reply that never arrives.
 */
extern "C" void *kill_network_retthis(void *self) { return self; }
extern "C" int   kill_network_ret0(void)          { return 0; }
extern "C" int   kill_network_ret1(void)          { return 1; }
extern "C" int   kill_network_retminus1(void)     { return -1; }

struct NetworkHook {
    uint32_t    offset;
    uintptr_t   replacement;
    const char *what;
};

static void patch_kill_network(so_module *mod)
{
    static const NetworkHook kHooks[] = {
        {0x007b6ee8, (uintptr_t)&kill_network_retthis,
         "Origin::OriginMinimizedDialogState ctor"},
        {0x006d021c, (uintptr_t)&kill_network_ret0,
         "Origin::OriginImpl::ShowOriginUI"},
        {0x00622ba8, (uintptr_t)&kill_network_ret0,      "ProtoSSLConnect"},
        {0x00626c34, (uintptr_t)&kill_network_retminus1, "SocketCreate"},
        {0x00627c58, (uintptr_t)&kill_network_retminus1, "SocketControl"},
        {0x0040e31c, (uintptr_t)&kill_network_ret1,      "EA::Util::InstallWallpaper"},
        {0x0027c244, (uintptr_t)&kill_network_ret0,
         "masseffect_widgets::GalaxyAtWarWidget::OnDraw"},
        {0x0027e96c, (uintptr_t)&kill_network_ret0,
         "widgets::UIWidget::CreateInstance<GalaxyAtWarWidget>"},

        /*
         * EASP root-cut, now DISABLED.
         *
         * The Vita keeps EASP alive and refuses it a network; src/symtab_net.cpp
         * does the same here, and that is the architecture with no debt.
         * Disabled. With a real backtrace available these were shown to be the
         * cause of the crash they appeared to be working around: cutting Init
         * leaves EASPWrapper as a pointer with no constructed object, and
         * Application::OnLanguageChange -> im::easp::SetLanguage ->
         * EASPWrapper::SetLanguage -> basic_string::assign then writes through
         * it. Measured with them on: 10 frames. Kept here with their offsets
         * because the combination is the fallback if EASP-alive turns out to be
         * a wall, and re-deriving them cost four iterations.
         *
         *   {0x008ffae4, ret0}  im::easp::Init
         *   {0x00900a24, ret0}  im::easp::AddDelegate
         *   {0x0033d9bc, ret0}  Application::InitDebugMenu
         */

        /*
         * The debug menu, which is what was reaching into EASP all along.
         *
         * Every EASP fault this port hit after the writable-directory fix came
         * from one subtree: Application::InitDebugMenu -> Online::InitDebugMenu
         * -> Online::RefreshDebugMenu, enumerating online state for a developer
         * UI. Not gameplay. A retail port never builds it.
         *
         * Cutting here rather than inside EASP is what the measurements argued
         * for. The eight hooks above are the reference's and on their own move
         * this crash by exactly zero bytes: DirtySock dies in ProtoHttpCreate
         * +0x78 on its own uninitialised globals, upstream of every socket call
         * they neutralise. Four narrower cuts were then tried and each was
         * rejected by a run:
         *
         *   OriginInit_MainThread (+0x0090304c) + ProtoHttpCreate (+0x0061fb5c)
         *                                       -> moves to NetController ctor
         *   im::easp::Init (+0x008ffae4)        -> cascade: AddDelegate faults
         *   + im::easp::AddDelegate (+0x900a24) -> cascade: GetCurrentUser faults
         *   EASPWrapper::InitEASP (+0x008fd7d0) -> worse: EA::SP::Core unbuilt,
         *                                          AssertCoreExists faults
         *
         * im::easp has 130 functions and leaf-by-leaf had no end in sight.
         * Removing the caller removes all of them at once and leaves EASP fully
         * constructed for anything that legitimately wants it.
         */
    };

    for (unsigned i = 0; i < sizeof(kHooks) / sizeof(kHooks[0]); i++) {
        hook_address(mod, mod->text_base + kHooks[i].offset,
                     kHooks[i].replacement);
        trace("kill_network: +0x%08x %s", kHooks[i].offset, kHooks[i].what);
    }
}

void so_patch_binary(so_module *mod)
{
    patch_kill_network(mod);

    /*
     * Every patch is checked against the instruction it expects to replace
     * before it is written. The sha1 gate in the harness already proves this
     * is the right build, but that check does not run on the console, where
     * the game comes from whatever dump the player found. A patch landing mid
     * function in a slightly different binary would not fail here - it would
     * fail hundreds of frames later as an unexplainable fault, which is the
     * single worst way for this to go wrong.
     */
    for (unsigned i = 0; i < sizeof(kPatches) / sizeof(kPatches[0]); i++) {
        const TextPatch *p = &kPatches[i];
        uint32_t *addr = (uint32_t *)(mod->text_base + p->offset);

        if (*addr != p->expect) {
            warning("patch %u at +0x%08x: expected %08x, found %08x - skipped.\n"
                    "         The library does not match the build these offsets\n"
                    "         were taken from (MEI v1.0.58).\n",
                    i, p->offset, p->expect, *addr);
            continue;
        }

        *addr = p->value;
        trace("patched +0x%08x: %s", p->offset, p->why);
    }

    patch_mount_call(mod);
    /*
     * VFP short-vector expansion, now enabled for the on-device audio pass.
     *
     * This game's own audit (analysis/vfp_discover.py) found 29 regions in
     * EA::Audio::Core and vfp_vector_patch.cpp carries them, self-test 29/29
     * with a negative control. It stayed off while the harness was the only
     * evidence, because qemu emulates FPSCR LEN/STRIDE and the R36S does not:
     * under emulation the mixer is already correct, so enabling it could only
     * add risk to a path nothing here could exercise.
     *
     * The first hardware test removed that argument. Audio came back distorted,
     * "like an old radio" - the signature of vector instructions executing as
     * scalar, one lane of eight, which is exactly what an ARMv8 core does with
     * a short-vector encoding an ARMv5 core would have run eight-wide.
     *
     * Expect no audible change under the harness for the same reason it was off:
     * qemu runs those instructions correctly either way. The proof is on-device.
     */
    patch_vfp_short_vectors(mod);

    /*
     * The module is mapped PROT_READ|PROT_WRITE|PROT_EXEC at this point and
     * nothing has executed from it yet, so on the harness (qemu-arm, which
     * invalidates its translation blocks on guest writes) this is already
     * coherent. On the console it is not: writing through the D-cache leaves
     * stale words in the I-cache, and the first execution of a patched page
     * would run the original instruction. Flushing costs nothing here and is
     * the difference between "works on my emulator" and "works".
     */
    __builtin___clear_cache((char *)mod->text_base,
                            (char *)(mod->text_base + mod->text_size));
}
