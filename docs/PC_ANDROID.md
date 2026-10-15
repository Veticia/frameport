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
  Frame), else **direct for `graphics` Vulkan, emulator for GLES / unknown**. Unreal games never declare
  `android.hardware.vulkan`; since analysis version 20 their manifest meta-data
  `com.epicgames.{ue4,unreal}.GameActivity.bSupportsVulkan` = true makes them "Vulkan (Unreal bSupportsVulkan)"
  (UE 4.25+ Quest builds, e.g. Deadpool VR, Pinball FX VR, Myst; false or missing = GLES: Espire 1, Vader Immortal). `deployment.json` records `backend`
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
  (FramePort removes its converted copies after an install; `FRAMEPORT_AXRB_LAUNCHER_DIR` moves FramePort's `axrb`
  folder, for isolated `FRAMEPORT_HOME` runs), the launch activity comes from the APK's manifest
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
says so. (Since 2026-10-15 the PC build also carries the runtime-independent APK fixes and FrameBridge, see below.)

## Which patches reach the build for this PC

One recipe serves both builds; `patches/base.pc_selection(patches, analysis)` picks the PC part (used by
`build.build(pc=True)`, `pipeline.pc_recipe_fingerprint`, the PC deployment record and `ui/components.pc_outdated`).
`docs/PATCHES.md` marks each kept patch "also in builds for this PC (AXRB)".

- **All of OVRPort's patches** (category `overport`).
- **APK fixes that don't depend on Lepton or the Frame's runtime** (`Patch.on_pc = True`): they fix the game's own
  code or its Meta libraries, which misbehave the same way in AXRB's Android (no Meta services, not a Quest device
  model, no Horizon app): `frame.ovrplatformcompat`, `frame.ovrstubs`, `frame.swapchain_limit` (OVRPort's
  dispatcher), `frame.ovr_stable_user`, `frame.unreal_ovrp_entrypoints`, `frame.avatar_stub`, `frame.vrapi_stub`,
  `frame.metaxr_telemetry`, `frame.oculusos`, `frame.ovr_microphone`, `frame.langpacks`, `frame.asset_files`,
  `frame.asset_files_stub`, `frame.ovr_trace`, `frame.blazerush`, `frame.unity_oculus_check` (with its ovrpshim),
  `frame.unity_text_input`, `frame.unreal_quest_precompile`, `frame.unreal_quest_keymap`,
  `frame.unreal_thumb_touch`, `frame.unreal_skip_obb_check`, `frame.unreal_skip_obb_delivery`, `frame.nodebug`,
  `frame.meta_permissions`, `frame.launcher`.
