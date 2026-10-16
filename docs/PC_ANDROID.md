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
  (`Android/obb/<pkg>/<file>\t<Windows path>`, read-only mounts of the files where they are; WSL-only folders, which
  Windows reads only through `\\wsl$`, are copied next to the APK). AXRB's `prepare_art_session.py` turns every
  content file into one `--mount-ro` argument of `axrb-direct.exe` (with the path resolved: junctions and 8.3 names
  don't shorten it), and Batman's 90 files from a long dump folder made a 32,876-character command line (Windows'
  limit is 32,767: "The filename or extension is too long"). A list over 25,000 characters therefore goes through
  hard links in `games\<pkg>\content` (no extra space; only on the drive FramePort's folder is on), else the install
  stops with a clear message. App data and saves: `<runtime>\direct\data\data\<pkg>` and
  `direct\sdcard\Android\data\<pkg>`.
- **Play**: Steam shortcut → `%LOCALAPPDATA%\FramePort\axrb\fp-axrb-direct.ps1 -Package … -Activity … [flags]` →
  sets `AXRB_DATA_HOME=<runtime>\output` (as AXRB's launcher does) → AXRB's `run_direct_game.ps1 -Apk -Package
  -Activity -GameName -Executable <root>\out\direct\axrb-direct.exe -DataDirectory <runtime>\direct -Sysroot
  <runtime>\direct\sysroot [-AssetList] -Translator …`. Exit 4 = host lost.
- **Launch test**: watches `direct.out` for `AXRB_AOSP_ACTIVITY_CREATED` / `AXRB_AOSP_LIFECYCLE` (5 min for ART's
  interpreted first start), runs, then stops through AXRB's `stop_game.ps1 -ParentPid` (closes the bridge window;
  the run script lets the game pause/stop/save). Logs `<runtime>\output\logs\game\{direct.out, direct.err,
  direct.log, host.err, host.log, session.json}` (diagnostics: `axrb/direct/*`); triage signatures
  `axrb-direct-*` and milestones ART started → activity created → OpenXR → Vulkan device (`catalog/triage.yaml`).

**Tested end to end with a local build** (2026-10-15, AXRB `windows-android16` @ 59626ac built on the dev PC,
`FRAMEPORT_AXRB_DIRECT_DIR` = its staged `runtime\`, the staged sysroot reached through a junction
`<FRAMEPORT_AXRB_DIRECT_RUNTIME>\direct\sysroot`; Monado's simulated HMD, see below): scan → PC build → direct
install → `frameport test --to pc --sim --axrb-backend direct` → launcher → AXRB's run script → bridge + runner →
milestones → AXRB's Stop. Still untested: the 2.x NSIS installer (no release yet), `setup_direct`'s download +
`make_sysroot.py` path, the upgrade of an installed 1.0.x, AXRB's launcher seeing FramePort-set-up runtimes.

### Direct-mode results (2026-10-15, Monado simulated HMD at 72 Hz, Dynarmic, no headset)

PC build = the catalog recipe's PC selection (OVRPort's patches + `on_pc` fixes + FrameBridge with `focus_hold=0`),
no `pc.axrb_backend` options. "fps" = AXRB.Perf `end-frame` (game) / `host-fresh-submit` (bridge → Monado), the
median of the last summaries. What is drawn was judged from Monado's window and AXRB's layer captures.

| Game (engine, graphics) | Reached | fps | Picture | Blocker / notes |
|---|---|---|---|---|
| Batman: Arkham Shadow 1.4.1-350961 (Unity, Vulkan; 90 content files, 27 GB) | menu (SELECT REGION) in ~25 s | 63.7 / 62.9 | stereo menu over the Batcave scene, both eyes correct | none; needed the content-list fix (command line overflow) and hard links; stop clean (activities stopped) |
| PowerWash Simulator VR 2.5.0 (Unity 2020.3, Vulkan) | in game: the garage hub (~3-4 min; at 90 s still its loading logo) | 70.5 / 69.8 | garage in both eyes, both hands drawn at the simulated controllers | none; Meta multiplayer callbacks throw NullReferenceExceptions (no Meta services, harmless so far) |
| LEGO Bricktales 2.1 (Unity, Vulkan, URP) | scene `Diorama_Hub_Startup` loaded (~3 min) | 72 / 72 | intro text panel ("You want to free the simulation? Follow the white rabbit.") on black, both eyes | stop slow: the game didn't finish its activity shutdown in 25 s (`axrb-direct-slow-stop`); Meta achievements fail (no Meta services, harmless) |
| Toy Master 1.1.2 (Unity 2022.3, **GLES**) | activity created, then crash in 18 s | - | - | not a Vulkan game: "Vulkan (Unity boot.config)" came from the Oculus plugin's `xr-vulkan-*` keys; Unity: "Unable to retrieve the default EGL display!" + SIGTRAP. Automatic choice now sends such games to the emulator |
| Marvel's Deadpool VR 1.0.40 (UE 4.27, Vulkan; `frame.vk_sanitize` in the build) | first-run LANGUAGE menu (~3 min; at 90 s still loading at 1-2 fps) | 64 (FrameBridge pacing) | game-drawn stereo menu, both eyes | none; AXRB transcodes ASTC to BC on the GPU; stop slow (`axrb-direct-slow-stop`) |
| Pinball FX VR 1.9 (UE 5.3, Vulkan, 7 GB OBB) | 3D lobby with the Terms & Conditions panel (~4 min) | ~60 (FrameBridge pacing) | arcade hall + panel in both eyes | none of the emulator's blockers (gfxstream `vkAllocateMemory` SIGSEGV, and Refract's FRSQRTE SIGILL / ASTC layout probe / promoted extension names) showed up in direct mode (no SIGILL under Dynarmic, no ASTC or device-loss failure; AXRB stores ASTC as BC); OVRPlugin's xrPerfSettingsSetPerformanceLevelEXT unsupported (harmless) |
| Myst 3.3.0 (UE 5.3, Vulkan) | loading screen (book), then its starfield intro | 72 | stereo, both eyes | a white splash quad at first (texture not shown yet), then normal |
| Metro Awakening 1.0-916944 (UE 5.2, Vulkan) | splash art, then the USER DATA COLLECTION consent screen with the laser pointer | 72 | stereo, both eyes | stop slow (`axrb-direct-slow-stop`) |

Unreal Vulkan games: before analysis version 20 every Unreal game was "GLES or unknown" (Unreal never declares
`android.hardware.vulkan`), so the automatic choice sent them to the emulator; now `GameActivity.bSupportsVulkan`
picks direct mode (Myst and Metro above were installed without `--axrb-backend`). Unreal probes EGL before it
creates its Vulkan device, which tripped `axrb-direct-no-gles`; the signature now needs no Vulkan device. None of
the four needed a recipe change for the PC.

Integration bugs found and fixed on the way: AXRB's trap message never reached FramePort's log; the content list
overflowed the command line (Batman); Unity's EGL failure and SIGTRAP weren't triaged; long sessions lost their
start-up milestones; the boot.config Vulkan label picked direct mode for GLES games. Open, AXRB's side: the content
list should reach `axrb-direct.exe` as a file (FramePort's hard links only work when the data is on the drive of
FramePort's folder; drafted as an upstream issue). Needs a headset: picture and latency through SteamVR, sound,
controllers (the simulated ones hold still), Batman beyond its region menu.

### VrApi-direct games (2026-10-16, direct mode, Monado simulated HMD, Dynarmic, no headset)

Games whose engine calls VrApi itself (no OVRPlugin), so OVRPort can't translate them: they need a `libvrapi.so`
that translates VrApi to OpenXR. Two candidates: FramePort's `frame.vrapi_bridge` (OVRPort's translator + FramePort's
changes) and OVRPort's own `patch_vrapi_openxr` (the unpatched translator; only in experimental CLI builds made with
`-PwithVrApi=true`, e.g. the `OVRPort-CLI-1.2.5-experimental-5e7df52.jar` AXRB's v1.0.0/1.0.1 releases attached;
AXRB's own profile for The Climb 2 uses it with `-PrecomposeProjectionLayers`). Only converted copies of both games
were at hand, so the comparison swapped the one file (OVRPort's = the v1.2.5 build from the 2026-10-02 Frame tests).

| Game (engine, graphics) | VrApi route | Reached | fps | Picture | Blocker / notes |
|---|---|---|---|---|---|
| The Climb 2 2.2 (CryEngine, Vulkan VrApi) | FramePort's bridge | first-start menu (gender/skin selection) in ~60 s | 69.8 / 69.8 | menu, both hands and controllers, both eyes (AXRB layer captures + Monado's window) | needs AXRB's HostWindow fix (below, AXRB's side) + `frame.vk_android_extensions`; crash only during AXRB's stop (forgiven by triage); no `-PrecomposeProjectionLayers` needed |
| The Climb 2 2.2 | OVRPort's translator | the same menu | 70.2 / 70.2 | identical | same blockers; passes AXRB's AHB device-extension request through unchanged (worked only because `libfp_vkext.so` lists AHB) |
| Path of the Warrior (own engine, **GLES** VrApi) | - | not run | - | - | GLES: direct mode can't run it; the emulator (1.0.4) had a copy signed with another key installed by another test, so it wasn't replaced. OVRPort's translator can't run it anywhere (no GLES path: `cannot locate symbol "vrapi_GetTextureSwapChainHandle"`, Frame test 2026-10-02); FramePort's bridge has one |

**Default for PC builds: FramePort's bridge** (`frame.vrapi_bridge`, now `on_pc = True`; recipes keep it as they
are, nothing to switch). Both translators give the same picture and frame rate in direct mode, but OVRPort's needs
an unpublished experimental CLI (stable jars list `patch_vrapi_openxr` but fail), has no GLES path (POTW, every GLES
VrApi game on the emulator) and lacks the device-extension filter; FramePort's is the same code the Frame builds
use, so one recipe serves both.

Blockers found on the way (in order):
1. **No window for NativeActivity** (AXRB's side, drafted upstream): CryEngine's `EngineActivity` takes its surface
   with `Window.takeSurface`; direct mode's `HostWindow.attach` only bridges SurfaceViews (`AXRB_AOSP_HOST_SURFACES
   0`), so `APP_CMD_INIT_WINDOW` never comes and the game waits after `Loading plug-in CryHTTP` (triage
   `axrb-direct-no-native-window`). A ~40-line HostWindow patch (`mTakeSurfaceCallback` → `registerSurface` +
   `surfaceCreated/Changed`, bootstrap dex rebuilt with AXRB's `build_art_bootstrap.py` into a test copy of the
   local build) fixed it. Likely the same for every NativeActivity engine.
2. **Quest Vulkan device extensions**: CryEngine checks its list before vkCreateDevice and gave up
   (`CrySystem initialization failed!`): AXRB's runtime asks for `VK_ANDROID_external_memory_android_hardware_buffer`
   also in direct mode, and the game requires `VK_EXT_fragment_density_map` (NVIDIA's Windows driver has neither).
   The bridge now leaves runtime extensions no device offers out; `frame.vk_android_extensions` lists the missing
   Quest ones to the renderer (AXRB drops them at vkCreateDevice). Triage `axrb-direct-vulkan-extensions`.
3. **Simulator only**: Monado's head lacks POSITION_TRACKED, so AXRB's host never made its LOCAL origin and the
   bridge never got a tracked head (empty frames, `initial recenter: head flags=0x0`) → `--sim` sets AXRB's
   `AXRB_TEST_SYNTHETIC_HMD=2`.
4. The analysis called The Climb 2 "GLES or unknown" (no manifest declaration) → analysis 20: engines importing
   `vrapi_CreateSystemVulkan` are "Vulkan (VrApi Vulkan system)", so it goes to direct mode automatically.

Debugging aid: `FRAMEPORT_AXRB_TEST_ENV="AXRB_DIRECT_TRACE=syscall,vulkan;AXRB_CAPTURE_FIRST_SEQUENCE=4500"` passes
variables to a launch test's AXRB processes (`AXRB_DIRECT_DUMP` only works in AXRB's own VM mode, not with ART).
Needs a headset: the picture through SteamVR, controllers past the first menu (the simulated ones hold still),
climbing, sound; and a released AXRB-Emulatorless with the window fix.

### Library sweep 2026-10-16

Every other Quest game of the owner's library that `pipeline.pc_installable` accepts and whose APK still exists,
installed with `frameport install <pkg> --to pc --no-library` (automatic backend, isolated `FRAMEPORT_HOME`, catalog
recipe or heuristics, no per-game changes) and tested with `frameport test <pkg> --to pc --sim --seconds 150` (240 for
Unreal) under Monado's simulated HMD at 72 Hz. Direct mode = the local AXRB build of 59626ac **with the HostWindow
patch** (above; upstream had no newer commit or 2.x release on 2026-10-16); emulator = AXRB 1.0.4 with this branch's
`adb install -g`, `projection_only=2` and qwerty-head fixes. "fps" = game submits / bridge to Monado (AXRB.Perf
medians); "Picture" = Monado's window (both eyes) at the end of the run, plus AXRB's layer captures. Not run: the
three 32-bit-only games (Sports Scramble, Shadow Point, Journey of the Gods: AXRB has no AArch32, and the PC build
drops the 32-bit bridge), and the emulator games after NEX Player (disk, see the end).

| Game (engine) | Backend | Reached | fps | Picture | Blocker / notes |
|---|---|---|---|---|---|
| Arcsmith 1.128 (Unity) | direct | frames | 59 / 51.7 | in game: the workshop, both hands with tools, both eyes | none |
| QuestCraft 6.0.0-RC4 (Unity) | direct | frames | 65.1 / 63.6 | launcher room, QuestCraft EULA panel, laser pointers | none (Minecraft itself is behind its Continue button) |
| BattleGlide 0.4.1 (UE) | direct | frames | 62.2 / 54.8 | title screen (START), both hands | none |
| In Death: Unchained 1.8.0 (UE) | direct | frames | 64.1 / 56.9 | sky + PRIVACY UPDATE panel | none |
| The Boys VR 1.1.0 (Unity) | direct | frames | 62.4 / 59.9 | ANALYTICS DISCLAIMER panel over a lit floor, laser | slow stop |
| Creed: Rise to Glory CE (UE) | direct | frames | 60.6 / 55.5 | IMPORTANT SAFETY INFORMATION panel, both hands, laser | none |
| Wallace & Gromit: The Grand Getaway 1.0.34 (UE) | direct | frames | 61.5 / 53.5 | space scene (nebula, planets), both gloved hands | none |
| VR4 2.3 (UE) | direct | frames | 71.9 / 72 | title scene: church interior, "resident evil 4" logo, both hands | none (slow stop) |
| Demeter 1.1.0 (Unity, MR) | direct | frames, FrameBridge scene emulation active | 62.3 / 57.3 | "PERMISSION REQUIRED ... access to your spatial data" panel, controllers | AXRB's direct mode grants only 8 allow-listed permissions (`HostPackageManager.permission`): `com.oculus.permission.USE_SCENE` stays denied although the build declares it (`frame.meta_permissions`). AXRB's side |
| Green Hell VR 2.5.2 (Unity) | direct | frames | 11.8 / 11.7 | loading screen at "50%" after 180 s | slow load |
| TRIANGLE STRATEGY 1.0 (UE) | direct | frames | 44.6 / 40.2 | both controllers with a "Control Guide" label, rest black at 280 s | still loading? |
| Riven 1.9.0 (UE) | direct | frames | 29.8 / 25.2 | white splash quad (AXRB captures), black window at 274 s | still loading (as Myst's white splash) |
| Espire 2 1.7.13 (UE) | direct | frames | 72 / 71.8 | white splash quad low in both eyes at 289 s | splash texture never shown |
| Carve Snowboarding 1.09 (UE) | direct | frames | 64.5 / 59.3 | both hands, rest black at 270 s | scene never appears; not diagnosed |
| Asgard's Wrath 2 12.1 (UE, 33 GB) | direct | frames | 30.6 / 21.9 | black at 287 s | still loading |
| Assassin's Creed Nexus 207706 (Unity) | direct | frames | 42.6 / 42.6 | black at 180 s (captures too) | loading or no visible layer yet |
| BARTENDER VR SIMULATOR 1.9.2.7 (UE) | direct | frames (in a second process) | 62.7 / 53.6 | black for 260 s (window + captures all 0) | not diagnosed (no Unreal log lines in direct.log) |
| Vermillion 1.7.2 (Unity) | direct | frames, OVRManager focused | 63.8 / 60.3 | black with its brush loading icon | stays there (Vuplex 3D WebView starts, then nothing) |
| H.U.N.T 0.1.2 (Unity OpenXR) | direct | session created, no frame in 200 s | - | bridge splash | never submits a frame; not diagnosed |
| WiiCompiled VR 0.4.0 | direct | 2D launcher activity, no OpenXR | - | bridge splash | its launcher imports games through a document picker; the VR part is a second activity |
| Medieval Dynasty New Settlement 2.4 (Unity) | direct | OpenXR FOCUSED, exit(1) at 30 s | - | - | **opens an Android Dialog** (right after "initOculus Java!"); direct mode can't add a second window (`UnsupportedOperationException: addToDisplayAsUser`, "Adding window failed"). Triage `axrb-direct-dialog-window` |
| Under Cover 1.1.3r2 (Unity) | direct | Vulkan device, exit(1) at 33 s | - | - | the same Dialog |
| Of Lies and Rain 1.1.0 (UE) | direct | vkCreateInstance, exit(1) at 32 s | - | - | Unreal's `MessageBox01` (an error box before its Vulkan device; its text is lost with the window) |
| NOPE CHALLENGE 1.0.964 (UE) | direct | ART, crash at 15 s | - | - | Google Play services call `IActivityManager.getMyMemoryState`: `AXRB_SERVICE_UNIMPLEMENTED` -> uncaught exception. Triage `axrb-direct-unimplemented-service` |
| BAM 1.2 (Unity) | direct (manifest declares Vulkan) | activity, crash at 33 s | - | - | runs GLES: "Unable to retrieve the default EGL display!"; the manifest's Vulkan feature doesn't say which API Unity picks first. Emulator run not reached |
| Mobile Suit Gundam: Silver Phantom 1.1.58 (Unity) | direct | install refused | - | - | 257 content files with ~80-character names don't fit axrb-direct.exe's command line even through hard links (the guest paths alone are too long); needs AXRB to read the list from a file or mount the folder. Emulator run not reached |
| Silhouette 1.3.0 (Unity) | emulator | frames | 30.2 / 30.1 | title menu (Play / Options / Quit), both eyes | none headless |
| Yupitergrad 2.6.1 (Unity) | emulator | frames | 13.8 / 14 | splash ("Loading...") in both eyes | still loading at 174 s |
| 4XVR Video Player 2.0.22 | emulator | frames | 34.9 / 32.2 | black | gfxstream rejects its GL every frame: `glEnable(GL_FRAMEBUFFER_SRGB_EXT)`, a `glTexParameteri` value, vertex attribute index >= `CODEC_MAX_VERTEX_ATTRIBUTES` |
| NEX Player 1.3.1 (Unity) | emulator | frames | 9.3 / 9.3 | black | not diagnosed |
| Lambda1VR 1.7.3 (Team Beef) | emulator | Java crash at start | - | - | `libopenxr_loader_google.so not found`: TBXR ports load `openxr_loader_<Build.MANUFACTURER>`; `frame.tbxr_vendor` handles "valve" and isn't in PC builds; AXRB's Android says "google" (a PC variant: add that library and match "google" in the maker checks) |

Summary (31 games run): **8 reach a menu or the game in both eyes** in direct mode (Arcsmith in game; QuestCraft,
BattleGlide, In Death, The Boys, Creed, Wallace & Gromit, VR4 at their first menu/panel) and 1 in the emulator
(Silhouette); Demeter shows its own "permission required" panel; 12 submit frames but were still loading or black at
the end of the run (big Unreal games at 30-45 fps while loading under Dynarmic); 9 never submit a frame. The most
common blockers, all on AXRB's side:
1. **No second window in direct mode** (3 games: Medieval Dynasty, Under Cover, Of Lies and Rain): any Android
   Dialog ends the game. Upstream candidate together with the permission allow-list (Demeter's `USE_SCENE`) and the
   unimplemented framework calls (Play services' `getMyMemoryState`, NOPE).
2. **Long or loading-heavy starts**: under Dynarmic the large Unreal games need more than 4-5 minutes to their first
   picture (Riven, Asgard's Wrath 2, TRIANGLE STRATEGY, Espire 2's white splash); a longer `--seconds` would show more.
3. **The content list on the command line** (Gundam): a list file or folder mounts in `axrb-direct.exe` would fix it
   for good (drafted upstream).
4. GLES-in-direct-mode mislabels: a Unity manifest that declares Vulkan can still run GLES (BAM).

FramePort changes from the sweep: triage `axrb-direct-dialog-window` and `axrb-direct-unimplemented-service`
(`axrb-direct-no-gles` no longer blames Unreal's EGL probe when one of those ended the game first); the
command-line message suggests the emulator; and **direct-mode uninstalls and reinstalls remove the libraries AXRB's
`prepare_art_session.py` extracts per APK** into `<runtime>\direct\installed\<identity>`, which AXRB never deletes
(5 GB after 25 games; `pc_android.staged_libraries` computes AXRB's identity: the APK's Windows path + every
`lib/arm64-v8a` entry's name, CRC and size; unit-tested, not yet seen on a real uninstall).

The emulator half stopped after NEX Player: the WSL disk and C: filled up (other jobs; C: 14 GB free, below the 60
GB the sweep keeps), so the remaining emulator games were not run: The Room VR, Pixel Ripped 1995, Time Stall, Five
Nights at Freddy's: HW, Star Wars Pinball VR, I Am Cat, Vader Immortal 1-3, Genotype, Phantom: Covert Ops, Sniper Elite
VR (+ Winter Warrior), Robo Recall, Budget Cuts Ultimate, Espire 1, BodyCombat, Star Wars: Tales from the Galaxy's
Edge, HITMAN 3, Jurassic World Aftermath, Nano, Wander, Stremio, Eleven Table Tennis, BlazeRush, plus BAM and Gundam
in the emulator.

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
  `frame.meta_permissions`, `frame.launcher`, and `frame.vrapi_bridge` (VrApi → OpenXR through OVRPort's loader,
  so AXRB's runtime; seen working in direct mode with The Climb 2, see "VrApi-direct games" below; the bridge
  doesn't need the GL shim, but GLES games keep `frame.gl_shim` for gfxstream, see "GLES games in the emulator").
- **Experimental on the PC, for GLES games in the emulator**: `frame.gl_shim` (only with a GLES analysis,
  `Patch.pc_keeps`; Vulkan VrApi games never get it), `frame.unity_multipass` (also added by `pc_detect`), and the
  GLES FrameBridge defaults `scale` ≤ 0.5, `scale_max=1`, `projection_only=1`.
- **Experimental on the PC** (`on_pc = "experimental"`, kept but not yet seen working in AXRB): `frame.vk_sanitize`
  (the Vulkan shim; gfxstream / the host driver instead of Lepton's Fossilize), `frame.unity_tls_key_pool`,
  `frame.eos_keychain_loop`.
- **PC-only APK fix** `frame.vk_android_extensions` (`pc_detect`: direct-VrApi games whose engine creates VrApi's
  Vulkan system, analysis `graphics` "Vulkan (VrApi Vulkan system)"): the renderer's
  `vkEnumerateDeviceExtensionProperties` import goes to `libfp_vkext.so`, which also lists the Quest device
  extensions the PC's driver lacks (VK_EXT_fragment_density_map, Android external memory); AXRB's vkCreateDevice
  leaves them out again.
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
  ~10 s per frame; at a lowered resolution 2644×2644 = 56 MB, 6.6 s per frame (~8.5 MB/s). AXRB's zero-copy GPU
  path covers Vulkan games only. The ~8.5 MB/s was the transport, not the readback: see "GLES games in the
  emulator" below (adb reverse: ~100 MB/s, half-size eyes, multipass: Lucky's Tale draws at 30 fps).
- **Pinball FX VR (UE5, Vulkan): crash 4 s after start**, the same gfxstream `vkAllocateMemory` SIGSEGV from AXRB's
  runtime layer as headless (so not a headless artefact). AXRB's side.

Not reached: sound, controllers. Next candidates: a Vulkan game AXRB reports playable (Deadpool VR, Batman: Arkham
Shadow); an AXRB issue for the Pinball crash.

## GLES games in the emulator (2026-10-16, headless under Monado's simulated HMD)

AXRB 1.0.4's emulator is the only backend with GLES (direct mode's EGL is a stub). A GLES frame is read back with
`glReadPixels` on the game's thread inside `xrEndFrame` (`runtime/src/frame_transport.cpp`), sent as raw RGBA to the
host (two eyes per frame) and uploaded there; `xrEndFrame` returns only after the send. Measured with
`AXRB.Perf eye-readback` / `image-send` (guest log) and `host-image-arrival` (host.err):

| Eye size (per eye) | Bytes/frame | Route | Send | Frame rate |
|---|---|---|---|---|
| 896×1007 (Monado's recommendation) | 7.2 MB | default: TCP to 10.0.2.2 (QEMU user-mode network) | 850-950 ms | 1.1 fps |
| 448×504 (`scale=0.5`) | 1.8 MB | default | 200-230 ms | 4.4 fps |
| 896×1008 | 7.2 MB | **adb reverse** (`debug.axrb.image_transport=adb`) | 87 ms (~83 MB/s) | 9.4 fps |
| 640×720 (`scale=0.7`) | 3.7 MB | adb reverse | 38 ms (~96 MB/s) | 17.6 fps |
| 448×512 (`scale=0.5`) | 1.8 MB | adb reverse | 14.5 ms (~125 MB/s) | 30.5-30.8 fps |

The readback itself is 1-3 ms. The default route caps at ~8 MB/s (the same rate as in the headset run above); AXRB's
adb reverse route (AXRB sets up `adb reverse tcp:38491` anyway) is 10-15x faster. For SteamVR's resolutions the math
stays hard: 2016² per eye = 32.5 MB/frame (~3 fps), at half size 8.1 MB (~10 fps); the owner's 2644² at half size
14 MB (~6 fps). Lowering SteamVR's resolution for the game is what helps further (AXRB has a minimum-extent setting,
`AXRB_MIN_EYE_EXTENT`, but no maximum).

What FramePort does for GLES games on this PC (all in the PC build / launcher, nothing on the Frame changes):
- FramePort's launcher (`fp-axrb-run.ps1`) starts Android itself when it isn't running (AXRB's Start action, as the run
  script would; the run script then gets `-OwnsEmulator` and shuts it down), sets `debug.axrb.image_transport=adb`
  (`FRAMEPORT_AXRB_IMAGE_TRANSPORT=default` leaves AXRB's choice) and `debug.wm.disable_deprecated_target_sdk_dialog=1`.
- `pc_selection` with a GLES analysis: FrameBridge `scale` at most 0.5 (FrameBridge's minimum), `scale_max=1` (the
  maximum eye size reported is the scaled one too), `projection_only=2`: AXRB's emulator refuses every GLES frame with
  more than one layer (`xrEndFrame` -2), so FrameBridge submits the projection layer alone after drawing the quad
  layers that follow it into its eye images (`native/adapter/quad_compose_gl.c`, on the game's GL context, per eye at
  the eye's view and FOV, blending by the quad's flags); `projection_only=1` only drops them.
- Game installs use `adb install -g` (runtime permissions granted, as Lepton does on the Frame): Android's permission
  dialog over the game's activity pauses Unity for good (BattleSisters asks for the microphone in its menu).
- The VrApi bridge copies the game's eye textures into the OpenXR images with `glBlitFramebuffer` when the context
  has no `glCopyImageSubData` (the emulator's GLES translator is "OpenGL ES 3.1" without `GL_EXT_copy_image`; its stub
  only logs `libEGL: called unimplemented OpenGL ES API`, so every eye reached the runtime black).
- `frame.unity_multipass` (experimental on the PC, `pc_detect` for Unity GLES + Oculus XR Plugin): gfxstream lists
  `GL_OVR_multiview2` but its encoder finds a multiview framebuffer incomplete and drops every draw
  (`GL2Encoder.cpp:s_glDrawElements … GL error 0x506 checkFramebufferCompleteness`): both eyes black.
- `frame.gl_shim` (experimental on the PC, GLES only, `Patch.pc_keeps`): gfxstream has no
  `GL_EXT_disjoint_timer_query`; the shim lists it and hands out stand-ins ("available, 0 ns") so GLAD engines that
  call the timer queries unconditionally don't jump to NULL. Its multiview hiding also suits gfxstream.
- FrameBridge settings without a rebuild: `frameport settings <pkg> --to pc key=value` writes
  `/sdcard/Android/data/<pkg>/files/framebridge.conf` (emulator: adb push, Android started for it if needed; direct
  mode: the file in AXRB's runtime folder); an empty value removes a key.

Per game (AXRB 1.0.4 emulator, Monado simulated HMD at 72 Hz, FramePort's PC build; Windows 11, RTX 4080 SUPER):

| Game | Build / settings | Furthest | Frame rate / time | Blocker |
|---|---|---|---|---|
| Lucky's Tale 1.0.55 (Unity 2019.4 + Oculus XR Plugin, GLES) | PC build + `frame.unity_multipass`, `scale=0.5`, adb route | **title scene drawn in both eyes** (Monado window captured) | 30.5-30.8 fps, image-send 14.5 ms | none headless; needs the headset (picture through SteamVR at its resolution, controllers) |
| Accounting+ 1.24.35 (Unity 2017.4 built-in Oculus, GLES) | PC build (`frame.unity_oculus_check`), `projection_only`, old-SDK dialog off | **stance selection screen drawn in both eyes** | 10 fps, image-send 82 ms: its OVRPlugin makes 896×1024 eye buffers whatever FrameBridge's scale/max say | its quads (OVROverlay) were dropped by `projection_only=1` (AXRB rejects every multi-layer GLES frame with -2); rebuilt with `projection_only=2` (2026-10-16, qwerty head): same screen at 9.9 fps, no regression (its controller pointers are missing there because qwerty's controllers are inactive); eye size only shrinks with the PC runtime's resolution |
| Path of the Warrior 21.11.10 (VrApi-direct GLES, own engine; from the `PATCHED/` copy) | VrApi bridge (blit copies) + GL shim (timer-query stand-ins), `scale=0.5`, `projection_only=2`, `--sim` with Monado's qwerty head | **its first screen drawn in both eyes**: the "Please ensure that your Oculus Touch Controllers are properly connected" panel (a VrApi cylinder → quad, drawn into the projection) | 38 fps, image-send ~15 ms | the simulator's controllers: Monado's qwerty builder (needed for a tracked head here, see `--sim` notes) has WMR controllers, which AXRB's bindings don't cover. Found on the way: (1) the simulated head: no tracked pose, `EnterVrMode` gave up after 30 s; (2) every frame -2 (projection + the cylinder quad); (3) black eyes: no `glCopyImageSubData` in the emulator's GLES. Before the stand-ins: SIGSEGV at pc 0 in `EgHalGrTimestampQueryGL` (NULL `glad_glGenQueriesEXT`). Needs a headset: controllers past this screen, gameplay |
| BattleSisters 1.2.4 (Unity 2019.4 built-in VR, GLES) | PC build (ovrpshim frame loop), `scale=0.5`, installed with `-g`, `--sim` with Monado's qwerty head | **main menu drawn in both eyes** (Continue / New game / Level select hub / Last Bastion / Settings) | 31 fps for 90 s, no stall | none headless. The "hang after 3 frames" was Android's permission dialog: a few seconds into its menu the game asks for a runtime permission (microphone), `GrantPermissionsActivity` became the resumed activity over the game, Unity paused (AAudio pause, no Update2/WaitToBeginFrame/BeginFrame/EndFrame in progress per `ovrp_trace`) and nobody can answer it; not the ovrpshim frame wait and not AXRB's pacing. Fixed by installing with `adb install -g` (as Lepton does). Needs a headset: controllers, gameplay |

`--sim` notes: AXRB 1.0.4's emulator host only reads `AXRB_TEST_SYNTHETIC_HMD=1` (direct mode's `=2` is newer) and
applies it only to a head *without* valid flags; Monado's simulated head is valid but not tracked, so the host never
sets its LOCAL origin and VrApi games never get a tracked head. For games on the emulator backend `frameport test
--sim` therefore runs Monado's **qwerty** builder (`SIMULATED_ENABLE=0`, `QWERTY_ENABLE=1`: its head reports
POSITION_TRACKED; host.err `AXRB OpenXR: LOCAL origin in tracking world=(0.000 1.600 0.000)`). Its controllers are
WMR ones, which AXRB's suggested bindings (Touch/Index/Vive) don't cover: no hands. `FRAMEPORT_PC_XR_SIM_HEAD=simulated`
keeps the simulated head (and its ml2 controllers), `=tracked` forces qwerty. Monado's `remote` builder (tracked head +
Index controllers) would cover both but listens on 0.0.0.0:4242 (a firewall prompt), so it isn't used. FrameBridge's
`ovrp_trace=1` (ovrpshim, Unity 2017-2019 legacy frame loop) logs every 5 s how many Update2/WaitToBeginFrame/
BeginFrame/EndFrame calls went through and which one is in progress (thread, frame, how long): where a stalled game
waits. AXRB's opt-in host capture (`AXRB_CAPTURE_PREFIX`) only starts at sequence 2000 in 1.0.4 (hard-coded;
`AXRB_CAPTURE_FIRST_SEQUENCE` isn't read), which GLES games at 10-30 fps don't reach in a test: use the Monado
window captures and FrameBridge `snapshot=N`. AXRB's guest log only keeps warnings plus AXRB's own tags: read
`adb logcat -d` for FrameBridge/OVRPlugin lines. `debuggerd -b <pid>` works from `adb shell` (stacks).

## Open (needs the owner + a headset)

- A game in the headset through SteamVR (picture, audio, controllers; Steam Frame controllers: AXRB #20).
- Pinball FX VR's Vulkan crash with the host attached happens in the emulator only; in direct mode it reaches its lobby
  headless. Needs the headset: picture/latency through SteamVR, controllers, a table played.
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
  submitted layers, 5 s apart, from frame 300) into `<data>/logs/xr-sim/`. A launch test that reached RUNNING saves
  Monado's window there too, just before it stops the game (`<time>-<pkg>-headset.bmp`, `pc_android.sim_capture`),
  and every AXRB launch test reports the frame rate from AXRB's `AXRB.Perf` summaries (game submits / bridge). Monado itself has no screenshot function;
  `XRT_WINDOW_PEEK` and `XRT_DEBUG_GUI` exist but only show windows.
- **Use:** `frameport test <pkg> --to pc --sim` (or `FRAMEPORT_PC_XR_SIM=1`): starts `monado-service.exe` hidden
  (WMI, like AXRB's scripts; ready when its pipe `\\.\pipe\<%TEMP%>\monado_comp_ipc` exists), passes the environment to
  the AXRB launch (`pc_android.launch_test(extra_env=…)`, default `xr_sim.active_env()`), stops the service afterwards
  (an already running Monado is used and left running). Service log: `<data>/logs/xr-sim-service.log`.
- **Head per backend:** when every game tested is on AXRB's emulator backend, the service runs Monado's qwerty builder
  instead of the simulated one (a tracked head; AXRB 1.0.4's emulator host has no synthetic head for Monado's
  untracked one, see the GLES section's `--sim` notes); its controllers stay inactive in AXRB.
  `FRAMEPORT_PC_XR_SIM_HEAD=tracked|simulated` overrides the choice. An already running Monado is used as it is.
- **Limits:** the simulated head wobbles slowly and the controllers hold still with no buttons pressed, so it proves
  startup, frame submission and what is drawn, not gameplay; the windowed compositor opens a visible window on the
  desktop; frame pacing is Monado's, not a headset's.
