# Quest games on this PC (AXRB)

FramePort's PC target ("This PC") already runs Oculus Rift games through Revive. For Quest/Android games it uses
**AXRB** (Android XR Bridge, github.com/Android-XR-Bridge/AXRB): an Android 16 emulator (x86_64 Google APIs image,
WHPX) that runs arm64 games through Android's ARM translation (`libndk_translation`), with AXRB's own OpenXR runtime
inside Android and a Windows host process (`axrb-host-bridge.exe`) that opens a real OpenXR session on the PC's
runtime (SteamVR, Virtual Desktop, ...). FramePort only drives it: it downloads AXRB's official release when a user
first installs a Quest game on the PC, never bundles or modifies it (AXRB's host/clock code is under the AXRB
Source-Available License 1.0, which allows running it and interoperating through its public interfaces).

Status: **experimental**. AXRB is young (v1.0.4, 2026-10-06) and many games still fail in it; Steam Frame
controllers don't work in AXRB yet (AXRB issue #20).

## Two backends: the emulator (1.0.4) and direct mode (2.x, "AXRB-Emulatorless")

AXRB's main branch (2026-10-15, launcher 2.0.0) became **AXRB-Emulatorless**: a separate program (own installer,
app id `org.axrb.emulatorless`, `%LOCALAPPDATA%\Programs\axrb-emulatorless-launcher`, runtime
`%LOCALAPPDATA%\AXRB-Emulatorless Runtime`, profile `%APPDATA%\AXRB-Emulatorless`) whose **direct mode** runs the
game without an emulator: `axrb-direct.exe` translates the game's arm64 code (Dynarmic JIT; Digitalis optional) and
runs the real AOSP ART + framework from Google's Android 16 arm64 system image, with its own Linux syscall/Binder
layer; Vulkan goes to the Windows driver, frames and tracking through the same `axrb-host-bridge.exe`. **Vulkan only:**
its EGL is a stub, there is no GLES renderer. Both programs can be installed side by side; FramePort keeps one
`axrb.Install` record per backend (`backend`, `version`, app folder, AXRB's root = `resources\runtime`, runtime
folder) and picks per game:

- **Choice** (`targets/pc_android.choose_backend`): `--axrb-backend direct|emulator` on `frameport install --to pc`,
  else the recipe's `pc.axrb_backend` (device category, `on_pc`: part of the PC build's selection, no-op on the
  Frame), else **direct for `graphics` Vulkan, emulator for GLES / unknown**. `deployment.json` records `backend`
  and the launcher `flags`; installs from before have none (= emulator). Switching leaves the other backend's copy
  and saves alone.
- **Per-game options** (`pc.axrb_backend` params → `run_direct_game.ps1` switches): `translator` (`dynarmic` =
  AXRB's default, `digitalis`), `precompose` (`-PrecomposeProjectionLayers`, also for the emulator's
  `run_windows_game.ps1`; also taken from AXRB's own `core/game-compatibility.json` profile for the game, e.g. The
  Climb 2), `legacy_oculus_driver` / `quest_identity` / `controllers_only` (for unconverted store APKs with older
  Oculus loaders; off by default and never taken from AXRB's profiles: FramePort always hands AXRB an OVRPort build;
  `controllers_only` only works with the legacy driver, as `prepare_art_session.py` requires).
