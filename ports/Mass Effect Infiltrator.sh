#!/bin/bash
# PORTMASTER: masseffect-portmaster.zip, Mass Effect Infiltrator.sh
#
# Mass Effect Infiltrator (Android gamepad build 1.0.58) — PortMaster launcher.
# Port and project by EapRules: https://github.com/EapRules
#
# The port never ships EA's files. The user's extracted game tree lives next
# to the loader and must contain:
#
#   assets/EAMCore.ini
#   assets/published/
#   lib/armeabi/libMassEffect.so
#
# This is a Java-driven JNI game, not NativeActivity and not the unrelated
# Mountain Sheep/OUYA game an early scaffold for this directory described.

# shellcheck disable=SC1090,SC1091,SC2154

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"

export PORT_32BIT="Y"
[ -f "$controlfolder/tasksetter" ]          && source "$controlfolder/tasksetter"
[ -f "$controlfolder/device_info.txt" ]     && source "$controlfolder/device_info.txt"
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR="/$directory/ports/masseffect"
cd "$GAMEDIR" || exit 1

: > "$GAMEDIR/log.txt"
exec > "$GAMEDIR/log.txt" 2>&1

# PortMaster's portable metadata points at masseffect/cover.png, which is the
# canonical source shipped in the release. ArkOS/dArkOS additionally keeps a
# normalized EmulationStation copy beside the other port artwork. A direct
# update does not rerun PortMaster's metadata importer, so that copy can remain
# an old APK icon indefinitely. Refresh only Mass Effect Infiltrator's own image when its
# bytes differ; never rewrite gamelist.xml or touch another port's metadata.
ES_PORT_IMAGE="/$directory/ports/images/Mass Effect Infiltrator.png"
if [ -f "$GAMEDIR/cover.png" ] && \
   { [ ! -f "$ES_PORT_IMAGE" ] || ! cmp -s "$GAMEDIR/cover.png" "$ES_PORT_IMAGE"; }; then
  mkdir -p "$(dirname "$ES_PORT_IMAGE")"
  if cp -f "$GAMEDIR/cover.png" "$ES_PORT_IMAGE"; then
    echo "Port artwork normalized at $ES_PORT_IMAGE (visible after frontend restart)"
  else
    echo "Warning: could not refresh $ES_PORT_IMAGE; continuing without artwork update"
  fi
fi

export LD_LIBRARY_PATH="$GAMEDIR/libs.armhf${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export MASSEFFECT_FACE_LAYOUT="${MASSEFFECT_FACE_LAYOUT:-nintendo}"
# The menu navigates on the d-pad directly, so the painted cursor is off by
# default (it was the only thing drawing behind the engine - the Mali trail).
# Set MASSEFFECT_NO_CURSOR=0 to bring it back if a menu ever needs the pointer.
export MASSEFFECT_NO_CURSOR="${MASSEFFECT_NO_CURSOR:-1}"
# Aspect-correct scaling for panels that are not the R36S's 640x480 (e.g. the
# RG34XXSP's 720x480): fit (default) keeps 4:3 and letterboxes, stretch fills
# the panel, integer scales by a whole number. The loader auto-detects the
# panel from the GL drawable; on a CFW that reports the wrong size, set
# MASSEFFECT_PANEL_W / MASSEFFECT_PANEL_H here to force it. On a real 640x480
# panel this is identity - nothing is scaled.
export MASSEFFECT_SCALE="${MASSEFFECT_SCALE:-fit}"
export LOADER_TRACE=1
# Audio routing is decided by what the device actually runs, never by CFW name -
# the same principle as the display scaling above: detect the capability, adapt
# to it. If a user audio server is present (PipeWire, or a PulseAudio socket),
# the 32-bit game must route through it or it grabs a PCM nobody is listening to
# and plays silence. If none is found, fall back to ALSA dmix, which is what a
# bare-ALSA CFW (the R36S on ArkOS) provides. No device or firmware is named.
_MASSEFFECT_PW=""
for _pw in /usr/lib32/pipewire-0.3 /usr/lib/arm-linux-gnueabihf/pipewire-0.3; do
  [ -d "$_pw" ] && { _MASSEFFECT_PW="$_pw"; break; }
done
for _xrd in "${XDG_RUNTIME_DIR:-}" /run/user/0 /var/run/user/0; do
  [ -n "$_xrd" ] && [ -d "$_xrd" ] && { export XDG_RUNTIME_DIR="$_xrd"; break; }
done
_MASSEFFECT_PULSE=""
for _pulse in "${XDG_RUNTIME_DIR:-}/pulse/native" /run/pulse/native /var/run/pulse/native; do
  [ -n "$_pulse" ] && [ -S "$_pulse" ] && { _MASSEFFECT_PULSE="$_pulse"; break; }
