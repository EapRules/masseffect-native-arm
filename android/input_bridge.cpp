/*
 * SDL controller input -> the JNI entry points exported by Mass Effect Infiltrator.
 *
 * This game is a Java-driven Android application, not NativeActivity. It has
 * no AInputQueue/ALooper imports; its Android layer calls
 * KeyboardAndroid.NativeOnKey* and TouchSurfaceAndroid.NativeOnPointerEvent.
 * Feeding the native_app_glue queue therefore looks plausible but is dead
 * code for this binary. The working Vita port calls these same exports, with
 * the signatures and module IDs copied below.
 *
 * Digital mapping follows that reference exactly. Analog movement does not go
 * through the touchscreen at all: this is the _GAMEPAD build and its sticks
 * belong to MassEffectActivity.NativeDispatchGenericMotionEvent. The virtual
 * touch drags remain available behind MASSEFFECT_STICK_TOUCH=1, and the menu
 * cursor keeps its own pointer events either way.
 *
 * MASSEFFECT_AUTOPILOT is separate from normal controls. It presses a varied
 * sequence of keys after startup and samples a framebuffer strip before each
 * swap. A scene is counted only when a substantial visual change occurs in a
 * short window after a synthetic key, rather than counting animation as proof
 * that input worked.
 */
#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "jni.h"
#include "so_util.h"
#include "trace.h"

#include "input_bridge.h"

enum {
    AKEYCODE_BACK          = 4,
    AKEYCODE_DPAD_UP       = 19,
    AKEYCODE_DPAD_DOWN     = 20,
    AKEYCODE_DPAD_LEFT     = 21,
    AKEYCODE_DPAD_RIGHT    = 22,
    AKEYCODE_DPAD_CENTER   = 23,
    AKEYCODE_B             = 30,
    AKEYCODE_Y             = 53,
    AKEYCODE_BUTTON_A      = 96,
    AKEYCODE_BUTTON_B      = 97,
    AKEYCODE_BUTTON_X      = 99,
    AKEYCODE_BUTTON_Y      = 100,
    AKEYCODE_BUTTON_L1     = 102,
    AKEYCODE_BUTTON_R1     = 103,
    AKEYCODE_BUTTON_START  = 108,
    AKEYCODE_BUTTON_SELECT = 109,
};

enum {
    ID_RAW_POINTER_DOWN = 0x6000c,
    ID_RAW_POINTER_MOVE = 0x4000c,
    ID_RAW_POINTER_UP   = 0x8000c,
    MODULE_KEYBOARD     = 600,
    MODULE_TOUCH_SCREEN = 1000,

    /*
     * There is no touchpad module in this game.
     *
     * 1100 is EA::Blast's touchpad id and the Vita reference defines it in
     * controls.h - but it never sends anything to it: all three of its
     * NativeOnPointerEvent calls use the touchscreen. This port routed the
     * right-stick camera gesture to 1100 anyway, and the first hardware test
     * came back with the sticks doing nothing in gameplay.
     *
     * The id is not registered in libMassEffect.so. EA::Blast::Register*Module()
     * accounts for every module this build has - 100 Accelerometer, 200 Battery,
     * 300 Device, 400 Display, 600 PhysicalKeyboard, 700 VirtualKeyboard,
     * 1000 Touchscreen, 1200 Vibrator, 1700 WebBrowserLauncher - and 1100 is not
     * among them. Nothing in the disassembly loads it as an immediate either. So
     * every right-stick event was addressed to a subsystem the engine has never
     * heard of, and was dropped without a diagnostic.
     *
     * Both sticks now go to the touchscreen with different pointer ids, which is
     * what two fingers on one panel look like - and what the game was built to
     * receive from a phone.
     */
    MODULE_TOUCH_PAD    = 1000,
};

using KeyFn = void (*)(JNIEnv *, void *, int, int, int);
/*
 * The game is armeabi softfp while this loader is armhf. pcs("aapcs") forces
 * the two float coordinates through core registers, which is the callee's ABI;
 * a normal hardfp function pointer would put them in s0/s1 and corrupt input.
 */
using PointerFn = void (__attribute__((pcs("aapcs"))) *)(
    JNIEnv *, void *, int, int, int, float, float);
using AccelFn = void (__attribute__((pcs("aapcs"))) *)(
    JNIEnv *, void *, float, float, float);
/*
 * This is the com.ea.games.meinfiltrator_GAMEPAD build - the Xperia Play SKU -
 * and it does not read movement from the touchscreen. Analog sticks arrive
 * through a separate gamepad entry point that takes both sticks in one call:
 *
 *   void NativeDispatchGenericMotionEvent(JNIEnv*, jobject,
 *                                         float lx, float ly,
 *                                         float rx, float ry, int action)
 *
 * Same softfp/hardfp hazard as the pointer path, four times over: without
 * pcs("aapcs") the four floats go in s0-s3 and the callee reads r2-r5, so the
 * character would move on garbage. The Vita reference calls this same export.
 */
using MotionFn = void (__attribute__((pcs("aapcs"))) *)(
    JNIEnv *, void *, float, float, float, float, int);

/* Action codes, from the Vita reference's pollPad(). */
enum {
    MOTION_LEFT_MOVE  = 0,
    MOTION_RIGHT_MOVE = 1,
    MOTION_LEFT_STOP  = 2,
    MOTION_RIGHT_STOP = 3,
};

static JNIEnv    *g_env = NULL;
static KeyFn      g_key_down = NULL;
static KeyFn      g_key_up = NULL;
static PointerFn  g_pointer = NULL;
static AccelFn    g_acceleration = NULL;
static MotionFn   g_motion = NULL;
static bool       g_stick_touch = false;
static bool       g_motion_trace_all = false;
static int        g_width = 640;
static int        g_height = 480;
static SDL_GameController *g_controllers[4] = {};

static bool g_left_active = false;
static bool g_right_active = false;
static int16_t g_lx = 0, g_ly = 0, g_rx = 0, g_ry = 0;
static float g_right_last_x = 0.0f, g_right_last_y = 0.0f;
static unsigned int g_right_pulses = 0;
static Uint8 g_accept_button = SDL_CONTROLLER_BUTTON_B;
static Uint8 g_back_button = SDL_CONTROLLER_BUTTON_A;

/*
 * The Xperia Play build uses accelerometer gestures for two actions that have
 * no ordinary key equivalent: tilting rotates/switches a weapon's fire mode,
 * and a sharp movement performs a Zero-G jump. R36S-class handhelds have no
 * motion sensor, so these replay the samples by hand - but only behind
 * MASSEFFECT_TRIGGER_ACCEL=1, since the triggers are worth more as fire and
 * cover buttons. See trigger_changed().
 *
 * These are the measured Android acceleration samples used by masseffect-vita's
 * optional D-pad accelerometer replacement, split at its explicit "for zero g
 * jump" boundary. Preserve the short cadence: the game recognises a gesture,
 * not a single static vector.
 */
struct AccelSample {
    float x;
    float y;
    float z;
    Uint16 delay_ms;
};

static const AccelSample kWeaponTilt[] = {
    {-0.998903f, -0.244644f, -1.292267f, 8},
    {-0.954221f, -0.367070f, -1.234914f, 8},
    {-1.029063f, -0.042471f, -1.256281f, 8},
    {-0.983264f, -0.268230f, -1.131455f, 8},
    {-1.049170f, -0.140187f, -1.296765f, 8},
    {-1.113959f, -0.304173f, -1.339498f, 8},
    {-1.655727f,  2.366763f, -2.261636f, 8},
    { 4.224865f, -2.941413f, -0.447724f, 8},
    { 2.926493f, -1.684568f, -1.615015f, 8},
    {-1.694824f,  1.514265f, -1.591399f, 8},
    {-2.287976f,  0.611223f, -1.149448f, 8},
    {-1.193269f, -0.221057f, -0.920038f, 8},
    {-1.024595f, -0.370441f, -1.369861f, 8},
    {-1.054755f, -0.157035f, -1.234914f, 8},
    {-1.159758f, -0.240151f, -1.291142f, 8},
    {-0.938582f, -0.179499f, -1.225918f, 8},
    {-1.046936f, -0.228920f, -1.210174f, 8},
    {-1.001137f, -0.234535f, -1.270900f, 8},
    {-1.055872f, -0.152542f, -1.367612f, 8},
    {-0.945284f, -0.057073f, -1.139327f, 8},
    {-1.002254f, -0.198594f, -1.224793f, 8},
    {-0.747567f, -0.235659f, -1.256281f, 8},
    {-0.699534f, -0.044716f, -1.474445f, 8},
    {-0.711821f,  0.117022f, -1.409221f, 8},
    {-1.023478f,  0.222602f, -1.628510f, 8},
    {-0.848101f, -0.006528f, -2.083956f, 8},
    {-0.272822f, -0.489498f, -3.042079f, 8},
    {-0.527508f, -0.292941f, -3.173653f, 8},
    {-1.081564f,  0.283254f, -3.189397f, 8},
    {-1.405508f,  0.348398f, -2.807047f, 8},
    {-1.814348f,  0.492166f, -2.401081f, 8},
    {-1.502691f,  0.095681f, -2.793552f, 8},
    {-1.847859f,  0.522491f, -3.261368f, 8},
};