- **Install/upgrade** (`tools/axrb.install`): `releases/latest` when it has the backend's installer
  (`AXRB-Setup-*.exe` / `AXRB-Emulatorless-Setup-*.exe`, SHA256SUMS checked, `/S`), else the known-good release
  (`KNOWN_GOOD = {"emulator": "1.0.4"}`, `releases/tags/v1.0.4`). An install older than the known-good one is
  upgraded on use; `frameport pc install-axrb --backend … --update` upgrades to the release on offer. No 2.x release
  is published yet: **`FRAMEPORT_AXRB_DIRECT_DIR`** names a local build (AXRB's source tree after
  `scripts\build\build_direct.ps1` + the host build, its `out\direct`, or a folder laid out like the installed
  `resources\runtime`): it needs the files of AXRB's `core/direct-runtime-files.cjs` (`out\direct\axrb-direct.exe`,
  `art\axrb-bootstrap.dex`, `art\lib64\*`, `guest\lib64\*`, `guest\axrb-systemdriver.apk`) plus
  `scripts\run\run_direct_game.ps1`, `scripts\run\stop_game.ps1`, `scripts\paths.ps1`,
  `direct\tools\make_sysroot.py`, `direct\tools\prepare_art_session.py`, `out\host\bin\Release\axrb-host-bridge.exe`
  (+ `openxr_loader.dll`, `out\gpu\Release` for the GPU-share layer) and ideally `tools\python` (else Python from
  PATH). A local build is set up but never replaced. `FRAMEPORT_AXRB_DIRECT_RUNTIME` overrides the runtime folder
  (default: the launcher's `library.json` `settings.managedDirectory`, else `%LOCALAPPDATA%\AXRB-Emulatorless Runtime`).
- **Direct setup** (`axrb.setup_direct`, what `core/direct.mjs ensureDirectSystem` does): the pinned image
  (`arm64-v8a-36_r02.zip`, 810,736,863 bytes, SHA-1 `62ad6714…`; values read from the installed `core/direct.mjs`,
  only `dl.google.com`) into its own empty folder, AXRB's `make_sysroot.py <zip> <runtime>\direct\sysroot.new --art`
  with AXRB's embedded Python, receipt `.axrb-sysroot.json` `{"digest","format"}`, rename to `sysroot`, archive
  deleted; `license-acceptance.json` like AXRB's DirectSetup. Ready = receipt matches + the key files exist and
  aren't empty + AXRB's direct files complete. Requirements: ≥ 6 GB RAM, `vulkan-1.dll` (no hypervisor, no GPU
  vendor gate).
- **Direct install** (no adb): the PC build is copied to `%LOCALAPPDATA%\FramePort\axrb\games\<pkg>\<pkg>.apk`
  (FramePort removes its converted copies after an install), the launch activity comes from the APK's manifest
  (`axml.launcher_activity`, `pkg/fully.qualified.Class`), the data files become AXRB's content list `assets.txt`
  (`Android/obb/<pkg>/<file>\t<Windows path>`, read-only mounts at the original place; WSL-only folders are copied
  next to the APK). App data and saves: `<runtime>\direct\data\data\<pkg>` and `direct\sdcard\Android\data\<pkg>`.
- **Play**: Steam shortcut → `%LOCALAPPDATA%\FramePort\axrb\fp-axrb-direct.ps1 -Package … -Activity … [flags]` →
  sets `AXRB_DATA_HOME=<runtime>\output` (as AXRB's launcher does) → AXRB's `run_direct_game.ps1 -Apk -Package
  -Activity -GameName -Executable <root>\out\direct\axrb-direct.exe -DataDirectory <runtime>\direct -Sysroot
  <runtime>\direct\sysroot [-AssetList] -Translator …`. Exit 4 = host lost.
- **Launch test**: watches `direct.out` for `AXRB_AOSP_ACTIVITY_CREATED` / `AXRB_AOSP_LIFECYCLE` (5 min for ART's
  interpreted first start), runs, then stops through AXRB's `stop_game.ps1 -ParentPid` (closes the bridge window;
  the run script lets the game pause/stop/save). Logs `<runtime>\output\logs\game\{direct.out, direct.err,
  direct.log, host.err, host.log, session.json}` (diagnostics: `axrb/direct/*`); triage signatures
  `axrb-direct-*` and milestones ART started → activity created → OpenXR → Vulkan device (`catalog/triage.yaml`).

**Untested** (written 2026-10-15 from AXRB's source at 5b3dba19; no Windows run, no AXRB 2.x build yet): every
direct-mode step on a real PC (silent install of the 2.x NSIS installer, `make_sysroot.py` through WSL interop, the
launcher script, the launch test's markers and stop, the triage patterns against real logs), the upgrade of an
installed 1.0.x, and whether AXRB's own launcher shows FramePort-set-up runtimes as ready.

## Spike results (2026-10-10, Windows 11 26200, i7-14700K, RTX 4080 SUPER, 64 GB; headless)

| Step | Result |
|---|---|
| AXRB install | `AXRB-Setup-<v>.exe /S` (electron-builder one-click NSIS, per user, no UAC) → `%LOCALAPPDATA%\Programs\axrb-launcher`, 462 MB. Release assets: Setup exe + `SHA256SUMS-<v>-setup.txt` + source zip (no portable zip since 1.0.4). |
| Runtime setup | Not scriptable through AXRB (it lives in the Electron launcher's JS), so FramePort reproduces it: 4 pinned `dl.google.com` zips from AXRB's `core/components.json` (emulator 36.5.11, platform-tools r37.0.1, build-tools 36.1, `sys-img/google_apis/x86_64-36_r07`), hash-checked, into `%LOCALAPPDATA%\AXRB Runtime\sdk`, AVD `axrb-managed-api36` written as AXRB writes it, `ready.json` / `license-acceptance.json` receipts so AXRB's own launcher sees the same runtime as set up. 2.4 GB download, ~3 min on a fast line; 26 GB on disk after one 7 GB game. |
| Emulator boot | AXRB's `scripts\emulator\windows_android_emulator.ps1 -Action Start` (headless QEMU, AXRB clock adapter + GPU layer). First cold boot ~6 min (TSC-corrected clock cold-boots every time). ABIs `x86_64,arm64-v8a`. QEMU (`Start-Process`) and the adb server inherit the caller's handles: a pipe, a file through WSL interop, even `/dev/null` through interop (its relay stays open) all left the FramePort call hanging after the script had ended → FramePort starts AXRB scripts through WMI (`Win32_Process.Create`, hidden, nothing inherited), polls the PID and reads an `FP_EXIT <code>` line from the log. Windows PowerShell 5.1's `*>>` writes UTF-16. |
| AXRB runtime | `adb install --no-incremental --force-queryable -r axrb-openxr-runtime-debug.apk` (0.3 s); found by the OpenXR loader through the system runtime broker. |
| Data | `adb push` of OBB/pak files: 25-35 MB/s (7.2 GB Pinball FX VR in 5.5 min). A full uninstall (or a signature change: `INSTALL_FAILED_UPDATE_INCOMPATIBLE`) deletes the OBB folder and the saves → FramePort's stable per-game keys matter here too. |
| **Original APK** (Pinball FX VR) | No launcher activity (Meta's VR category only), started explicitly: **SIGABRT 0.3 s after loading Meta's `libovrplatformloader.so`**. Unconverted Quest APKs don't run. |
| **OVRPort-only build** (Lucky's Tale, Unity 2019 GLES) | OVRPort's loader → AXRB runtime → instance + session, frames submitted (2 s each without a host: the image send times out), no crash in 40 s. |
| **OVRPort-only build** (Pinball FX VR, UE5 Vulkan) | OVRPlugin pre-init OK (AXRB reports "Oculus 67.522.0"), then exits (ForceQuit) / with the no-ForceQuit build SIGSEGV in `gfxstream vkAllocateMemory` called from AXRB's runtime Vulkan layer. Not decided headless (no host attached, see below). |
| Host bridge | Needs an OpenXR runtime with a D3D11 adapter. Headless options all failed: SteamVR's null driver (`failed to find D3D11 adapter requested by OpenXR runtime`), `vrlink` without a headset (`xrCreateInstance` -2), Meta XR Simulator (binary download needs a Meta developer login). → frames-to-SteamVR need a real headset. |

**Build decision: FramePort hands AXRB an OVRPort-only build** (the recipe's OVRPort patches, no Steam Frame fixes,
signed with the game's FramePort key). The original APK crashes; FramePort's Frame fixes (FrameBridge adapter, Lepton
and Frame-runtime workarounds) target the Frame. The recipe's alternate (no-ForceQuit) build is used when the recipe
says so.

### FramePort end to end (same day, headless, this branch)

`frameport install com.playful.LuckysTale --to pc --no-library` (isolated `FRAMEPORT_HOME`): PC build (19 patches:
OVRPort's + `frame.ovrplatformcompat`/`frame.ovrstubs`/`frame.swapchain_limit`; static checks pass without the
FrameBridge check) → AXRB found, requirements met → emulator → AXRB runtime → `adb install` → 3.7 GB OBB in ~95 s
(39 MB/s) → `deployment.json` + launcher script. `frameport test com.playful.LuckysTale --to pc`: launcher → AXRB run
script → host bridge → triage `axrb-no-headset` (expected without a headset). The Steam shortcut write (needs a Steam
restart) is unit-tested only.

## How FramePort drives AXRB

- `tools/axrb.py`: find (env `FRAMEPORT_AXRB_DIR` > installed AXRB) / install (Setup exe, SHA256SUMS checked, `/S`) /
  runtime setup (above) / requirements (WHPX via AXRB's `check_windows.ps1`, ≥ 12 GB RAM, x64).
- Environment for every AXRB script (the installed build sets none of it itself): `AXRB_DATA_HOME=<Runtime>\output`,
  `ANDROID_AVD_HOME=<Runtime>\avd`, `ANDROID_USER_HOME=<Runtime>\android`, `ANDROID_HOME`/`ANDROID_SDK_ROOT=<Runtime>\sdk`,
  `ANDROID_ADB_SERVER_PORT=5038`, `ADB_USB_LEGACY=1`, `ADB_LOCAL_TRANSPORT_MAX_PORT=5683`, AXRB's embedded Python
  (`resources\runtime\tools\python`) first on PATH; arguments `-Sdk <Runtime>\sdk -Avd axrb-managed-api36 -Port 5584`.
- Install: start the emulator if needed (and stop it again if FramePort started it), install the AXRB runtime APK,
  `adb install -r` the build, push OBB/data to `/sdcard/Android/obb|data/<pkg>/`, record `<data>/pc/<pkg>/deployment.json`
  (`kind: "android"`).
- Play: a Steam shortcut runs FramePort's small launcher script (`%LOCALAPPDATA%\FramePort\axrb\fp-axrb-run.ps1`),
  which sets the environment above and calls AXRB's `scripts\run\run_windows_game.ps1` (boots the emulator when
  needed and then owns it, registers a SteamVR app identity, starts the host bridge, `am start`, waits; exit codes
  0 ok, 1 error, 3 game lost, 4 host lost; `logs\game\session.json`, `host.err`).
- Logs for diagnostics: `<Runtime>\output\logs\game\{host.log,host.err,session.json,guest.log}`,
  `logs\emulator\emulator.std{out,err}.log`.

## Headset results (2026-10-10, Steam Frame streaming to the PC through Steam Link / vrlink, 96 Hz)

AXRB's bridge opened a SteamVR session on the Frame each time (session state up to FOCUSED, both hands active), started
through FramePort's launcher with the emulator booted by AXRB's run script (~1 min), and shut down cleanly.

- **Lucky's Tale (Unity, GLES): black.** GLES games go through AXRB's raw image path (eye readback → TCP), sized
  by SteamVR's recommended resolution: 3412×3412 per eye at the owner's 250 % supersampling = 93 MB per frame,
  ~10 s per frame; at a lowered resolution 2644×2644 = 56 MB, 6.6 s per frame (~8.5 MB/s). Not fixable from
  FramePort (AXRB has no maximum-extent setting, only AXRB_MIN_EYE_EXTENT); even ~1000² would be ~1 fps.
  AXRB's zero-copy GPU path covers Vulkan games only.
- **Pinball FX VR (UE5, Vulkan): crash 4 s after start**, the same gfxstream `vkAllocateMemory` SIGSEGV from AXRB's
  runtime layer as headless (so not a headless artefact). AXRB's side.

Not reached: sound, controllers. Next candidates: a Vulkan game AXRB reports playable (Deadpool VR, Batman: Arkham
Shadow); an AXRB issue for the Pinball crash.

## Open (needs the owner + a headset)

- A game in the headset through SteamVR (picture, audio, controllers; Steam Frame controllers: AXRB #20).
- Pinball FX VR's Vulkan crash with the host attached (AXRB's demo game: likely an AXRB/emulator issue if it repeats).
- Play from the PC's Steam library (shortcut → launcher script → AXRB).