done
if [ -n "$_MASSEFFECT_PW" ] || [ -n "$_MASSEFFECT_PULSE" ]; then
  # A running audio server: route through it, never grab the PCM exclusively.
  unset AUDIODEV ALSA_CONFIG_PATH SDL_AUDIO_DEVICE_NAME ALSA_CARD
  export SDL_AUDIODRIVER=alsa
  export ALSOFT_DRIVERS=alsa
  export SDL_AUDIO_ALSA_SET_BUFFER_SIZE=1
  for _spa in /usr/lib32/spa-0.2 /usr/lib/arm-linux-gnueabihf/spa-0.2; do
    [ -d "$_spa" ] && { export SPA_PLUGIN_DIR="$_spa"; break; }
  done
  [ -n "$_MASSEFFECT_PW" ] && export PIPEWIRE_MODULE_DIR="$_MASSEFFECT_PW"
  if [ -n "$_MASSEFFECT_PULSE" ]; then
    export PULSE_SERVER="unix:$_MASSEFFECT_PULSE"
  else
    unset PULSE_SERVER
  fi
  echo "Audio: routing through the device's audio server (PipeWire/Pulse), dmix bypassed"
else
  export AUDIODEV="${AUDIODEV:-plug:dmix}"
  export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
  echo "Audio: ALSA dmix (no audio server detected)"
fi

CUR_TTY=/dev/tty0
[ -w "$CUR_TTY" ] || CUR_TTY=/dev/tty1

show_screen() {
  $ESUDO chmod 666 "$CUR_TTY" 2>/dev/null
  printf "\033c" > "$CUR_TTY"
  cat > "$CUR_TTY"
  sleep "${1:-10}"
  printf "\033c" > "$CUR_TTY"
}

GAME_SO="$GAMEDIR/lib/armeabi/libMassEffect.so"

# A release user should not have to unpack an Android package by hand. eapx
# discovers APK/ZIP/folder donors by their contents, stages the complete game
# tree away from the live install, validates the exact native library and only
# then publishes assets/ and lib/. Existing manually-extracted installs skip
# this path and continue to work exactly as before.
if [ ! -f "$GAME_SO" ] || [ ! -f "$GAMEDIR/assets/EAMCore.ini" ] \
   || [ ! -d "$GAMEDIR/assets/published" ]; then
  if ! command -v python3 >/dev/null 2>&1; then
    echo "Game-data import failed: python3 is unavailable"
    show_screen 12 <<EOF

  Mass Effect Infiltrator - Python 3 missing

  Automatic game-data import needs
  Python 3 from the CFW.

  Update PortMaster/your firmware, or
  extract the donor on a computer into:
    ports/masseffect/

EOF
    pm_finish
    exit 1
  fi

  if [ ! -f "$GAMEDIR/eapx.py" ] || [ ! -f "$GAMEDIR/masseffect.eapx.json" ]; then
    echo "Game-data import failed: eapx runtime or recipe is missing"
    show_screen 12 <<EOF

  Mass Effect Infiltrator - incomplete port

  eapx.py or masseffect.eapx.json
  is missing. Reinstall the release ZIP
  through PortMaster/autoinstall.

EOF
    pm_finish
    exit 1
  fi

  echo "Game data is absent; starting content-based first-boot import"
  if ! python3 "$GAMEDIR/eapx.py" install \
       --recipe "$GAMEDIR/masseffect.eapx.json" \
       --game-dir "$GAMEDIR" --tty "$CUR_TTY"; then
    echo "Game-data import failed; see $GAMEDIR/eapx.log"
    show_screen 15 <<EOF

  Mass Effect Infiltrator - game data not ready

  Put your own v1.0.58 gamepad build
  APK, ZIP, or extracted folder in:
    ports/masseffect/

  The filename does not matter.
  See README.md and eapx.log.

EOF
    pm_finish
    exit 1
  fi
fi

if [ ! -f "$GAME_SO" ] || [ ! -f "$GAMEDIR/assets/EAMCore.ini" ] \
   || [ ! -d "$GAMEDIR/assets/published" ]; then
  echo "Game-data check failed: extracted game files are incomplete."
  show_screen 12 <<EOF

  Mass Effect Infiltrator - missing game data

  Put your APK, ZIP or extracted game in:
    ports/masseffect/

  Required:
    assets/EAMCore.ini
    assets/published/
    lib/armeabi/
      libMassEffect.so

  Required build: v1.0.58 gamepad build

EOF
  pm_finish
  exit 1
fi

rm -f "$GAMEDIR/PUT_MASS_EFFECT_DATA_HERE.txt"