static const AccelSample kZeroGJump[] = {
    {-1.761846f, -0.322144f, -1.657748f, 25},
    {-1.188801f, -0.022254f, -1.695983f, 8},
    {-0.794483f, -0.263738f, -2.931873f, 8},
    {-0.745333f, -0.150296f, -2.825040f, 8},
    {-0.873793f, -0.444571f, -1.743215f, 8},
    {-0.804536f, -0.127832f, -3.795533f, 8},
    {-1.742857f,  0.038399f, -1.233790f, 8},
    {-0.812356f,  0.224848f, -2.738449f, 8},
    { 0.863030f,  3.779731f, -9.721955f, 8},
    {-4.183607f,  3.622485f,  8.522037f, 8},
    {-6.169717f,  7.627765f, 23.052782f, 8},
    {-0.738630f,  6.358565f, 14.906132f, 8},
    { 0.151609f,  2.630712f,  9.449697f, 8},
    { 0.831056f,  3.416942f,  6.462183f, 8},
    {-2.905704f,  5.808204f, -9.787180f, 8},
    {-2.807403f,  1.275026f, -4.152019f, 8},
    {-2.541546f, -0.363701f,  0.349680f, 8},
    {-2.520322f,  0.236080f, -2.166049f, 8},
    {-1.934989f, -0.196346f, -0.763724f, 8},
    {-2.559419f, -0.453556f,  0.108265f, 8},
    {-1.904829f, -0.004282f, -1.038117f, 8},
    {-2.014299f, -0.291818f, -0.051879f, 8},
    {-2.021002f, -0.269354f, -1.045988f, 8},
    {-1.804294f, -0.230042f, -1.018999f, 8},
    {-2.016533f, -0.192978f, -0.518571f, 8},
    {-1.768549f, -0.101999f, -1.323754f, 8},
    {-1.541788f, -0.406383f, -1.025746f, 8},
};

static const AccelSample *g_accel_samples = NULL;
static size_t g_accel_count = 0;
static size_t g_accel_index = 0;
static Uint32 g_accel_next_ms = 0;
static const char *g_accel_name = NULL;
static bool g_l2_down = false;
static bool g_r2_down = false;

static void start_accel_gesture(const AccelSample *samples, size_t count,
                                const char *name)
{
    if (!g_acceleration) {
        warning("input: cannot start %s gesture; acceleration JNI symbol is missing\n",
                name);
        return;
    }
    g_accel_samples = samples;
    g_accel_count = count;
    g_accel_index = 0;
    g_accel_next_ms = SDL_GetTicks();
    g_accel_name = name;
    trace("input: %s accelerometer gesture started (%zu samples)", name, count);
}

static void update_accel_gesture(void)
{
    if (!g_accel_samples || !g_acceleration)
        return;

    Uint32 now = SDL_GetTicks();
    while (g_accel_index < g_accel_count &&
           (Sint32)(now - g_accel_next_ms) >= 0) {
        const AccelSample &sample = g_accel_samples[g_accel_index++];
        g_acceleration(g_env, (void *)0x42424242,
                       sample.x, sample.y, sample.z);
        g_accel_next_ms += sample.delay_ms;
    }

    if (g_accel_index == g_accel_count) {
        trace("input: %s accelerometer gesture completed", g_accel_name);
        g_accel_samples = NULL;
        g_accel_count = 0;
        g_accel_index = 0;
        g_accel_name = NULL;
    }
}

static void send_key(int code, bool down);
static void send_pointer(int raw_event, int module, int id, float x, float y);
static void aim_button(bool down);

/*
 * The synthetic aim tap, now opt-in: MASSEFFECT_AIM_TAP=1.
 *
 * It was the default for two builds on the belief that a tap at the crosshair
 * is how this game aims. Reversing the engine
 * showed that it is not, and that the tap was doing harm:
 *
 *   - Playable::AllowTapToAim (+0x1f5cb4) is `Settings[101] != 0 || the
 *     weapon's own force flag`, and it gates the enemy's *target state*, not
 *     the tap. With tap-to-aim off and an ordinary weapon no enemy ever reaches
 *     TargetState 4, which is the only state Interactive::OnEvent dispatches to
 *     TryTapToAim. The tap cannot enter aim in that mode at all - it is not a
 *     matter of hitting the right coordinate.
 *   - What the player aims with in that mode is the right stick: the crosshair
 *     is fixed at the centre of the screen and the stick turns the character
 *     under it. There is no aim state to enter.
 *   - Meanwhile Playable::OnEventSingleTap (+0x2074e0) - which is the *exit*
 *     handler, not the entry one - gave the tap two side effects worth being
 *     rid of: with the weapon holstered it fires Hud::TriggerNavAid(), and
 *     during the "fire_hold" rig state it calls Playable::AbortFire(), i.e. it
 *     cancelled the player's own shot.
 *
 * So L2 falls through to the shoulder button above it, the way R2 already
 * doubles R1: L1/L2 cover and slide, R1/R2 fire, right stick aims. The tap
 * stays behind the flag for the day someone wants to support the game's own
 * tap-to-aim scheme, which needs the setting on *and* an enemy under the
 * crosshair.
 */
static bool g_aim_tap = false;

/*
 * The triggers are combat buttons first, gesture emulators second.
 *
 * They used to be nothing but accelerometer gestures, inherited from Dead
 * Space. That cost nothing there; here it left a shooter with its shoulder
 * triggers doing no shooting, which is what the first hardware test reported.
 * The engine has no key of its own for a trigger - 0xf023 and 0xf025 are dead
 * in this binary - so each trigger doubles the shoulder button above it, which
 * is the layout a player expects: R2 fires like R1, L2 takes cover like L1.
 *
 * Unlike the shoulder buttons these need both edges, because Hud+0x489 is a
 * held-fire flag and a missing key-up would leave the weapon firing forever.
 *
 * MASSEFFECT_TRIGGER_ACCEL=1 restores the gestures. They are still the only way
 * to reach weapon-tilt and the Zero-G jump on a device with no motion sensor,
 * so the code stays; it is just no longer the default.
 */
static bool g_trigger_accel = false;

static void trigger_changed(bool left, Sint16 value)
{
    bool down = value > 16000;
    bool &was_down = left ? g_l2_down : g_r2_down;
    if (down == was_down)
        return;
    was_down = down;

    if (g_trigger_accel) {
        if (!down)
            return;
        if (left)
            start_accel_gesture(kWeaponTilt,
                                sizeof(kWeaponTilt) / sizeof(kWeaponTilt[0]),
                                "weapon-tilt/L2");
        else
            start_accel_gesture(kZeroGJump,
                                sizeof(kZeroGJump) / sizeof(kZeroGJump[0]),
                                "zero-g/R2");
        return;
    }

    /*
     * Only when the aim tap has been asked for. By default L2 is cover, and
     * falls through to the send_key() below. See g_aim_tap for why.
     */
    if (left && g_aim_tap) {
        aim_button(down);
        return;
    }

    int code = left ? AKEYCODE_BUTTON_L1 : AKEYCODE_BUTTON_R1;
    trace("input: trigger %s %s -> Android key=%d",
          left ? "L2" : "R2", down ? "down" : "up", code);
    send_key(code, down);
}

/*
 * Mass Effect Infiltrator's menus are touch-only. The Vita port uses the front touchscreen
 * and explicitly documents that a pad cannot navigate them. R36S has no touch
 * panel, so the d-pad drives this software cursor and physical A taps it.
 *
 * The cursor starts visible for the title/menu. Moving either analog stick
 * means gameplay and dismisses it; L3 brings it back when a menu is opened.
 */