- **Experimental on the PC** (`on_pc = "experimental"`, kept but not yet seen working in AXRB): `frame.vk_sanitize`
  (the Vulkan shim; gfxstream / the host driver instead of Lepton's Fossilize), `frame.vrapi_bridge` (VrApi → OpenXR
  through OVRPort's loader, so AXRB's runtime; the **GL shim stays off**: it fixes Mesa/Zink GLSL strictness, and
  the bridge doesn't need it — only `frame.gl_shim` requires the bridge, not the other way round),
  `frame.unity_tls_key_pool`, `frame.eos_keychain_loop`.
- **FrameBridge** (`frame.adapter`) with its settings in `lib/<abi>/libframe_settings.so`: it wraps OVRPort's loader
  the same way in AXRB (scale, haptic_fix, pose_time_fix, rect_clamp, profile_remap, ...). Left out
  (`settings.PC_DENY`): `sync_guard` (a race in the Frame's vrclient.so), `focus_hold` / `focus_hold_ms` (the
  Frame's flickering wear sensor; FrameBridge's built-in default is on, so the PC build writes `focus_hold=0`,
  `settings.PC_VALUES`), `controller_models` (models the agent converts from the Frame's SteamVR at install) and the
  settings only the GL shim / Zink shader layer read (`gl_*`, `zink_*`). `controller_fix` stays: it is the game's
  choice (0 = hand tracking) and its Touch mapping of `/valve/` and generic profiles is runtime-independent.
  FrameBridge finds `libframe_settings.so` next to its own path (dladdr); on the Frame the agent also passes the
  settings as files, in AXRB the library is the only source, so the PC build turns `extractNativeLibs` on
  (`build.extract_native_libs`; apps with `extractNativeLibs="false"` would map their libraries from base.apk).
  Without OVRPort (a 32-bit recipe) the PC build drops FrameBridge: there is no loader to wrap.
- **PC-only:** OVRPort's `patch_disable_meta_xr_audio_telemetry` (for x86_64 ARM translation, which AXRB is) is
  never in a Steam Frame recipe (`applies()` is False); `pc_selection` adds it, given the analysis, to games with
  Meta XR Audio (`Patch.pc_detect`; AXRB's own profile turns it on for North Star).
- **Off on the PC:** Lepton/Android 11 workarounds (clipboard, Vivox audio route, start activity, ...), the Zink/Mesa
  GL shims and shader layer, pac_hints/luajit, hardware video decoding, every `device.*` option, the 32-bit bridge.
- TODO: `device.files` (files written into the game's storage at install) could be reproduced with `adb push` in
  `targets/pc_android.py`.

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

## Testing without a headset (Monado's simulated HMD, 2026-10-15)

AXRB's host bridge needs an OpenXR session with a D3D11 binding, which SteamVR doesn't give without a headset. For
unattended tests FramePort can run the bridge against **Monado for Windows** with Monado's simulated HMD instead,
without changing the system's OpenXR runtime or touching SteamVR:

- **Monado build:** an official CI artifact of `monado/monado` (gitlab.freedesktop.org, project 2685), job `windows`
  of a `main` pipeline (there is no Windows release asset; the job's `install/` folder is the portable runtime:
  `bin\monado-service.exe`, `bin\openxr_monado.dll`, `openxr_monado.json`, SDL2/Vulkan loader DLLs). Tested build:
  pipeline 1766968, job 112793105, commit 8b259763f (`v25.1.0-917-g8b259763f`). Unpacked by hand into
  `%LOCALAPPDATA%\FramePort-dev\monado\install` (or `FRAMEPORT_MONADO_DIR`): no installer, no UAC, no registry. Its
  `share\steamvr-monado` (a SteamVR driver) is never registered.
- **Selection per process only:** the OpenXR loader's `XR_RUNTIME_JSON=<monado>\openxr_monado.json`. HKLM's
  `ActiveRuntime` stays SteamVR. AXRB's run script registers an app identity with SteamVR whenever HKLM names SteamVR
  (an OpenVR utility client, which starts SteamVR), so under the simulator FramePort's launcher call answers that one
  `python steamvr_app_identity.py` call "failed" (the script only warns).
- **Service settings** (`tools/xr_sim.SERVICE_ENV`): `SIMULATED_ENABLE=1` (the simulated builder; the Windows build
  only has the qwerty/remote/simulated builders, so no real device is probed), `SIMULATED_LEFT/RIGHT=ml2` (the only
  simulated controllers whose bindings cover the Vive wand profile: AXRB suggests Touch/Index/Vive, so with `simple`
  or `wmr` its hands stay inactive), `XRT_COMPOSITOR_DEFAULT_FRAMERATE=72`. Headless variant: `XRT_COMPOSITOR_NULL=1`
  + `XRT_COMPOSITOR_NULL_FPS=72` (no window, nothing to capture).
- **D3D11 + adapter:** Monado's D3D11 client compositor works (timeline semaphores); it renders on the discrete GPU
  (Vulkan device 0, RTX 4080 SUPER here; `XRT_COMPOSITOR_FORCE_GPU_INDEX` would change it) and asks D3D11 clients for
  that adapter's LUID, which DXGI finds.
- **Results:** a minimal D3D11 client (`native/xrsimprobe/xr_sim_probe.cpp`, built with VS Build Tools against the
  Khronos `openxr_loader_windows` 1.1.63 release) went IDLE → READY → SYNCHRONIZED → VISIBLE → FOCUSED in 0.01 s
  and submitted 72 frames/s until it asked to exit. AXRB's host bridge (`--serve-openxr 38490 0 <title>`, no
  emulator) reached `D3D11 graphics binding ready`, session state 5 (FOCUSED), `AXRB TCP: listening on
  0.0.0.0:38490`, both controllers active with valid poses, `host-end-frame: rate=72.0/s`, its splash in both eyes.
  No game was run yet.
- **Seeing what is drawn:** the windowed compositor ("Monado (Windowed)", 1264×681, both eyes side by side without
  distortion) can be saved with `native/xrsimprobe/xr_sim_capture.cpp` (`PrintWindow`, works behind other windows;
  `xr_sim.capture(path)`), and `--sim` also sets AXRB's own opt-in `AXRB_CAPTURE_PREFIX` (4 PPM readbacks of the
  submitted layers, 5 s apart, from frame 300) into `<data>/logs/xr-sim/`. Monado itself has no screenshot function;
  `XRT_WINDOW_PEEK` and `XRT_DEBUG_GUI` exist but only show windows.
- **Use:** `frameport test <pkg> --to pc --sim` (or `FRAMEPORT_PC_XR_SIM=1`): starts `monado-service.exe` hidden
  (WMI, like AXRB's scripts; ready when its pipe `\\.\pipe\<%TEMP%>\monado_comp_ipc` exists), passes the environment to
  the AXRB launch (`pc_android.launch_test(extra_env=…)`, default `xr_sim.active_env()`), stops the service afterwards
  (an already running Monado is used and left running). Service log: `<data>/logs/xr-sim-service.log`.
- **Limits:** the simulated head wobbles slowly and the controllers hold still with no buttons pressed, so it proves
  startup, frame submission and what is drawn, not gameplay; the windowed compositor opens a visible window on the
  desktop; frame pacing is Monado's, not a headset's.
