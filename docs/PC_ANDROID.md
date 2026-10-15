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