/*
 * Aiming, which is a touch gesture and has no key.
 *
 * The gameplay key switch, Hud::onKeyDown (+0x118bdc), does contain one aim
 * path - BUTTON_L1 while standing still runs Playable::TryTapToAim over every
 * Interactive in the world - but it is behind a weapon gate:
 *
 *     11a46c: if (Weapon::GetWeaponTag(weapon) != "weapon_sniper") goto 11a658
 *
 * With any other weapon that branch only raises a notice. So the sniper aims
 * from a key and nothing else does, which is what the hardware test found.
 *
 * The general path is touch, and it is a hit test rather than a region.
 * Interactive::OnEvent (+0x174a80), on a single tap:
 *
 *     if (this[0xcc] != ev[0x10]) return 0            ; the DOWN's pointer id
 *     if (!InputRegionList::IsPointWithinScaled(this+8, ev[8], ev[12], 1.5f))
 *         return 0
 *     switch (this[0x78] - 1) {
 *         0 -> Playable::TryInteract()      2 -> Playable::TryMelee()
 *         1 -> Hud::OnBioticSelect()        3 -> Playable::TryTapToAim()   <- aim
 *     }
 *
 * Every enemy carries its own projected screen hitbox at Interactive+8 and the
 * tap is tested against it scaled by 1.5 (the float is a global, GOT +0xa816a4
 * -> +0xa8370c, read out of the binary as 1.5). There is no "aim region": the
 * tap has to land on the enemy, with that much finger tolerance.
 *
 * Where to tap is not a guess either. Hud::GetCrosshairPos (+0x10f90c) is
 * eleven instructions and returns w*0.5, h*0.5 - the crosshair is the fixed
 * centre of the screen. So the button taps the centre, and the player brings
 * the enemy under it with the right stick, which already drives the camera.
 * This is not auto-target: with no enemy under the crosshair, nothing happens.
 *
 * Leaving aim is a tap on the left half. The split comes from
 * InputSchemeBasic::UpdateRegions (+0x123f2c), whose left region is
 * (0, 0, 0.55*w, h) - the 0.55 is a config float in the GOT, and 0.45/0.55 are
 * the only two values it takes.
 *
 * HOLD, AND WHY IT IS NOT A HELD POINTER.
 *
 * The button is hold-to-aim: press to aim, release to come out. The obvious
 * implementation - keep the pointer DOWN for as long as the trigger is held -
 * does not work, and InputForwarderTaps says so in two measured numbers:
 *
 *   - it tracks 12 pointer slots (the OnUpdateEvent loop runs r5 = 0..11 over
 *     28-byte entries), so the id has to be under 12;
 *   - a press held longer than 500 ms stops being a single tap (the global at
 *     +0xa838ac, read out of the binary as 500). Past that it becomes
 *     PointerEvent<1004> SingleHoldTap, and Playable::OnEventSingleTap only
 *     handles <1002>. A held pointer would aim at nothing.
 *
 * So a hold is two short taps, not one long press: centre on the way in, left
 * half on the way out. That is the shape InputForwarderTaps actually emits.
 *
 * The engine also defers the tap - the single finger event goes out on a timer
 * after the UP - so the bridge releases and lets it run. Which means a press
 * and release faster than one tap would overlap; hence the one-deep queue.
 */
static const float kRegionSplit = 0.55f;
static const int kAimPointerId = 4;
static Uint32 g_aim_tap_ms = 100;
static bool g_aim_hold = true;
static bool g_aim_engaged = false;
static bool g_aim_tap_active = false;
static Uint32 g_aim_tap_release_ms = 0;
static float g_aim_tap_x = 0.0f, g_aim_tap_y = 0.0f;
static float g_aim_x = -1.0f, g_aim_y = -1.0f;
/* One-deep queue: 0 none, 1 enter, 2 exit. */
static int g_aim_pending = 0;

static const int kCursorPointerId = 3;
static const float kCursorSpeed = 420.0f;
static float g_cursor_x = -1.0f, g_cursor_y = -1.0f;
static int g_cursor_dx = 0, g_cursor_dy = 0;
static bool g_cursor_visible = false;
static bool g_cursor_down = false;
static Uint32 g_cursor_last_ms = 0;

static bool g_autopilot = false;
static long g_auto_keys = 0;
static long g_auto_scenes = 0;
static long g_pending_key_frame = -1;
static int  g_pending_key_serial = 0;
static int  g_counted_key_serial = 0;

static void update_sticks(void);

static const int kAutopilotKeys[] = {
    AKEYCODE_DPAD_CENTER,
    AKEYCODE_DPAD_DOWN,
    AKEYCODE_DPAD_CENTER,
    AKEYCODE_BUTTON_START,
    AKEYCODE_BACK,
    AKEYCODE_DPAD_RIGHT,
    AKEYCODE_DPAD_CENTER,
    AKEYCODE_DPAD_UP,
};

/*
 * Gameplay keys, read out of the binary rather than guessed.
 *
 * EA::Blast::KeyboardAndroid::RawToStdKey (+0x361ad0) turns an Android keycode
 * into the engine's own Key enum through a 165-entry table at +0x987430. The
 * gamepad range maps
 *
 *   BUTTON_A 96 -> 0xf026   BUTTON_X  99 -> 0xf029   BUTTON_L1 102 -> 0xf022
 *   BUTTON_B 97 -> 0xf027   BUTTON_Y 100 -> 0xf02a   BUTTON_R1 103 -> 0xf024
 *
 * and Hud::onKeyDown (+0x118bdc) switches on exactly those six values:
 *
 *   0xf026  Hud::TriggerNavAid()          (key-up: Morality::ChooseRenegade)
 *   0xf027  Playable::TryMelee() / Interactive - melee and context actions
 *   0xf029  Playable::tryCloak()          - tactical cloak
 *   0xf02a  Hud::TryFireBiotic()          - biotic power
 *   0xf022  Playable::TrySlide()          - slide into cover
 *   0xf024  FIRE. Sets the held-fire flag Hud+0x489, which Hud::OnUpdate
 *           consumes every frame; with a "weapon_sniper" tag it runs
 *           Playable::OnSniperEvent() instead, which is the scope. Key-up
 *           clears the flag, so this is a hold-to-shoot button.
 *
 * There is no separate aim key: 0xf023/0xf025 (BUTTON_L2/R2) and 0xf030-0xf031
 * (THUMBL/THUMBR) appear in no handler in the binary. Aiming is the right
 * stick; see dispatch_motion().
 *
 * The catch is that a second table sits in front of that one.
 * KeyboardAndroidXperiaPlay::RawToStdKey (+0x92ed90) intercepts exactly four
 * codes - DPAD_CENTER 23, BACK 4, BUTTON_X 99 and BUTTON_Y 100 - and turns them
 * into 0xf02c-0xf02f, which no handler in this binary reads; everything else
 * falls through to the base table. Keyboard::Create (+0x92daec) installs it when
 * the device reports manufacturer "sony" and name "R800", and this port reports
 * exactly that (jni/classes/ea_SystemAndroidDelegate.h), as does the Vita
 * reference. So those four codes are unusable here.
 *
 * That is the whole of the missing-combat-buttons report. A was sent as
 * DPAD_CENTER and B as BACK - two of the four the subclass swallows - and X and
 * Y were sent as BUTTON_X/BUTTON_Y, the other two. Every face button landed in
 * the dead range. The menus never noticed because they are driven by the
 * software cursor's pointer events, not by these keys.
 *
 * The mapping below therefore only uses codes the subclass lets through, which
 * also makes it correct whichever table ends up installed. Two of them are
 * letter keys: the base table sends keycodes 29-54 to ASCII 'a'-'z', and
 * Hud::onKeyDown gives 'b' and 'y' the same branches as 0xf029 and 0xf02a. The
 * Vita reference reaches cloak the same way, through AKEYCODE_B.
 */
static int map_button(Uint8 button)
{
    if (button == g_accept_button)
        return AKEYCODE_BUTTON_A;
    if (button == g_back_button)
        return AKEYCODE_BUTTON_B;

    switch (button) {
    case SDL_CONTROLLER_BUTTON_X:             return AKEYCODE_B;
    case SDL_CONTROLLER_BUTTON_Y:             return AKEYCODE_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return AKEYCODE_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AKEYCODE_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_START:         return AKEYCODE_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_BACK:          return AKEYCODE_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return AKEYCODE_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return AKEYCODE_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return AKEYCODE_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return AKEYCODE_DPAD_RIGHT;
    default:                                  return 0;
    }
}

static void send_key(int code, bool down)
{
    KeyFn fn = down ? g_key_down : g_key_up;
    if (fn)
        fn(g_env, (void *)0x42424242, MODULE_KEYBOARD, code, 1);
}

static float normalise_axis(int16_t value, float deadzone, float maximum)
{
    float v = value / 32767.0f;
    float magnitude = fabsf(v);
    if (magnitude <= deadzone)
        return 0.0f;
    float scaled = (magnitude - deadzone) / (1.0f - deadzone);
    scaled = std::min(scaled, maximum);
    return copysignf(scaled, v);
}