# The loader patches fixed offsets, so a genuinely different build would crash
# deep inside startup. eapx install already validates the native library by
# critical_regions against the recipe - the exact bytes the loader patches - so
# any legitimate v1.0.58 copy (regional variant, modder NOP, resigned APK) is
# accepted and a truly wrong build is rejected before we get here. An exact sha1
# would instead reject those legitimate copies, breaking bring-your-own-game.
# A cheap size sanity-check stays, to catch a manually dropped file of the wrong
# game without re-enforcing a per-distribution hash.
EXPECTED_SIZE=16952541
GAME_SIZE=$(stat -c%s "$GAME_SO" 2>/dev/null || stat -f%z "$GAME_SO" 2>/dev/null)
if [ -n "$GAME_SIZE" ] && [ "$GAME_SIZE" != "$EXPECTED_SIZE" ]; then
  echo "Game-data check failed: size=$GAME_SIZE expected=$EXPECTED_SIZE"
  show_screen 12 <<EOF

  Mass Effect Infiltrator - unsupported game build

  This port needs the v1.0.58 gamepad
  build native library.

  Found size:
    $GAME_SIZE

EOF
  pm_finish
  exit 1
fi

# This build ships its language tables reversed: the Russian text is under
# strings/ENG_US and the English text under strings/RUS_RU. The engine defaults
# to ENG_US (it has no Russian in its language list at all), so the game shows
# Russian. Swap the two tables once, guarded by content so a correctly-laid
# donor is never touched and the swap is idempotent: English "Mattock" belongs
# in ENG_US, so if it is missing there but present in RUS_RU the tables are
# reversed. This fixes text for any copy with the same defect and leaves others
# untouched.
STRINGS_DIR="$GAMEDIR/assets/published/strings"
ENG_BIN="$STRINGS_DIR/ENG_US/masseffect.bin"
RUS_BIN="$STRINGS_DIR/RUS_RU/masseffect.bin"
if [ -f "$ENG_BIN" ] && [ -f "$RUS_BIN" ] \
   && ! grep -qa "Mattock" "$ENG_BIN" && grep -qa "Mattock" "$RUS_BIN"; then
  SWAP_TMP="$STRINGS_DIR/.masseffect.bin.swap"
  if mv "$ENG_BIN" "$SWAP_TMP" && mv "$RUS_BIN" "$ENG_BIN" \
     && mv "$SWAP_TMP" "$RUS_BIN"; then
    sync
    echo "Language: fixed this build's reversed ENG_US/RUS_RU string tables"
  else
    rm -f "$SWAP_TMP"
    echo "Language: could not swap the reversed string tables; text may be Russian"
  fi
fi

# SDL must create its context through the 32-bit Mali blob. Build symlinks in
# /tmp because the SD card may be exFAT and cannot preserve symlinks.
# The Mali driver is a single "unified" blob - one .so exports EGL, GLESv1 and
# GLESv2 - so once found it is symlinked to every name the game asks for. Try
# the exact R36S filename first (fast, tested), then any Mali build in the
# standard 32-bit library directories (valhall-g52, midgard, an H700's own
# bifrost...), so other Mali handhelds work without naming their SoC here.
MALI_BLOB=""
for candidate in \
  /usr/lib/arm-linux-gnueabihf/libmali-bifrost-g31-rxp0-gbm.so \
  /usr/lib/arm-linux-gnueabihf/libMali.so \
  /usr/lib/arm-linux-gnueabihf/libmali.so.1; do
  [ -e "$candidate" ] && { MALI_BLOB="$candidate"; break; }
done
if [ -z "$MALI_BLOB" ]; then
  for _gldir in \
    /usr/lib/arm-linux-gnueabihf \
    /usr/lib/arm-linux-gnueabihf/mali \
    /usr/lib32 \
    /lib/arm-linux-gnueabihf; do
    [ -d "$_gldir" ] || continue
    for _cand in "$_gldir"/libmali*.so* "$_gldir"/libMali.so*; do
      [ -e "$_cand" ] && { MALI_BLOB="$_cand"; break; }
    done
    [ -n "$MALI_BLOB" ] && break
  done
fi

if [ -n "$MALI_BLOB" ]; then
  GL_SHIM="/tmp/masseffect-gl"
  rm -rf "$GL_SHIM"
  if mkdir -p "$GL_SHIM" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libEGL.so.1" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libGLESv1_CM.so.1" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libGLESv2.so.2" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libmali.so.1"; then
    export LD_LIBRARY_PATH="$GL_SHIM:$LD_LIBRARY_PATH"
    echo "GL: using Mali blob $MALI_BLOB"
  else
    echo "GL: failed to create /tmp shim, using system libraries"
  fi
else
  echo "GL: no compatible 32-bit Mali blob found"
fi

mkdir -p "$GAMEDIR/var"
$ESUDO chmod +x "$GAMEDIR/masseffect"

# Controls are delivered directly through the game's JNI key/pointer exports.
# gptokeyb remains only for PortMaster's standard exit combination.
$GPTOKEYB "masseffect" -c "$GAMEDIR/masseffect.gptk" &

if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$GAMEDIR/masseffect"
fi

$TASKSET "$GAMEDIR/masseffect" "$GAMEDIR"
GAME_RC=$?

$ESUDO kill -9 "$(pidof gptokeyb)" 2>/dev/null
rm -rf /tmp/masseffect-gl
unset LD_LIBRARY_PATH SDL_GAMECONTROLLERCONFIG

pm_finish
exit "$GAME_RC"
