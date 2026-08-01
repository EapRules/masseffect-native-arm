# Mass Effect Infiltrator — native PortMaster port

Runs the 2011 **Mass Effect Infiltrator** game by IronMonkey Studios / EA
directly on Linux/ARM handhelds. No Android runtime and no emulator: the
original ARM game library is loaded through a bionic/JNI compatibility layer.

**Port and project by [EapRules](https://github.com/EapRules).**

> **Bring your own game.** The release contains no EA game binary or asset.
> Supply a copy you own; nothing is downloaded and no protection is bypassed.

## Required game version

The supported donor is the **v1.0.58 gamepad build**:

```text
Android package: com.ea.games.meinfiltrator_gamepad
Library:         lib/armeabi/libMassEffect.so
Library SHA1:    ea58b733d3d267ab639431b50539542faa43f0d0
```

The filename and store label are not trusted. The first-boot importer accepts a
donor only when its native library and required campaign/UI files match this
build. Validation checks the exact bytes the loader patches (critical regions),
not a single whole-file hash, so regional variants, modder NOPs and re-signed
copies are accepted while a genuinely different build is rejected.

## Install with PortMaster autoinstall

Do not unzip the port into `ports/` by hand. Let PortMaster install it so the
executable bit and the EmulationStation entry are created correctly.

1. Put `masseffect-portmaster.zip` in PortMaster's `autoinstall/` directory,
   without renaming it.

   | CFW | Autoinstall directory |
   |---|---|
   | ArkOS, dArkOS | `/roms/tools/PortMaster/autoinstall/` |
   | AmberELEC, ROCKNIX, JELOS | `/roms/ports/PortMaster/autoinstall/` |
   | muOS | `/mmc/MUOS/PortMaster/autoinstall/` |
   | Knulli | `/userdata/system/.local/share/PortMaster/autoinstall/` |

2. Put your donor in `ports/masseffect/`. It may be the game's APK plus its
   cache/OBB zip, a single ZIP, or an already-extracted folder. The filename
   does not matter.

3. Open PortMaster. It installs the release from `autoinstall/` and adds the
   menu metadata. Wait for the exact **Finished running autoinstall** dialog,
   acknowledge it, and let PortMaster return or close normally. Do not power off
   while its file list is still visible.

4. **Reboot through the firmware menu.** Autoinstall does not refresh the Ports
   list EmulationStation loaded at boot, so the game will not appear until then.

5. Launch Mass Effect Infiltrator from Ports. The first launch discovers the
   donor by content, extracts roughly 750 MiB, validates every required output
   and publishes it atomically. Keep at least 800 MiB free and do not power off
   during this first extraction. Later launches start normally, and the original
   donor file is no longer required.

The importer accepts these donor layouts without rearranging them:

```text
lib/armeabi/libMassEffect.so + assets/...
lib/armeabi/libMassEffect.so + published/...
lib/armeabi/libMassEffect.so + com.ea.games.meinfiltrator_gamepad/published/...
Android/data/com.ea.games.meinfiltrator_gamepad/published/...
```

### Two autoinstall notes

- The game does not appear in Ports until the console is rebooted.
- Do not use **Reinstall** or **Uninstall** under Manage Ports. This independent
  release is not in PortMaster's catalogue, so those actions try to download a
  source that does not exist and may remove the installed folder, including your
  donor and extracted data. To update, put the new release in `autoinstall/`
  again.

## Device support

The loader is device-generic: it adapts by capability, not by device name.

- **glibc:** built against an old glibc, so it runs on ArkOS/AeolusUX and older
  firmwares as well as current ones. No downloaded runtime is required.
- **Display:** the engine renders at 640x480; the loader detects the real panel
  and maps that output onto it. `MASSEFFECT_SCALE=fit` (default, letterbox),
  `stretch` or `integer`. `MASSEFFECT_PANEL_W`/`H` force a size if a CFW reports
  the wrong drawable.
- **Audio:** routes through the device's audio server (PipeWire/PulseAudio) when
  present, otherwise ALSA dmix.
- **GPU:** requires armhf execution and a 32-bit Mali GPU userspace, like
  box86/GMLoader ports. The Mali blob is found generically.

Tested on an R36S (RK3326 / Mali-G31) with dArkOSRE at 640x480. Reports from
other devices and firmwares are welcome.

## Controls

| Control | Action |
|---|---|
| Left stick | Move |
| Right stick | Aim / camera (continuous) |
| R1 / R2 | Fire |
| L1 / L2 | Cover |
| X | Cloak |
| Y | Biotic power |
| B | Melee / interact |
| A | Action / confirm |
| D-pad | Menu navigation |
| Start | Pause |

## Troubleshooting

Each launch replaces `ports/masseffect/log.txt`. First-boot extraction writes
`ports/masseffect/eapx.log`. If installation fails, include both files plus the
device and CFW when reporting.

Common causes:

- `no game package was found`: donor is not inside `ports/masseffect/`.
- `no input matches this recipe`: wrong build or incomplete archive.
- native library rejected: the `.so` is not the v1.0.58 gamepad build.
- `GL: no compatible 32-bit Mali blob found`: the firmware lacks the armhf GPU
  userspace this port needs.

## Credits and licence

Game by **IronMonkey Studios**, published by **Electronic Arts**. Port by
**EapRules**.

The bionic ELF loader and JNI/libc compatibility work derive from
[gmloader-next](https://github.com/JohnnyonFlame/gmloader-next), based on Andy
Nguyen's Vita so-loader. PVRTC decoding comes from Imagination Technologies'
PowerVR SDK; the ARMv8 short-vector expansion is adapted from Bythos14's
VFPVector; trigger-driven accelerometer samples are adapted from the
MIT-licensed masseffect-vita port by v-atamanenko.

The port is GPL-3.0. Notices and the copyright terms for every bundled shared
library ship under `licenses/`; full attribution is in `CREDITS.md`.