/*
 * The gamepad entry point wants the raw normalised stick, not the virtual
 * touch radius, so it gets the reference's own curve rather than
 * normalise_axis(). Both deadzone pairs are masseffect-vita's defaults:
 * settings.c seeds 0.13/0.12 for the inner ones, controls.c defines the outer.
 */
static const float kLeftInnerDeadzone  = 0.13f;
static const float kLeftOuterDeadzone  = 0.992f;
static const float kRightInnerDeadzone = 0.12f;
static const float kRightOuterDeadzone = 1.0f;

static float lerp_axis(float x1, float y1, float x3, float y3, float x2)
{
    return ((x2 - x1) * (y3 - y1) / (x3 - x1)) + y1;
}

static float coord_normalize(float value, float deadzone_min, float deadzone_max)
{
    float sign = value < 0.0f ? -1.0f : 1.0f;

    if (fabsf(value) < deadzone_min)
        return 0.0f;
    if (fabsf(value) > deadzone_max)
        return sign;
    return lerp_axis(0.0f, deadzone_min * sign, sign, deadzone_max * sign, value);
}

/*
 * Every gamepad motion event this bridge hands the engine, on the record.
 *
 * Held sticks fire one move per frame, so the two move actions are sampled the
 * way send_pointer() samples its MOVEs - the first and then every 30th.
 * The two stop actions fire once per release and are always printed, because a
 * missing stop is what leaves the character walking into a wall.
 * MASSEFFECT_MOTION_TRACE_ALL=1 prints every dispatch.
 */
static void send_motion(float lx, float ly, float rx, float ry, int action)
{
    static unsigned long moves[2] = {0, 0};

    if (!g_motion)
        return;

    const bool is_move = action == MOTION_LEFT_MOVE || action == MOTION_RIGHT_MOVE;
    const bool sampled = is_move && !g_motion_trace_all &&
                         (moves[action]++ % 30) != 0;
    if (!sampled) {
        static const char *names[] = {"left-move", "right-move",
                                      "left-stop", "right-stop"};
        trace("input: motion l=%.3f,%.3f r=%.3f,%.3f mode=%d (%s)%s",
              lx, ly, rx, ry, action, names[action],
              is_move && !g_motion_trace_all ? " (every 30th)" : "");
    }

    g_motion(g_env, (void *)0x42424242, lx, ly, rx, ry, action);
}

/*
 * The gate that keeps an impatient thumb from killing the game.
 *
 * NativeDispatchGenericMotionEvent's first act is im::IApplication::GetApplication(),
 * and that function *constructs* the whole application on its first ever call:
 * it sets a first-time flag, installs the Bullet and FMOD allocators, then runs
 * the guarded `new Application` - which builds im::M3GApplication and calls
 * im::ResourcesManager::Init(). Dispatching a stick before the engine has built
 * that object therefore does not deliver input, it *creates a second engine*
 * from the input thread. Measured under qemu, the very first dispatch died in
 * ResourcesManager::Init at a null dereference:
 *
 *     SIGSEGV at 0x4c
 *     im::ResourcesManager::Init +0x38
 *     im::M3GApplication::M3GApplication +0x478
 *     Application::Application +0x10
 *     im::IApplication::GetApplication +0x68
 *     NativeDispatchGenericMotionEvent +0x24
 *
 * So the dispatch waits for the singleton to exist. Every pc-relative literal
 * in GetApplication resolves to one base, text_base + 0xa98c48, holding the
 * first-time flag at +0xe8, the Itanium-ABI guard at +0xec and the instance
 * pointer at +0xfc. Guard bit 0 set means the object is built and GetApplication
 * returns it without constructing anything.
 *
 * Pinned to this build the same way the orientation handshake word is
 * (src/main.cpp), and the .so is sha1-pinned by the harness.
 * MASSEFFECT_MOTION_NO_GATE=1 dispatches regardless, which is how the next
 * device log can tell "the gate never opened" from "the engine ignored us".
 */
static volatile const uint32_t *g_app_guard = NULL;
static bool g_motion_gate_open = false;
static bool g_motion_no_gate = false;

static bool app_singleton_ready(void)
{
    if (g_motion_gate_open)
        return true;
    if (g_motion_no_gate)
        return true;
    if (!g_app_guard)
        return false;
    if (!(*g_app_guard & 1u))
        return false;

    g_motion_gate_open = true;
    trace("input: engine Application singleton is up; analog motion events "
          "are now live");
    return true;
}

/*
 * The right stick has to arrive as a swipe, not as a position.
 *
 * Hud::moveCamera (+0x10e620) is the camera half of the gamepad entry point,
 * and it does not read the stick as a direction. Every one of its eight bands -
 * |axis| over 0.1, 0.4 and 0.9, per sign, per axis - is guarded by the delta
 * against the previous call, which it stores at the end:
 *
 *     Hud+0x4b4 = rx ; Hud+0x4b8 = ry     (epilogue, +0x10e8e0)
 *     if ((ry - Hud[0x4b8]) < 0) { Hud[0x4a9] = 1; Hud[0x4b0] = -speed; }
 *
 * So the camera only turns while the value keeps growing. It is written for a
 * finger dragging across glass, where every sample is further along than the
 * last. A stick held at the rail reports the same number every frame, the delta
 * is exactly zero, and the camera does not move. Hud::movePlayer (+0x10f058)
 * has no such test - it writes the movement vector straight into Playable+0x400
 * from the absolute value - which is why the left stick walks perfectly with
 * this same bridge and the right one does not.
 *
 * It also explains the shape of the hardware report: horizontal worked because
 * a thumb rolling left and right keeps producing new values, while vertical
 * "shook but did not move" because SDL pinned it at exactly -1.000 / 1.000 -
 * a stream of zero deltas, with the shake coming from the odd frame that
 * happened to differ.
 *
 * The fix is to hand the engine the ramp it is looking for. A held axis is sent
 * as a three-frame cycle of 0.94, 0.97 and 1.00 of its value, so two frames out
 * of three carry a delta with the right sign and the third is ignored by every
 * band. The multipliers are proportional on purpose: they keep the sample
 * inside the same speed band as the real deflection, so the camera moves at the
 * speed the player asked for rather than a scaled-down one.
 *
 * Only the right stick is dithered. MASSEFFECT_CAMERA_NO_SWIPE=1 sends the raw
 * value instead, which is the behaviour the first hardware test had.
 */
static const float kSwipeRamp[] = {0.94f, 0.97f, 1.0f};
static bool g_camera_no_swipe = false;

static float swipe_ramp(float value, unsigned int *phase)
{
    if (value == 0.0f || g_camera_no_swipe) {
        *phase = 0;
        return value;
    }
    float scaled = value * kSwipeRamp[*phase % (sizeof(kSwipeRamp) /
                                                sizeof(kSwipeRamp[0]))];
    (*phase)++;
    return scaled;
}

/*
 * The reference's move/stop state machine, kept literally.
 *
 * Releasing a stick produces a final move carrying zeroes on the frame the
 * axis reaches centre, then exactly one stop on the frame after - which is why
 * it needs two frames of history and not one.
 */
static void dispatch_motion(void)
{
    static float last_lx = 0.0f, last_ly = 0.0f, last_rx = 0.0f, last_ry = 0.0f;
    static float prev_lx = 0.0f, prev_ly = 0.0f, prev_rx = 0.0f, prev_ry = 0.0f;

    if (!app_singleton_ready()) {
        static bool complained = false;
        if (!complained && (g_lx || g_ly || g_rx || g_ry)) {
            complained = true;
            trace("input: stick moved before the engine Application exists; "
                  "holding motion events back (this dispatch would construct a "
                  "second engine and crash)");
        }
        return;
    }

    float lx = coord_normalize(g_lx / 32767.0f, kLeftInnerDeadzone,  kLeftOuterDeadzone);
    float ly = coord_normalize(g_ly / 32767.0f, kLeftInnerDeadzone,  kLeftOuterDeadzone);
    float rx = coord_normalize(g_rx / 32767.0f, kRightInnerDeadzone, kRightOuterDeadzone);
    float ry = coord_normalize(g_ry / 32767.0f, kRightInnerDeadzone, kRightOuterDeadzone);

    bool left_centred = lx == 0.0f && ly == 0.0f;
    bool right_centred = rx == 0.0f && ry == 0.0f;
    bool left_was_centred = last_lx == 0.0f && last_ly == 0.0f;
    bool right_was_centred = last_rx == 0.0f && last_ry == 0.0f;

    /*
     * The state machine votes on the real stick; only the values that leave for
     * the engine are ramped. Dithering before this point would let a sample dip
     * to zero and fake a stop the player never asked for.
     */
    static unsigned int rx_phase = 0, ry_phase = 0;
    float swipe_rx = swipe_ramp(rx, &rx_phase);
    float swipe_ry = swipe_ramp(ry, &ry_phase);

    if (left_centred && left_was_centred && (prev_lx != 0.0f || prev_ly != 0.0f))
        send_motion(lx, ly, swipe_rx, swipe_ry, MOTION_LEFT_STOP);
    if (right_centred && right_was_centred && (prev_rx != 0.0f || prev_ry != 0.0f))
        send_motion(lx, ly, swipe_rx, swipe_ry, MOTION_RIGHT_STOP);
    if (!left_centred || !left_was_centred)
        send_motion(lx, ly, swipe_rx, swipe_ry, MOTION_LEFT_MOVE);
    if (!right_centred || !right_was_centred)
        send_motion(lx, ly, swipe_rx, swipe_ry, MOTION_RIGHT_MOVE);

    prev_lx = last_lx; prev_ly = last_ly;
    prev_rx = last_rx; prev_ry = last_ry;
    last_lx = lx; last_ly = ly;
    last_rx = rx; last_ry = ry;
}

/*
 * Every pointer event this bridge hands the engine, on the record.
 *
 * The first two device tests both came back "the pads do nothing in gameplay"
 * and neither log could say whether that was because no event was sent, or one
 * was sent to the wrong module, or the engine ignored a correct one. Those are
 * three different bugs and the log distinguished none of them.
 *
 * Gated because a held stick sends one MOVE per frame and would bury the log in
 * minutes: every DOWN and UP is printed, MOVEs only every 30th. DOWN/UP are the
 * events that start and end a gesture, so they are the ones that carry meaning;
 * a sampled MOVE is enough to show the drag is live and where it is going.
 */
static void send_pointer(int raw_event, int module, int id, float x, float y)
{
    static unsigned long moves = 0;

    if (!g_pointer) {
        static bool complained = false;
        if (!complained) {
            complained = true;
            warning("input: NativeOnPointerEvent never resolved - every touch "
                    "this bridge produces is being dropped.\n");
        }
        return;
    }

    const bool is_move = raw_event == ID_RAW_POINTER_MOVE;
    if (!is_move || (moves++ % 30) == 0) {
        trace("input: pointer %s module=%d id=%d at %.0f,%.0f%s",
              raw_event == ID_RAW_POINTER_DOWN ? "DOWN" :
              raw_event == ID_RAW_POINTER_UP   ? "UP  " : "MOVE",
              module, id, x, y,
              is_move ? " (every 30th)" : "");
    }

    g_pointer(g_env, (void *)0x42424242, raw_event, module, id, x, y);
}

/*
 * Whether this port draws a menu cursor at all - MASSEFFECT_NO_CURSOR=1 says no.
 *
 * The cursor exists because the menus were reached by touch on a phone and this
 * console has none: the d-pad drives a painted pointer and the accept button
 * synthesises a tap under it. It is also the only thing in the port that paints
 * into the default framebuffer behind the engine's back, which is where the
 * trail on the Mali came from.
 *
 * The player reports the menus responding to the d-pad directly, which - if it
 * holds - makes the whole mechanism redundant. THAT IS NOT ESTABLISHED. This
 * build is `meinfiltrator_gamepad` and it does route face buttons through
 * KeyboardAndroidXperiaPlay, so d-pad navigation is plausible; but the menu
 * widgets may equally be listening for pointer events and nothing else, in
 * which case turning the cursor off leaves the menus unnavigable. This switch
 * is here so that question gets answered on the hardware in one run instead of
 * being argued about. Unset - the default - is byte-for-byte today's behaviour.
 *
 * Gating cursor_show() is deliberately the whole implementation. Every other
 * behaviour keys off g_cursor_visible, so refusing to raise it once makes the
 * rest follow on its own: the d-pad and the accept button stop being
 * intercepted and fall through to map_button() - AKEYCODE_DPAD_* and
 * AKEYCODE_BUTTON_A to module 600, the same path the combat buttons already
 * use - android_cursor_draw() returns early because the position query reports
 * invisible, and aiming stops being suppressed. One condition, no second
 * version of the input path to keep in step with this one.
 *
 * Known cost when it is on: android_input_cursor_set() becomes a no-op, so the
 * emulator's click/move commands (emulator/send.sh) do nothing. That is a
 * development path, not a player-facing one, and the switch is for the console.
 */
static bool cursor_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("MASSEFFECT_NO_CURSOR");
        enabled = (v && *v && *v != '0') ? 0 : 1;
        trace("input: menu cursor %s%s", enabled ? "on" : "OFF",
              enabled ? "" : " (MASSEFFECT_NO_CURSOR) - the d-pad and the "
                             "accept button go straight to the game; unset it "
                             "if the menus stop navigating");
    }
    return enabled != 0;
}

static void cursor_show(void)
{
    if (!cursor_enabled())
        return;

    if (!g_cursor_visible) {
        g_cursor_x = g_width * 0.5f;
        g_cursor_y = g_height * 0.5f;
        trace("input: MODE -> menu (cursor shown at %.0f,%.0f); sticks drive the "
          "cursor, A taps", g_cursor_x, g_cursor_y);
    }
    g_cursor_visible = true;
    g_cursor_last_ms = SDL_GetTicks();
}

static void cursor_hide(void)
{
    if (g_cursor_down) {
        send_pointer(ID_RAW_POINTER_UP, MODULE_TOUCH_SCREEN, kCursorPointerId,
                     g_cursor_x, g_cursor_y);
        g_cursor_down = false;
    }
    if (g_cursor_visible)
        trace("input: MODE -> gameplay (cursor hidden); sticks now send "
              "touchscreen drags");
    g_cursor_visible = false;
    g_cursor_dx = 0;
    g_cursor_dy = 0;
}

static void cursor_tap(bool down)
{
    if (!g_cursor_visible)
        return;
    send_pointer(down ? ID_RAW_POINTER_DOWN : ID_RAW_POINTER_UP,
                 MODULE_TOUCH_SCREEN, kCursorPointerId,
                 g_cursor_x, g_cursor_y);
    g_cursor_down = down;
    trace("input: menu tap %s at %.0f,%.0f",
          down ? "down" : "up", g_cursor_x, g_cursor_y);
}

/* Fire one tap now. enter = crosshair, !enter = left half. */
static void aim_tap_emit(bool enter)
{
    if (enter) {
        g_aim_tap_x = g_aim_x >= 0.0f ? g_aim_x : g_width * 0.5f;
        g_aim_tap_y = g_aim_y >= 0.0f ? g_aim_y : g_height * 0.5f;
    } else {
        g_aim_tap_x = g_width * kRegionSplit * 0.5f;
        g_aim_tap_y = g_height * 0.5f;
    }
    g_aim_tap_active = true;
    g_aim_tap_release_ms = SDL_GetTicks() + g_aim_tap_ms;

    trace("input: aim tap %s at %.0f,%.0f (id=%d, %u ms)",
          enter ? "ENTER (crosshair)" : "EXIT (left half)",
          g_aim_tap_x, g_aim_tap_y, kAimPointerId, (unsigned int)g_aim_tap_ms);
    send_pointer(ID_RAW_POINTER_DOWN, MODULE_TOUCH_SCREEN, kAimPointerId,
                 g_aim_tap_x, g_aim_tap_y);
}

/*
 * The aim button changed state.
 *
 * The engaged flag is the bridge's own bookkeeping, not the engine's - nothing
 * reachable from here reports whether the player is currently aiming. It exists
 * so a release only sends the exit tap when we believe we are aiming: a stray
 * tap on the left half is not harmless, because tap_to_move_enabled
 * (Settings[102]) would read it as an order to walk there.
 */
static void aim_button(bool down)
{
    /* A menu is open: its cursor owns the touchscreen, leave it alone. */
    if (g_cursor_visible) {
        if (down)
            trace("input: aim ignored while the menu cursor is up");
        return;
    }

    bool enter;
    if (g_aim_hold) {
        if (down == g_aim_engaged)
            return;
        enter = down;
    } else {
        if (!down)
            return;
        enter = !g_aim_engaged;
    }
    g_aim_engaged = enter;

    if (g_aim_tap_active) {
        /*
         * A tap is still on the way out. Queue this one instead of dropping
         * it: a quick tap of the trigger is a press and a release inside the
         * same 100 ms, and losing the release would leave the player aiming.
         */
        g_aim_pending = enter ? 1 : 2;
        trace("input: aim %s queued behind the tap in flight",
              enter ? "ENTER" : "EXIT");
        return;
    }
    aim_tap_emit(enter);
}

static void aim_tap_update(void)
{
    if (g_aim_tap_active) {
        if ((Sint32)(SDL_GetTicks() - g_aim_tap_release_ms) < 0)
            return;
        send_pointer(ID_RAW_POINTER_UP, MODULE_TOUCH_SCREEN, kAimPointerId,
                     g_aim_tap_x, g_aim_tap_y);
        g_aim_tap_active = false;
    }
    if (g_aim_pending) {
        bool enter = g_aim_pending == 1;
        g_aim_pending = 0;
        aim_tap_emit(enter);
    }
}

void android_input_tick(void)
{
    aim_tap_update();

    /*
     * SDL axis events report value changes, not elapsed held time. The game
     * consumes both virtual sticks once per Android poll, so refresh them once
     * per rendered frame even when the physical axis value is unchanged.
     */
    update_sticks();
    update_accel_gesture();

    Uint32 now = SDL_GetTicks();
    if (!g_cursor_last_ms)
        g_cursor_last_ms = now;
    float dt = (now - g_cursor_last_ms) / 1000.0f;
    g_cursor_last_ms = now;

    if (!g_cursor_visible || (!g_cursor_dx && !g_cursor_dy))
        return;

    g_cursor_x += g_cursor_dx * kCursorSpeed * dt;
    g_cursor_y += g_cursor_dy * kCursorSpeed * dt;
    g_cursor_x = std::max(0.0f, std::min(g_cursor_x, (float)g_width - 1.0f));
    g_cursor_y = std::max(0.0f, std::min(g_cursor_y, (float)g_height - 1.0f));

    if (g_cursor_down)
        send_pointer(ID_RAW_POINTER_MOVE, MODULE_TOUCH_SCREEN, kCursorPointerId,
                     g_cursor_x, g_cursor_y);
}

extern "C" void android_input_cursor_position(float *x, float *y, int *visible)
{
    if (x)
        *x = g_cursor_x;
    if (y)
        *y = g_cursor_y;
    if (visible)
        *visible = g_cursor_visible ? 1 : 0;
}

void android_input_cursor_set(float x, float y)
{
    cursor_show();
    g_cursor_x = std::max(0.0f, std::min(x, (float)g_width - 1.0f));
    g_cursor_y = std::max(0.0f, std::min(y, (float)g_height - 1.0f));
}

void android_input_cursor_press(bool down)
{
    cursor_tap(down);
}

bool android_input_inject_control(const char *name, bool down)
{
    if (!name)
        return false;

    /* The aim button, so the emulator can exercise the tap without a pad. */
    if (!strcasecmp(name, "aim")) {
        aim_button(down);
        return true;
    }

    if (!strcasecmp(name, "l2") || !strcasecmp(name, "r2")) {
        SDL_Event event = {};
        event.type = SDL_CONTROLLERAXISMOTION;
        event.caxis.axis = !strcasecmp(name, "l2")
            ? SDL_CONTROLLER_AXIS_TRIGGERLEFT
            : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        event.caxis.value = down ? 32767 : 0;
        return android_input_event(&event);
    }

    Uint8 button;
    if (!strcasecmp(name, "a"))             button = g_accept_button;
    else if (!strcasecmp(name, "b"))        button = g_back_button;
    else if (!strcasecmp(name, "x"))        button = SDL_CONTROLLER_BUTTON_X;
    else if (!strcasecmp(name, "y"))        button = SDL_CONTROLLER_BUTTON_Y;
    else if (!strcasecmp(name, "l1"))       button = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    else if (!strcasecmp(name, "r1"))       button = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    else if (!strcasecmp(name, "start"))    button = SDL_CONTROLLER_BUTTON_START;
    else if (!strcasecmp(name, "select"))   button = SDL_CONTROLLER_BUTTON_BACK;
    else if (!strcasecmp(name, "l3"))       button = SDL_CONTROLLER_BUTTON_LEFTSTICK;
    else if (!strcasecmp(name, "r3"))       button = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    else if (!strcasecmp(name, "up"))       button = SDL_CONTROLLER_BUTTON_DPAD_UP;
    else if (!strcasecmp(name, "down"))     button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    else if (!strcasecmp(name, "left"))     button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    else if (!strcasecmp(name, "right"))    button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    else return false;

    SDL_Event event = {};
    event.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    event.cbutton.button = button;
    return android_input_event(&event);
}

bool android_input_inject_stick(const char *name, float x, float y)
{
    if (!name || (strcasecmp(name, "left") && strcasecmp(name, "right")))
        return false;

    x = std::max(-1.0f, std::min(x, 1.0f));
    y = std::max(-1.0f, std::min(y, 1.0f));
    int16_t raw_x = (int16_t)lrintf(x * 32767.0f);
    int16_t raw_y = (int16_t)lrintf(y * 32767.0f);
    if (!strcasecmp(name, "left")) {
        g_lx = raw_x;
        g_ly = raw_y;
    } else {
        g_rx = raw_x;
        g_ry = raw_y;
    }
    return true;
}

static void update_sticks(void)
{
    if (!g_pointer && !g_motion)
        return;

    float lx = normalise_axis(g_lx, 0.15f, 0.76f);
    float ly = normalise_axis(g_ly, 0.15f, 0.76f);
    float rx = normalise_axis(g_rx, 0.15f, 1.00f);
    float ry = normalise_axis(g_ry, 0.15f, 1.00f);

    /*
     * Any stick deflection means gameplay, whichever path carries it: the menu
     * cursor is driven by the d-pad and must get out of the way first.
     */
    if (lx != 0.0f || ly != 0.0f || rx != 0.0f || ry != 0.0f)
        cursor_hide();

    /*
     * Gameplay movement goes to the gamepad entry point, not the touchscreen.
     * The menu cursor keeps its own pointer events (cursor_tap /
     * android_input_tick) - that path works and is untouched.
     *
     * dispatch_motion() needs no mode check of its own: a centred stick emits
     * nothing but its one trailing stop, so a visible menu cursor never sees a
     * stray motion event.
     */
    if (g_motion && !g_stick_touch) {
        dispatch_motion();
        return;
    }

    const float left_base_x  = g_width  * (192.0f / 960.0f);
    const float left_base_y  = g_height * ( 75.0f / 544.0f);
    const float right_base_x = g_width  * (786.0f / 960.0f);
    const float right_base_y = g_height * (180.0f / 544.0f);

    float left_x  = left_base_x  + g_width  * ( 75.0f / 960.0f) * lx;
    float left_y  = left_base_y  + g_height * ( 75.0f / 544.0f) * ly;
    float right_x = right_base_x + g_width  * (155.0f / 960.0f) * rx;
    float right_y = right_base_y - g_height * (105.0f / 544.0f) * ry;

    bool left_now = lx != 0.0f || ly != 0.0f;
    bool right_now = rx != 0.0f || ry != 0.0f;

    if (left_now && !g_left_active)
        send_pointer(ID_RAW_POINTER_DOWN, MODULE_TOUCH_SCREEN, 1,
                     left_base_x, left_base_y);
    if (left_now)
        send_pointer(ID_RAW_POINTER_MOVE, MODULE_TOUCH_SCREEN, 1,
                     left_x, left_y);
    else if (g_left_active)
        send_pointer(ID_RAW_POINTER_UP, MODULE_TOUCH_SCREEN, 1,
                     left_x, left_y);
    g_left_active = left_now;

    /*
     * The original Vita bridge ends and restarts the right-stick swipe on
     * every poll. Mass Effect Infiltrator treats one long touchpad drag as a finite camera
     * gesture; repeatedly pulsing UP/DOWN/MOVE makes a held stick rotate
     * continuously. Keep the left movement stick as one sustained drag.
     */
    if (g_right_active)
        send_pointer(ID_RAW_POINTER_UP, MODULE_TOUCH_PAD, 2,
                     g_right_last_x, g_right_last_y);
    if (right_now) {
        if (!g_right_active) {
            g_right_pulses = 0;
            trace("input: right-stick camera gesture started");
        }
        send_pointer(ID_RAW_POINTER_DOWN, MODULE_TOUCH_PAD, 2,
                     right_base_x, right_base_y);
        send_pointer(ID_RAW_POINTER_MOVE, MODULE_TOUCH_PAD, 2,
                     right_x, right_y);
        g_right_last_x = right_x;
        g_right_last_y = right_y;
        g_right_pulses++;
        if (g_right_pulses == 30)
            trace("input: right-stick camera gesture remains continuous "
                  "(30 frame pulses)");
    } else if (g_right_active) {
        trace("input: right-stick camera gesture ended after %u frame pulses",
              g_right_pulses);
    }
    g_right_active = right_now;
}

static void open_controller(int index)
{
    if (!SDL_IsGameController(index))
        return;
    for (SDL_GameController *controller : g_controllers)
        if (controller &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) ==
                SDL_JoystickGetDeviceInstanceID(index))
            return;
    for (SDL_GameController *&slot : g_controllers) {
        if (!slot) {
            slot = SDL_GameControllerOpen(index);
            if (slot)
                trace("controller: %s", SDL_GameControllerName(slot));
            return;
        }
    }
}

void android_input_init(so_module *mod, JNIEnv *env, int width, int height)
{
    g_env = env;
    g_width = width;
    g_height = height;
    g_key_down = (KeyFn)so_symbol(
        mod, "Java_com_ea_blast_KeyboardAndroid_NativeOnKeyDown");
    g_key_up = (KeyFn)so_symbol(
        mod, "Java_com_ea_blast_KeyboardAndroid_NativeOnKeyUp");
    g_pointer = (PointerFn)so_symbol(
        mod, "Java_com_ea_blast_TouchSurfaceAndroid_NativeOnPointerEvent");
    g_acceleration = (AccelFn)so_symbol(
        mod, "Java_com_ea_blast_AccelerometerAndroidDelegate_NativeOnAcceleration");
    g_motion = (MotionFn)so_symbol(
        mod,
        "Java_com_ea_games_meinfiltrator_1gamepad_MassEffectActivity_"
        "NativeDispatchGenericMotionEvent");

    /* Read once here so which mode the run was in is in the log from the start,
     * rather than only from the first time something tries to raise the cursor
     * - a run where the menus never navigate is exactly the run where that
     * never happens. */
    cursor_enabled();

    /*
     * Escape hatch for the device tests: MASSEFFECT_STICK_TOUCH=1 puts the
     * sticks back on the old virtual-touchscreen drags, so the two routings can
     * be compared on the same build instead of on two trips.
     */
    const char *stick_env = getenv("MASSEFFECT_STICK_TOUCH");
    g_stick_touch = stick_env && *stick_env && strcmp(stick_env, "0") != 0;
    const char *motion_trace_env = getenv("MASSEFFECT_MOTION_TRACE_ALL");
    g_motion_trace_all = motion_trace_env && *motion_trace_env &&
                         strcmp(motion_trace_env, "0") != 0;
    const char *gate_env = getenv("MASSEFFECT_MOTION_NO_GATE");
    g_motion_no_gate = gate_env && *gate_env && strcmp(gate_env, "0") != 0;
    const char *swipe_env = getenv("MASSEFFECT_CAMERA_NO_SWIPE");
    g_camera_no_swipe = swipe_env && *swipe_env && strcmp(swipe_env, "0") != 0;
    const char *accel_env = getenv("MASSEFFECT_TRIGGER_ACCEL");
    g_trigger_accel = accel_env && *accel_env && strcmp(accel_env, "0") != 0;
    /* Opt-in, not opt-out: the default is L2 = cover. */
    const char *aim_env = getenv("MASSEFFECT_AIM_TAP");
    if (aim_env && *aim_env && strcmp(aim_env, "0") != 0)
        g_aim_tap = true;
    const char *aim_hold_env = getenv("MASSEFFECT_AIM_HOLD");
    if (aim_hold_env && *aim_hold_env && strcmp(aim_hold_env, "0") == 0)
        g_aim_hold = false;
    /*
     * Escape hatch for the device test: the aim tap lands on the crosshair,
     * which Hud::GetCrosshairPos puts at the exact centre. If that turns out to
     * miss, these move it without a rebuild.
     */
    const char *aim_x_env = getenv("MASSEFFECT_AIM_X");
    const char *aim_y_env = getenv("MASSEFFECT_AIM_Y");
    if (aim_x_env && *aim_x_env)
        g_aim_x = (float)atof(aim_x_env);
    if (aim_y_env && *aim_y_env)
        g_aim_y = (float)atof(aim_y_env);
    const char *aim_ms_env = getenv("MASSEFFECT_AIM_TAP_MS");
    if (aim_ms_env && *aim_ms_env) {
        int ms = atoi(aim_ms_env);
        /* Over 500 ms the engine stops calling it a single tap. */
        if (ms > 0 && ms < 500)
            g_aim_tap_ms = (Uint32)ms;
        else
            warning("input: MASSEFFECT_AIM_TAP_MS=%s ignored; keep it under "
                    "500, the engine's single-tap limit\n", aim_ms_env);
    }

    /*
     * Read back the two settings that decide whether the synthetic tap means
     * anything: tap_to_aim_enabled picks the aiming scheme and swapped_input
     * mirrors the halves, so a mirrored layout would need the exit tap on the
     * other side. Both offsets come from Settings::LoadSettings (+0x23c8e4 and
     * neighbours), matching each config string to the strb that follows it.
     *
     * Off by default and behind MASSEFFECT_AIM_DIAG=1 because it means calling
     * Settings::GetInstance() by fixed offset, and a GetInstance on this engine
     * can construct its singleton on first call - that is exactly how the very
     * first stick dispatch built a second engine and crashed. Diagnostics are
     * not worth that risk on every boot.
     */
    const char *aim_diag = getenv("MASSEFFECT_AIM_DIAG");
    if (aim_diag && *aim_diag && strcmp(aim_diag, "0") != 0) {
        using SettingsFn = void *(*)(void);
        SettingsFn get_settings = (SettingsFn)(mod->text_base + 0x23fffc);
        const unsigned char *settings = (const unsigned char *)get_settings();
        if (settings)
            trace("input: settings tap_to_aim_enabled=%u swapped_input=%u "
                  "horizontal_input=%u facing_invert_y=%u",
                  settings[101], settings[98], settings[97], settings[96]);
        else
            warning("input: Settings::GetInstance() returned NULL\n");
    }

    g_app_guard = (volatile const uint32_t *)(mod->text_base + 0xa98c48 + 0xec);
    trace("input: engine Application at +0x00a98c48 "
          "(first-time flag=%u guard=0x%08x instance=%p)%s",
          (unsigned int)*(volatile const uint8_t *)(mod->text_base + 0xa98c48 + 0xe8),
          (unsigned int)*g_app_guard,
          *(void *volatile const *)(mod->text_base + 0xa98c48 + 0xfc),
          g_motion_no_gate ? " gate=off" : "");

    if (!g_motion)
        warning("input: NativeDispatchGenericMotionEvent never resolved - "
                "gameplay analog movement falls back to touchscreen drags, "
                "which this build does not read.\n");

    const char *auto_env = getenv("MASSEFFECT_AUTOPILOT");
    g_autopilot = auto_env && *auto_env && strcmp(auto_env, "0") != 0;

    /*
     * SDL names face buttons by position. R36S-class handhelds are lettered
     * Nintendo-style: their physical A (right) arrives as SDL B, while their
     * physical B (bottom) arrives as SDL A. This is measured on the same
     * device by the working sibling ports.
     */
    const char *layout = getenv("MASSEFFECT_FACE_LAYOUT");
    if (layout && strcmp(layout, "xbox") == 0) {
        g_accept_button = SDL_CONTROLLER_BUTTON_A;
        g_back_button = SDL_CONTROLLER_BUTTON_B;
    }

    int available = SDL_NumJoysticks();
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        open_controller(i);
    }
    int opened = 0;
    for (SDL_GameController *controller : g_controllers)
        if (controller)
            opened++;

    trace("input bridge: JNI keys=%s pointer=%s acceleration=%s motion=%s "
          "sticks=%s joysticks=%d controllers=%d "
          "face-layout=%s%s",
          g_key_down && g_key_up ? "yes" : "no", g_pointer ? "yes" : "no",
          g_acceleration ? "yes" : "no", g_motion ? "yes" : "no",
          (g_motion && !g_stick_touch) ? "gamepad-motion" : "touch-drag",
          available, opened,
          g_accept_button == SDL_CONTROLLER_BUTTON_B ? "Nintendo" : "Xbox",
          g_autopilot ? " autopilot=on" : "");

    cursor_show();
    trace("input: menus use d-pad cursor + physical A tap; L3/R3 toggle cursor; "
          "Start restores it");
    trace("input: combat keys A=%d(nav aid) B=%d(melee) X=%d(cloak) Y=%d(biotic) "
          "L1/L2=%d(cover/sniper aim) R1/R2=%d(fire); camera swipe ramp %s",
          AKEYCODE_BUTTON_A, AKEYCODE_BUTTON_B, AKEYCODE_B,
          AKEYCODE_Y, AKEYCODE_BUTTON_L1, AKEYCODE_BUTTON_R1,
          g_camera_no_swipe ? "off" : "on");
    if (g_aim_tap)
        trace("input: MASSEFFECT_AIM_TAP=1 - L2 = aim tap (%s, EXPERIMENTAL): "
              "tap %.0f,%.0f to aim, %.0f,%.0f to exit; id=%d, %u ms. Only does "
              "anything with Tap To Aim ON in the game's Options AND an enemy "
              "under the crosshair; with it off the tap cannot enter aim and "
              "can cancel a shot",
              g_aim_hold ? "hold" : "toggle",
              g_aim_x >= 0.0f ? g_aim_x : width * 0.5f,
              g_aim_y >= 0.0f ? g_aim_y : height * 0.5f,
              width * kRegionSplit * 0.5f, height * 0.5f,
              kAimPointerId, (unsigned int)g_aim_tap_ms);
    else
        trace("input: L2 = cover (doubles L1); aiming is the right stick. "
              "MASSEFFECT_AIM_TAP=1 restores the experimental aim tap");
    if (g_trigger_accel)
        trace("input: MASSEFFECT_TRIGGER_ACCEL=1 - L2 simulates weapon tilt, "
              "R2 simulates Zero-G motion; neither trigger fires");
}

bool android_input_event(const SDL_Event *event)
{
    if (!event)
        return true;

    switch (event->type) {
    case SDL_QUIT:
        return false;
    case SDL_CONTROLLERDEVICEADDED:
        open_controller(event->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        for (SDL_GameController *&slot : g_controllers) {
            if (!slot)
                continue;
            SDL_Joystick *stick = SDL_GameControllerGetJoystick(slot);
            if (SDL_JoystickInstanceID(stick) == event->cdevice.which) {
                SDL_GameControllerClose(slot);
                slot = NULL;
            }
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        bool down = event->type == SDL_CONTROLLERBUTTONDOWN;
        Uint8 button = event->cbutton.button;

        /* The stick-click cursor toggle only makes sense while there is a
         * cursor to toggle. With MASSEFFECT_NO_CURSOR set it must not swallow
         * the button either, or the log would report a toggle that did not
         * happen and the button could never be mapped to anything else. */
        if (cursor_enabled() &&
            (button == SDL_CONTROLLER_BUTTON_LEFTSTICK ||
             button == SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
            if (down) {
                if (g_cursor_visible)
                    cursor_hide();
                else
                    cursor_show();
            }
            trace("input: controller button=%u %s -> menu cursor toggle",
                  (unsigned int)button, down ? "down" : "up");
            break;
        }

        /*
         * Analog movement deliberately hides the menu cursor for gameplay.
         * Opening the pause menu is the reliable recovery path even on devices
         * whose stick-click buttons are not exposed by their SDL mapping.
         */
        if (button == SDL_CONTROLLER_BUTTON_START && down &&
            !g_cursor_visible)
            cursor_show();

        if (g_cursor_visible) {
            switch (button) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                g_cursor_dx = down ? -1 : (g_cursor_dx < 0 ? 0 : g_cursor_dx);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                g_cursor_dx = down ? 1 : (g_cursor_dx > 0 ? 0 : g_cursor_dx);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                g_cursor_dy = down ? -1 : (g_cursor_dy < 0 ? 0 : g_cursor_dy);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                g_cursor_dy = down ? 1 : (g_cursor_dy > 0 ? 0 : g_cursor_dy);
                break;
            default:
                break;
            }
            if (button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
                button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                trace("input: controller button=%u %s -> menu cursor",
                      (unsigned int)button, down ? "down" : "up");
                break;
            }
            if (button == g_accept_button) {
                cursor_tap(down);
                break;
            }
        }

        int code = map_button(event->cbutton.button);
        trace("input: controller button=%u %s -> Android key=%d",
              (unsigned int)event->cbutton.button,
              down ? "down" : "up", code);
        if (code)
            send_key(code, down);
        break;
    }
    case SDL_CONTROLLERAXISMOTION:
        {
        static int axis_lines = 0;
        if (axis_lines < 24 && abs((int)event->caxis.value) > 4000) {
            trace("input: controller axis=%u value=%d",
                  (unsigned int)event->caxis.axis,
                  (int)event->caxis.value);
            axis_lines++;
        }
        switch (event->caxis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:  g_lx = event->caxis.value; break;
        case SDL_CONTROLLER_AXIS_LEFTY:  g_ly = event->caxis.value; break;
        case SDL_CONTROLLER_AXIS_RIGHTX: g_rx = event->caxis.value; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: g_ry = event->caxis.value; break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            trigger_changed(true, event->caxis.value);
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            trigger_changed(false, event->caxis.value);
            break;
        default: return true;
        }
        break;
        }
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (event->key.keysym.sym == SDLK_ESCAPE)
            send_key(AKEYCODE_BACK, event->type == SDL_KEYDOWN);
        else if (event->key.keysym.sym == SDLK_RETURN)
            send_key(AKEYCODE_DPAD_CENTER, event->type == SDL_KEYDOWN);
        break;
    default:
        break;
    }
    return true;
}

void android_input_autopilot_tick(long frame)
{
    if (!g_autopilot || !g_key_down || !g_key_up)
        return;

    const long first = 90;
    const long period = 45;
    if (frame < first)
        return;

    long phase = (frame - first) % period;
    long index = (frame - first) / period;
    int code = kAutopilotKeys[index %
        (sizeof(kAutopilotKeys) / sizeof(kAutopilotKeys[0]))];

    if (phase == 0) {
        send_key(code, true);
        g_auto_keys++;
        g_pending_key_frame = frame;
        g_pending_key_serial++;
        trace("autopilot: key down %d at frame %ld", code, frame);
    } else if (phase == 3) {
        send_key(code, false);
    }
}

/*
 * A compact perceptual signature: 32 horizontal buckets over the middle row,
 * RGB averaged in each. It is deliberately coarser than a pixel hash so menu
 * animation and tiny cursor movement do not become fake scene transitions.
 */
static bool sample_signature(unsigned char signature[32 * 3])
{
    using ReadPixels = void (*)(int, int, int, int, unsigned int, unsigned int,
                                void *);
    static ReadPixels read_pixels =
        (ReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    if (!read_pixels)
        return false;

    static unsigned char *row = NULL;
    static size_t row_size = 0;
    size_t need = (size_t)g_width * 4;
    if (need > row_size) {
        unsigned char *next = (unsigned char *)realloc(row, need);
        if (!next)
            return false;
        row = next;
        row_size = need;
    }

    read_pixels(0, g_height / 2, g_width, 1, 0x1908 /* GL_RGBA */,
                0x1401 /* GL_UNSIGNED_BYTE */, row);

    for (int bucket = 0; bucket < 32; bucket++) {
        int begin = bucket * g_width / 32;
        int end = (bucket + 1) * g_width / 32;
        unsigned int sum[3] = {};
        for (int x = begin; x < end; x++) {
            sum[0] += row[x * 4 + 0];
            sum[1] += row[x * 4 + 1];
            sum[2] += row[x * 4 + 2];
        }
        int pixels = std::max(1, end - begin);
        signature[bucket * 3 + 0] = (unsigned char)(sum[0] / pixels);
        signature[bucket * 3 + 1] = (unsigned char)(sum[1] / pixels);
        signature[bucket * 3 + 2] = (unsigned char)(sum[2] / pixels);
    }
    return true;
}

void android_input_autopilot_sample(long frame)
{
    if (!g_autopilot || frame % 15 != 0)
        return;

    static unsigned char latest[32 * 3] = {};
    static unsigned char baseline[32 * 3] = {};
    static bool have_latest = false;
    static int baseline_serial = 0;

    unsigned char current[32 * 3];
    if (!sample_signature(current))
        return;

    if (g_pending_key_serial != baseline_serial && have_latest) {
        memcpy(baseline, latest, sizeof(baseline));
        baseline_serial = g_pending_key_serial;
    }

    if (baseline_serial != 0 &&
        baseline_serial != g_counted_key_serial &&
        frame >= g_pending_key_frame + 10 &&
        frame <= g_pending_key_frame + 44) {
        long difference = 0;
        for (size_t i = 0; i < sizeof(current); i++)
            difference += abs((int)current[i] - (int)baseline[i]);
        long average = difference / (long)sizeof(current);
        if (average >= 18) {
            g_auto_scenes++;
            g_counted_key_serial = baseline_serial;
            trace("autopilot: scene change %ld after key %d "
                  "(mean strip delta=%ld)",
                  g_auto_scenes, baseline_serial, average);
        }
    }

    memcpy(latest, current, sizeof(latest));
    have_latest = true;
}

long android_input_autopilot_keys(void) { return g_auto_keys; }
long android_input_autopilot_scenes(void) { return g_auto_scenes; }
