# Device notes

Detailed device findings per game and issue, moved verbatim from CLAUDE.md (which keeps a one-line pointer
with the key fact and the patch/setting names for each). Read the matching section before debugging a game;
general runtime facts are in docs/FRAME_RUNTIME.md and symptom → fix rows in docs/PLAYBOOK.md.

## Per-game language (GitHub #354, 2026-10-17)

Asgard's Wrath 2 starts in English with no in-game language choice. Headless verification used Transformers:
`LEPTON_GFXRECON_FP_LOCALE="0\npersist.sys.locale=de-DE"` gave container
`getprop persist.sys.locale = de-DE`, while `ro.product.locale` remained `en-US`. The game's VM started with
`user.locale=de-DE`, Android configuration was `[de_DE]`, and Unreal logged `Locale=de_DE`.
Patch `device.language` sets this boot property independently of `device.hide_navbar`.
Text/voice switching in individual games, including Asgard's Wrath 2, still needs headset verification.

Recipes store the selection as `patches: {device.language: {tag: de-DE}}`. The catalog's `device` field is a
list of patch IDs, with dedicated fields for some parameterized device settings (files, environment, config sync,
foveation); it has no field for a language tag. Catalog import/export cannot currently preserve this selection.

## a32 bridge: games, lessons, debugging aids, profiling

Learned: Unity 2019 contexts
have no EGLConfig (EGL_KHR_no_config_context: the VrApi bridge takes the window's or an RGBA8 ES3 config); Unity's
floor origin = vrapi_SetTrackingSpace(1) (the VrApi bridge now honours LOCAL_FLOOR/STAGE); mapped GL buffers are
per buffer object (a per-target guest copy leaked ~10 MB/s and garbled vertices); the guest runtime's own scudo
names its maps `[anon:scudo:*]` below 4 GiB (watch them for guest leaks). The Go controller = the right Frame
controller (stick = touchpad touch, sideways > 0.6 or press = click). Gunjack (CCP, UE 4.10 NativeActivity, Gear VR,
2026-10-10): needed the native_app_glue imports (ALooper/AInputQueue/AConfiguration/AAsset*, imports.txt), an arm64
libvrapi.so for Java's System.loadLibrary("vrapi") and MOUNTED under OVP_HEADLESS_TEST; it then loads its map and
quit on purpose: AndroidMain compares AndroidThunkCpp_GetSignature() with an obfuscated copy. **Works since
2026-10-11 (owner-verified in the headset):** GameActivity.AndroidThunkJava_GetSignature() = Base64.DEFAULT(SHA-1(
signing certificate DER)) (jadx); `a32_bridge.original_signature` computes it from the original APK's
META-INF/*.RSA|DSA|EC, `convert()` stores it as `assets/fp_a32_signature.txt` for libUE4.so games, the bridge loads
it at activity creation (native_activity.c) and jni.c answers Call*Method on the method ID of
`AndroidThunkJava_GetSignature()Ljava/lang/String;` with it. After loading, Unreal leaves VR mode and enters it again on
the same GL context; a second OpenXR session there aborted in vrclient (CSharedSemaphoreLinux::WaitFromGPU from the GL
binding's PreBeginFrame, also with OVP_HEADLESS_TEST) → the VrApi bridge keeps a running GLES session across
LeaveVrMode and reuses it for an EnterVrMode with the same context (upstream-patches/runtime.cpp.patch).
Debugging aids added: A32_BREAK=<guest addr>[,..] (stack of the first hits; guest libs load at fixed addresses, e.g.
libUE4 0xf25d5000), A32_STRACE in bridge mode = path/exit calls to logcat, activity lifecycle + LeaveVrMode caller
logs. Lepton's logcat mirror often dies for these games: read `podman exec <container> logcat` instead. Gunjack input
(2026-10-11): it imports no vrapi input functions and reads the Gear VR touchpad/back as AInputEvents from its
AInputQueue (native_app_glue), so dispatchKeyEvent never reached it (no control at all) → input.c queues synthetic
events there (ALooper_poll* return the queue's ident while one waits; AInputQueue_getEvent/finishEvent and the
AInputEvent/AKeyEvent/AMotionEvent getters are hand-written "!" imports that know them; touch positions scaled to the
window size). Same mapping as AFFECTED: trigger = hold touchpad, A = tap, right stick = swipe, B = back; for NativeActivity games the stick's up/down = swipe forward/back (touchpad X: Gunjack's OnFlick = reload at ~0°, special weapon at ~180°). Not yet
headset-verified. Quest builds on the bridge (2026-10-13, Sports Scramble = a 32-bit Santa Cruz build,
UE 4.21 + OVRPlugin on VrApi 1.21): manifest requires `android.hardware.vr.headtracking` version 1
(`detect.requires_6dof`, headset "quest", ANALYSIS_VERSION 16) → convert writes `assets/fp_a32_headset.txt`
= "quest" → vrapi.c `quest_touch` (read in vrapi_Initialize via the activity's AssetManager; A32_TOUCH=1/0
overrides) passes the 64-bit bridge's Touch controllers through (enumerate/caps/state/tracking/haptics, struct
tails per VrApi minor) and starts no synthetic input. It also needed AAsset_getBuffer (guest copy, freed in
AAsset_close), AKeyEvent_getFlags and the other AConfiguration getters. Headset (owner, 2026-10-13): plays,
both controllers work. Recenter (holding the Frame's menu button) turned VrApi games ~90-120 degrees aside: the bridge's app space is LOCAL + the start yaw and the runtime moves LOCAL to the head → the bridge now takes the origin from the head again on XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING (LOCAL) and on vrapi_RecenterPose (owner-verified with Gunjack, 2026-10-13). Next: the other 32-bit catalog games (Journey of the Gods, Shadow Point: still "unsupported" until
tested), translator speed. Profiled on the Frame
(2026-10-12, `A32_PERFMAP` + perf): FPCR switches are negligible (8 of 86k samples); the cost was the run loop
(a mutex + hash-chain walk on every dispatch, a contended counter, inline-lookup slots never refilled: Gunjack
~45 % of its CPU) and `jit_invalidate` scanning every block on each Mono `cacheflush` (72 % of Spunky Dungeon's
main thread). Now lock-free lookups, ltab refill, per-thread flags, per-page block lists: Gunjack's dispatcher
23.6 % -> 1.6 %, CPU in the same phase about -55 %, still 72 fps.

## Upload speed

**Upload speed (measured 2026-09-29):** via the home router (PC Wi-Fi → router → Frame wlan0) only 15-18 MB/s, the
same for paramiko, OpenSSH and 4 parallel streams, so the network path is the limit, not the SSH code. Direct to the
Frame's hotspot (`wlanap` 10.35.78.1; the owner's PC has Valve's USB Wi-Fi dongle on it) 83 MB/s OpenSSH / 97 MB/s
paramiko; reading dumps from D: via WSL drvfs is then the cap (~82 MB/s). `Frame.fast_link()` (installer
`transfer_link`, uploads ≥64 MiB) uses usb0 > wlanap when reachable **and** the host key matches the paired Frame.

## Revive under Proton arm64 (Lone Echo)

**Revive under Proton arm64 (Lone Echo, 2026-09-29):** Revive's Detours hooks (LoadLibraryW/ExW, OpenEventW, the
signature check) don't take effect: Wine's kernelbase is ARM64EC. The LibOVR shim in the game then does a plain
`LoadLibrary("LibOVRRT64_1.dll")` search (exe dir, cwd, system32, windows, PATH — no registry, `OculusBase` or
`LIBOVR_DLL_DIR` used by this build) → `Failed to initialize Oculus API (-3001)` (= Lies Beneath's "Unable to load
LibOVRRT DLL"). A symlink `pfx/drive_c/windows/system32/LibOVRRT64_1.dll -> <base>/revive/LibReviveXR64.dll` (Wine
maps a symlink to the already-loaded module) gets past it to **-3021 = ovrError_LibSignCheck**: the shim only accepts
an Oculus-signed runtime. Next step (not done): make the runtime check pass without Detours, e.g. an injected helper
that patches the game's import table (IAT) for LoadLibrary*/WinVerifyTrust, or a prefix-level override. This is
runtime interop (what Revive does), not the Platform SDK licence check (which FramePort never touches).
Uninstall (Quest, saves kept) used to leave `deployment.json` → still "installed"; fixed (agent v16).


## OVRPort 1.2.5

**OVRPort 1.2.5 (2026-10-01, the fork's first release; CLI-only):** same commands (`patches [--json]`, `patch`, `help`,
`install`), but `patch` rejects unknown/duplicate args, unknown patch ids and an empty `--patches=`
(`tools/overport.patch` refuses empty lists). After patching it adds `libovrplatformcompat.so` itself when the platform
loader lacks `ovrMessageType_ToString` (our `frame.ovrplatformcompat` then skips: same library). New patches (all off;
`patches/overport.py`): `patch_ac_nexus_no_appsw_72/_90` (AC Nexus build 207706 only, exclusive),
`patch_disable_meta_xr_audio_telemetry` (x86_64 emulators: hidden), `patch_vrapi_openxr` = OVRPort's VrApi adapter =
the **unpatched** upstream of our `frame.vrapi_bridge` (`native/vrapi` unchanged since our 5e7df52), only usable with an
experimental CLI built with `-PwithVrApi=true` (stable jars list it but fail). The owner prefers OVRPort's fixes over
ours where they work as well (less to maintain): compare in the headset before switching a default.
`frameport install <pkg> --apk <file>` installs a specific (test) build signed with the game's key.
Headset results (2026-10-02, `PATCHED/_test-ovrport-1.2.5/TESTING.md`): OVRPort's own VrApi translator fails on the
Frame (Climb 2: requests VkFormat 37 → crash; POTW: no GLES path, missing `vrapi_GetTextureSwapChainHandle`) → keep
`frame.vrapi_bridge`; those two changes are upstream candidates. 1.2.5 builds (its platform compat, new permissions)
work. AC Nexus: `patch_ac_nexus_no_appsw_90` is the catalog default (owner preferred 90 Hz); `STRICT` patches are only
taken from a catalog recipe where `applies()` holds (the AC Nexus ones need build MAIN.450412.207706.final, else the
whole overport run fails).


## Steam Frame controller models

**Steam Frame controller models (2026-09-30, not yet seen in a game):** adapter setting `controller_models`
(`native/adapter/render_model.c`) emulates XR_FB_render_model and serves `files/framebridge/controller_{left,right}.glb`;
agent v20 (`install_controller_models`, command `controller_models`) converts the Frame's SteamVR render models
(folder name matching "frame" + left/right, OBJ+PNG, `openxr_grip` component → grip space) at finalize/set_settings.
Valve's models are never committed or copied off the Frame. Only games that use Meta's runtime controller models
benefit (manifest `RENDER_MODEL` permission/feature → suggested); **none of the 34 catalog games do** (they ship their
own meshes: that would need per-game asset replacement). Model discovery and conversion
checked on the device 2026-09-30: `/opt/steamvr/drivers/frame_controller/resources/rendermodels/frame_controller_{left,right}`
(component OBJs in model space = the whole `<name>.obj`, one `_color.png` 2048² near-black, `openxr_grip` rotates about
X only; hidden-by-default components like `status` are skipped) → ~2 MB glb each, ~1 s. Not yet seen in a game (none
of the installed builds has the new adapter, and none requests runtime models), so whether Meta's SDK attaches runtime models at the grip pose is unverified.

## Crash backtraces, Fossilize, swapchain limit, cylinder and equirect layers

**Crash backtraces (2026-09-30):** Lepton writes tombstones only to `lepton-logcats/steamlaunch-<appid>/logcat-crash.log` (after "Dumping logcat"), not launch.log; agent v23 launch tests wait for it and return `crash_log`, triage gets it via `triage(..., crash=)` (not pid-filtered: tombstones come from crash_dump's pid). The guest is `userdebug` (`ro.debuggable=1`), so Lepton's Fossilize layer loads into every app whatever the APK's debuggable flag; it has no off switch. Fossilize crashed Deadpool VR on uninitialized attachment-reference pNexts (several, in FVulkanRenderPass and the render-target layout) → `frame.vk_sanitize` = `native/vkshim` (engine's `dlopen("libvulkan.so")` string → `libfp_vk.so`, wraps vkCreateRenderPass2/KHR, drops unreadable/wrong-sType pNexts; verified: game runs). overport's dispatcher aborts on swapchains > 4096 px (`cmp wN,#4096` in xrCreateSwapchain) → `frame.swapchain_limit` (verified with 4XVR, 7680×3840 accepted by the runtime). Both are default-on (parity classifies their rewrites as
"expected"; library migration `quest_binary_fixes_v2`). The adapter shows cylinder layers (4XVR's movie screen) as ~15° quad strips (`cylinder_strips`, setting
`cylinder_strips`); the runtime composites them at full resolution. Equirect (360°) layers are dropped: drawing
them ourselves (GLES renderer in xrEndFrame: stutter, never showed 4XVR's VR video; Vulkan renderer: hung the
Frame's GPU in AC Nexus; reading the newest *released* image broke 4XVR's theatre) was removed again on the owner's
request — don't retry without a new approach. Converted layers must count as "swapped" or the original layer list
is submitted (runtime returns -2 for every frame). A focus debounce (hiding the Frame's brief
FOCUSED→VISIBLE→SYNCHRONIZED dips, which make 4XVR recenter) was also removed: AC Nexus stayed black with it (it
hid 521 ms focus changes at start). The new approach (below) is per-game, off the app's render thread, GLES-only.


## 360° layers / 4XVR

**360° layers / 4XVR (2026-09-30):** the Frame's Android SteamVR runtime (`/opt/steamvr/bin/androidarm64/vrclient.so`,
mounted in Lepton as `/data/steamvr/runtime`) only composites quad + projection layers (cube/cylinder/equirect(2)
are enum names only) — nothing to unlock. Per-game adapter settings (off by default, hooks installed only when on):
`equirect_emul` (GLES only: a worker thread with a shared EGL context converts each 360° image to a cube map when it
changes and draws one adapter projection layer from it for every frame with exactly that frame's views (the app's own
projection views, else xrLocateViews at displayTime); xrEndFrame waits ≤6 ms for the worker's CPU submit, never the
GPU. Learned in the headset: quads can't be a background (the Frame draws quad layers above all projection layers
whatever the order: hid 4XVR's balcony/controllers), and the Frame doesn't reproject a projection layer from its own
pose (images drawn only after head turns, or ahead from xrWaitFrame and sometimes late, wobbled);
`equirect_flip/face/res/fps/stereo`), `stable_local`, `focus_hold` (only after 3 s FOCUSED, dips <600 ms),
`aim_pitch/aim_yaw/aim_forward`, `refresh_rate`, `layer_debug` (diagnostics); focus_hold is default-on since 2026-10-05. 4XVR re-creates LOCAL spaces every 2–4 s
(menu recentring suspect); its theatres are baked 7680×3840 equirect2 images (`assets/100.png` …). Test 360° videos
(NASA, public domain) are in the Frame's ~/Videos; copies in `~/Downloads/frameport-360`. 4XVR's "Internal Storage"
lists its own `/sdcard/4XPlayer`, not Movies: agent v25 `link_media` hard-links sent files into an app's own top-level
folder (`app_media_dirs`), `frameport frame send --app <pkg>`, GUI game page "Add videos" (players) / "Add videos &
files…". A 4XVR webm stereo swapchain was refused with XR_ERROR_LIMIT_REACHED (-10) → right eye grey; our projection
swapchain was originally halved (1536/eye) while investigating memory. Issue #387 below confirms a swapchain
count cap; reducing image sizes does not avoid it. Frame data snapshot
(SteamVR runtimes, Lepton scripts, logs; never commit Valve binaries): `~/frameport-research/frame-data-2026-09-30/`.
Not yet verified in the headset.


**Issue #387 (2026-10-18, host-tested only):** 4XVR's extra sub-panels hit the session cap at roughly 15–16 live
swapchains: the same 256×128 create succeeds then fails in the same millisecond, so shrinking images is not a fix.
Default-off GLES `swapchain_pool` (catalog enabled for 4XVR) starts at eight live app runtime swapchains, reserving runtime
headroom for up to two 4096² SRGB atlases. Single-face/layer/sample/mip colour images use private app-context textures;
submitted panels are blitted into packed atlas rectangles after cylinder strips and rect_clamp. Destroy frees rectangles.
Vulkan is untouched. No adapter revision bump: this opt-in fix should not mark every installed game for update.
The new catalog key already makes older apps reject the recipe via `unusable_reason`; triage suggests no unknown setting.
Headset check: repeatedly open/close all 4XVR submenus while playing stereo and 360 video, verify both eyes, panel crops,
curved strips, alpha and frame pacing; confirm no new -10 errors or menu closures and reuse after closing panels.

**Issue #424 (2026-10-23, host-tested only): Vulkan panel pool.** AC Nexus (Unity, Vulkan) creates ~12 single-layer
format-43 panels (1200x1020, 768x153, 1459x1228, 855x1500, 1875x1500, 2457x1152, 512x512, 460x102, 691x153, 360x360,
…) plus a 3-swapchain eye set (arraySize 2: colour 43, 384x384 formats 97/129). With the Quest 3 identity it creates a
2592x2256 eye set while the old one lives: the runtime refuses it (-10) and the game stops drawing (the catalog kept
the Quest 2 identity until now). `swapchain_pool` now also covers Vulkan sessions: from eight live app swapchains,
format-43 panels get private VkImages copied into one or two 4096² atlases at xrEndFrame (no CPU wait on this frame's
GPU work; a 3-deep fence ring). Host harness: 8 app runtime swapchains + 1 atlas + the new eye set = 12 live, 7 panels
pooled. Catalog: AC Nexus back to Quest 3 with `swapchain_pool: 1` (`min_app` 1.3.1). Device check (headless first):
the 2592x2256 create succeeds, `pacing` continues, no `atlas update failed`; then the headset: panels upright, crisp and
in place, menus, frame time.


## Companion apps / intents (Stremio + 4XVR)

**Companion apps / intents (2026-10-03, tested with Stremio + 4XVR):** a second APK can be installed into a running
Lepton container (`podman exec -i lepton-steamlaunch-<appid> pm install -g -S <size> < apk`) and runs there, VR
included (4XVR in Stremio's flatscreen container: FrameBridge 72 fps, settings via LEPTON_ENV_FRAMEBRIDGE_CONFIG). But
Lepton's services.jar (`ActivityStarter.execute`) intercepts **every** `android.intent.action.VIEW` with data (any
scheme, explicit component or not, no property to disable): it writes `steam://openurl/<uri>` to `/lepton/steam.pipe`
(= the host Steam client's `~/.steam/steam.pipe`) and starts nothing. So "open in external player" hand-offs (Stremio
→ 4XVR) can't work inside Lepton; don't retry without Valve changing it. Lepton installs exactly one `*.apk` per app
folder (two break `get_apk_path`).

## Text input (Stremio VR)

**Text input (2026-10-03, verified in the headset with Stremio VR):** Lepton's Android has no IME (`ime list` empty)
and runs VR apps headless (`lepton.headless=true`): no Android window has input focus (`dumpsys input` FocusedWindows
empty), so neither `input text`, a USB keyboard nor Steam's keyboard reach the app. `lepton-show-flatscreen` on a VR
app keeps VR working (FrameBridge 72 fps) and gives the window focus; Steam's on-screen keyboard then opens for text
fields. Unity's TMP_InputField on non-Quest Android waits for the system keyboard (`TouchScreenKeyboardShouldBeUsed`)
and deselects a frame later unless `isKeyboardUsingEvents` (Android: `InPlaceEditing() && m_HideSoftKeyboard`);
uGUI InputField's LateUpdate keeps the field when `InPlaceEditing()`. Patch `frame.unity_text_input` rewrites them
(`mov w0,#0|#1; ret`) at Cpp2IL's **Offset** (= file offset; RVA differs by 0x4000 in Stremio's lib), Cpp2IL
2022.1 pre-release (Unity 6 / metadata v31; Il2CppDumper can't), cached per libil2cpp sha. `/dev/uinput` has an ACL
for steamos (Steam Input) → agent v33 `_keyboard` uinput keyboard ("Type on Frame") reaches everything with focus.
Installing a second APK into a container with `pm install` re-runs Lepton's post-install hook on the **main** app
(`lepton.active_app_id`; moves its files to /data/steam_app) and corrupts it on the next start (fix: touch the APK →
re-bake); `cmd_real package install` skips the hook.

## User reports 2026-10-04 (issues #4-#10)

**User reports (2026-10-04, issues #4-#10):** Steam shortcuts go to the signed-in account (`loginusers.vdf`
MostRecent) else every account (agent v35 `library_users`; it used to refuse with >1 account → Play gave Steam's "Game
configuration unavailable"); `launch` uses the shortcut's own appid and errors `NOT_IN_LIBRARY`, the app then adds it
and plays. Unity IL2CPP fixes share one Cpp2IL run (`unity_text_input.Il2cppReturnPatch`, `ALL_TARGETS`, cache keyed by
global-metadata.dat): `frame.unity_runtime_msaa_off` (OVRManager raises MSAA to 4x at runtime: "Switching to the
recommended level" → Lucky's Tale restarted the headset; `OVRDisplay.get_recommendedMSAALevel` → 0) and
`frame.unity_multipass` (Oculus XR Plugin multiview → MultiPass via `OculusSettings.GetStereoRenderingMode`; I Am Cat's
right eye grey). Both checked against Toy Master's Cpp2IL output, not yet in a headset. VR4 quits itself (System.exit
after the intro movie) → catalog `use_alt`. **Catalog changes reach existing games** without hardcoded migrations:
recipes store `catalog_rev` (= `CatalogEntry.rev()`), `library._follow_catalog` re-derives non-user recipes whose entry
changed; a maintained entry beats the user's shared copy when its `updated`/verified date is later (`catalog._newer`).

## Round 2 of reports, headset rounds 2-3 (2026-10-04)

**Round 2 of reports (2026-10-04):** VR4's campaign hang = one fragment shader reading an uninitialized loop counter
(reporter's capture): vkshim `vk_shader_fix` setting (`<size>:<sha256>:<offset>:<words>`, from the recipe's `adapter:`;
matched by size + SHA-256, words inserted into a copy) — on the device the module is fixed at load ("fixed shader
module (6488 -> 6512 bytes)"); campaign itself not yet seen in a headset by us. Text settings (kind "str") stay out of
the Game settings dialog (saving it dropped them). I Am Cat: `GetStereoRenderingMode` is inlined (no call sites) →
`Il2cppReturnPatch.field_loads` rewrites the field read in `OculusLoader.Initialize` (Cpp2IL gives the field offset,
`field:<name>`); verified with Toy Master: eye swapchains array=1 ×2 instead of array=2. Builds record `recipe_fp`
(`patches/base.recipe_fingerprint`, `Patch.revision`): a changed recipe or revised patch shows "Update on Frame".
Remembered Frames are found again after an address change (`Frame.relocate`: scan, same SSH host key, before
login). Packaged apps show artwork by file path (`thumbs.use_file_paths`, #16). Diagnostics: this boot's kernel log.
Device tests of requested games (2026-10-04, headless): I Am Cat, Myst 3.3.0, BattleGlide, Blade & Sorcery: Nomad
start (RUNNING, frames). Roblox: SIGSEGV in je_free from libroblox.so ~3 s in (likely its anti-tamper vs the re-signed
APK). BONELAB (Unity 2021.3 Vulkan, OVRPlugin 1.94): SIGSEGV with pc == fault addr in vkCreateInstance, called from
libSLZQuestNative.so's Vulkan hooks into an unmapped (unloaded) library right after OVRPlugin's pre-init
xrDestroyInstance; pinning openxr/vrclient libraries (RTLD_NODELETE) did NOT help — find the unloaded library with the
linker's dlopen/dlclose logging next. Accounting+ (Unity 2017.4 built-in Oculus, libOVRPlugin + libvrapi): Unity
never starts VR on the Frame (no OVRPlugin/OpenXR lines; runs as a 2D app), which is why its "controller setup"
screen can't be passed — Unity 2017's Oculus device check, not the controller profiles. Cpp2IL can't read Unity 2017.
The Frame's Android runtime (vrclient.so) knows oculus/touch_controller but not Meta's Touch Plus/Pro profiles
(OVRPlugin suggests those too, routinely) → adapter `profile_remap`.
Headset round 2 (2026-10-04): the Frame drops focus often (ms to 7 s; only some are presence/standby) → games pause
or recentre (B&S Player.OnVRPresence → Teleport, BattleGlide/Unreal pauses); focus_hold now 1 s focused / 1 s max dip
(longer must still pause). Myst crash = vrclient xrSyncActions race after FOCUSED → `sync_guard` (confirmed); its
object glitches: app space warp suspected (FB_space_warp is advertised and used) → recipe `patch_disable_space_warp`.
I Am Cat multipass: poses/times consistent, but every frame is submitted after the next xrWaitFrame (one period late;
Zink + doubled draws) and the game clamps its physics step to 10 ms → judder while still; testing scale 0.8. Roblox's
newest crash is on Fossilize's recording thread → vk_sanitize extended to own-engine libs (engine "Other").
Accounting+: unity_oculus_check + native/ovrpshim (libfp_ovrp.so waits via ovrp_WaitToBeginFrame before Unity
2017's ovrp_Update2) → VR in the headset at ~72 fps (2026-10-04), but stuck at "press any button" although input
reaches its OVRInput cleanly (probe: both Touch connected 0x63, individual buttons, input focus 1) → works with issues.
"outside of frame bounds" warnings stay (~2/frame) and are harmless.
Steam library (GitHub #4/#21/#27, agent v38): shortcuts go to every Steam account (signed-in first; MostRecent isn't
always the account on the Frame); stop_steam also waits for Steam's helpers; after the restart the agent re-reads
shortcuts.vdf and writes once more if Steam put its old copy back (`shortcuts_lost`); lost again (agent v89): Steam
ignores FramePort's entries on that Frame, so `add_devkit_entry` adds the devkit entry instead (`added` gets `via:
devkit`) and only reports an error if that fails too. 0.6.3's Play
auto-repair (`_add_then_play`) crashed (its job got a Job, not a reporter). Uploads resume on a transient OSError.
Lepton's Android 11 has no clipboard service (134 services; checked with `podman exec … service check clipboard`):
SDL/LÖVE apps crashed at start → `frame.sdl_clipboard` (`apk/dex.py`: in-place dex edit, nops one invoke, fixes
the header checksum/signature; verified with LÖVE for Android 11.5, GitHub #24 Dramatic Shape). Godot 4.2-4.4 (Kotlin `as ClipboardManager`,
GitHub #134 EndoparasiticVR) → `frame.godot_clipboard` (nops the `Intrinsics.checkNotNull` before that check-cast; Godot's
clipboard methods return at once; analysis `godot_clipboard`, ANALYSIS_VERSION 10; checked on real godot-lib/templates
4.2.2-4.4.1, not on the device; 4.1 and 4.5+ don't throw). 2D apps (vr_kind none)
keep every suggested patch that isn't about VR (`needs_vr = False`), not a fixed list. FramePort on the Frame:
`frame/local.py` (127.0.0.1, own key authorized; "This Frame (experimental)"; app data in
~/.local/share/frameport-app because ~/.local/share/frameport is the agent's) and Steam library changes wait while
Desktop Mode is open. Verified on the device 2026-10-04 (install + launch test from the Frame). Library updates on the
Frame run one at a time (agent v40 flock): two quick uninstalls used to bring a removed shortcut back (Roblox).
Headset round 3 (owner's verdicts, 2026-10-04): Blade & Sorcery works — its freezes were the headset's wear sensor
flickering off while worn (vrserver.txt "HMD off/on" 0.5-2 s) → per-game `focus_hold_ms=2500` (only this game reacts;
not a default). BattleGlide works (focus_hold). Myst works with issues (object glitches; space warp ruled out, its
recipe keeps it on). I Am Cat works with issues (judder; scale 0.8 no help). Roblox unsupported (je_free in
libroblox, also with the Vulkan shim). Installs skip the Steam restart when the shortcut is unchanged and wait while a
game runs (agent v37). New default fixes in FramePort's code reach games already in a library: after every app update
`library._follow_catalog` re-derives each non-user recipe once (setting `recipes.app_version`; tests switch it off via
`library.REFRESH_ON_UPDATE`). Analysis fields added later: **bump `analysis/detect.ANALYSIS_VERSION`** (stored as `analysis.extra.analysis_version`); older entries whose APK is still there are analysed again at the GUI's start (background thread, not a job: ~8 s per 900 MB APK) and before a build (`pipeline.refresh_analyses`, GitHub #104); only `analysis`/`suggested` change, user recipes stay; an unreadable APK gets `analysis_failed` = the version (not retried until the next bump). Troubleshooting techniques: docs/PLAYBOOK.md "Debugging techniques". Unity `boot.config` "vulkan" substring mislabels GLES games as Vulkan
(I Am Cat ran GLES); OVRPlugin's "Unavailable OpenXR extension: XR_FB_scene" is routine (no longer triaged).

## Round 4 (2026-10-04): surface swapchains, save-folder permissions

**Round 4 (2026-10-04):** XR_KHR_android_surface_swapchain is listed by the Frame's runtime but returns
FUNCTION_UNSUPPORTED → adapter `surface_emul` (default on, `native/adapter/surface_swapchain.c`): an ordinary runtime
swapchain is returned, the game gets a Surface from a SurfaceTexture on a worker thread (own EGL context, JavaVM from
XrInstanceCreateInfoAndroidKHR), frames are read back (glReadPixels) and uploaded in xrEndFrame (Vulkan: own command
buffer + fence per panel, never waits; GLES: glTexSubImage2D). I Am Monkey's intro works (owner-confirmed). SUPERHOT
(Quest) quit at start: its cloud save folder was mode 1700 and Lepton's app writes through the folder's group →
launch.sh `fix_perms` adds u+rwx,g+rwx (agent v41). Lucky's Tale: Unity's Loading.PreloadManager thread grows the
native heap ~1 GB/s until the OOM killer (Frame freezes); no fix. `perf` works as steamos (paranoid 2; ptrace_scope 1
blocks gdb/eu-stack/debuggerd) — see PLAYBOOK "Debugging techniques". QuestCraft downloads its JRE at first run into
`files/runtimes/JRE` (Pojlib Installer); on the Frame it was missing (unpacked by hand, unverified). WiiCompiled: its launcher shows
(lepton-show-flatscreen) but imports .wcgame files through Android's document picker (OPEN_DOCUMENT), which Lepton
lacks (ActivityNotFoundException) → imported by hand: libmain.so + game.json → internal files/game/<profile>,
DATA/ → external files/WiiCompiledOpenXRVR/DATA, MOD/ → .../RetroRewind6 (decompiled with jadx). Lepton has no
picker at all: a FramePort fix would be an injected picker activity. Generated cover/banner art used to suppress the
Steam placeholder set (no Steam art at all) → fixed in artwork/steam.py.

## Steam library failures and the devkit fallback (GitHub #21/#30/#41/#42)

**Steam library failures (GitHub #21/#30):** on some Frames Steam never loads FramePort's shortcuts.vdf entries (same SteamOS build and arm64 beta client as the dev Frame; IDs correct; file written before Steam starts): Play → console_log `GameAction [AppID <id>] … RequestingLicense → UpdatingAppInfo → LaunchApp failed with AppError_9` (= "Game configuration unavailable", Steam treats the id as a store app). Cause unknown; agent v42/43 diagnostics: `launch` returns `steam` (started / error + code / silent from console_log), `collect_diag` adds `steam_library` (real Steam dir, beta, Steam start vs vdf write time, entries per account, devkit games) + `steam-console.txt` (incl. Steam's `logs/shortcuts.previous.txt` if present; steamclient.so has "LoadShortcuts: rejecting attempt to load shortcuts: invalid account ID"). **Fallback (agent v43):** when Play gets AppError_9 the agent registers the game through Steam's devkit interface (what Valve's Devkit Management Tool does, MIT: `devkit-1 steam://devkit-1/<~/.steam/steam.token>/create-shortcut?response=<file>&gameid=<id>&directory=~/devkit-game` written to ~/.steam/steam.pipe; answer file / `.error`; `~/devkit-game/<id>/launch.sh` = link to the game's launch.sh, `<id>-argv.json` ["launch.sh"] (relative to the folder), `<id>-settings.json` {steam_play 0, compat_tool ""}). Steam adds it live (no restart), picks its own appid (read back from shortcuts.vdf by DevkitGameID), shows it as a normal non-Steam game named "Devkit Game: <id>" (OpenVR 0, but VR works: owner-verified 2026-10-04), art copied to grid/<appid>*. Ids: letter first, then letters/digits/_ only (spaces, '-', leading digit → "missing/invalid arguments"). Later Plays use the devkit entry; uninstall (`devkit_unregister`, `delete-shortcut` live) and purge remove it. Verified on the dev Frame: register/launch/unregister; the AppError_9 trigger itself only in unit tests. GitHub #42 (two Steam accounts, neither MostRecent): Steam rewrote the active account's shortcuts.vdf without FramePort's entries right after starting, and added the devkit entry live without saving it, so `devkit_appid` (vdf only) failed ("Steam added the devkit entry but didn't save it"); agent v55 `devkit_appid_from_log` reads Steam's `sanitize shortcut app id "~/devkit-game/<id>/launch.sh": replacing 0 with N` console line. GitHub #41 (one account): same Steam behaviour, and every install restarts Steam, which forgets the never-saved devkit entry. Agent v56: the log fallback reads only the current session's console_log.txt; Play re-registers a devkit entry that gets AppError_9; art updates also write the devkit entry's grid art (`copy_grid_art`). Open: why some Frames' Steam never loads or saves shortcuts.vdf (single account too).

## Exit game (GitHub #36)

**Exit game (GitHub #36, 2026-10-04):** `Apps.TerminateApp` (what Exit game calls; reachable for tests through Steam's CEF devtools on 127.0.0.1:8080, SharedJSContext, `Runtime.evaluate` — a stdlib websocket client is enough) closed both a normal shortcut and a devkit entry on the dev Frame within 1-3 s. SIGTERM to Steam's `reaper` alone left launch.sh + Lepton (setsid) + the container running (an earlier headset launch ran on for 87 min); SIGTERM to launch.sh cleans up in ~4 s. Agent v44: launch.sh's 2 s loop ends the game when its parent (the reaper) is gone; `upgrade_launchers` (from ensure_host_fixes) adds that to existing launchers in place. Which in-headset exit path fails is still unknown. Screenshots on the Frame: all under SteamVR's appid 250820 (`760/remote/250820/screenshots`, screenshots.vdf has creation time, no game).

## SteamVR dashboard at game start

**SteamVR dashboard at game start (2026-10-04):** the dashboard (Resume game / controller / VR options) was open whenever a FramePort game started (also over Lepton's 2D launcher). Steam's UI exposes `SteamClient.OpenVR.VROverlay.{IsDashboardVisible,HideDashboard,ShowDashboard}` (CEF devtools, `steam_js` in the agent). Agent v44: launch.sh starts `_dashboard_worker` (waits for FrameBridge's first `pacing:` line in launch.log, then hides a visible dashboard for 25 s, at most 3 times; log `<base>/dashboard.log`; opt out with env FRAMEPORT_KEEP_DASHBOARD=1). Verified on the dev Frame (dashboard shown → 4XVR launched via Steam → hidden ~5 s after the first frames); not yet seen in the headset. **Headset (ITR2, 2026-10-05): too late** — Steam shows its frame menu (`valve.steam.gamepadui.frame.menu`, vrwebhelper_systemui.txt `[PooledPopups] Showing`) ~0.3 s after the game's first submitted frame (FrameBridge `new layer:`), but the first `pacing:` summary comes ~8 s later; the owner had pressed Resume (`[HideDashboard] return_to_game`) before the worker looked. Agent v52: watches from the first `new layer:` line (incremental log reads, 0.5 s polls, 30 s window). Agent v59: watches 120 s and hides up to 10 times (the menu came back after 30 s in the owner's sessions). Agent v53: stops for good once the player opens the dashboard with the controller (`toggle_dashboard_action` in vrwebhelper_systemui.txt since the worker started): v52 closed it 60 ms after each press, and the game, paused for it, stayed paused.

## Space warp / ITR2

**Space warp / ITR2 (2026-10-04):** Into The Radius 2 (UE5, `libUnreal.so` + OVRPlugin, Vulkan) uses Application SpaceWarp (extra swapchains `376x376 format=97` motion vectors + `format=129` depth); #35 reports flickering textures. OVRPort's `patch_disable_space_warp` has no effect on UE5, and hiding XR_FB_space_warp in FrameBridge's enumerate doesn't reach the game either: **OVRPort's dispatcher offers/enables XR_FB_space_warp itself**. FrameBridge `hide_space_warp` (per game, off) hides it and strips `XrCompositionLayerSpaceWarpInfoFB` (1000171000) from the projection views (log `hide_space_warp: removed space warp info`); the game still renders its MV swapchains. Test build installed on the dev Frame; flicker not yet checked in a headset. Heuristic: UE5 + OVRPlugin shows it as an option (off). `equirect_emul` is suggested for Unreal games whose graphics API isn't detected ("GLES or unknown"); on Vulkan it switches itself off (Myst/Riven/ITR2): harmless, but Unreal Vulkan detection is a known gap.

## PR #34: Meta XR Audio metadata (Batman)

**PR #34 (merged 2026-10-04, Lucas-Mathieu):** `native/adapter/audio_metadata.h` patches Meta XR Audio Wwise (only build ID e1619e7f…, Batman: Arkham Shadow 1.4.1) so queued audio metadata isn't freed while the current audio frame references it (smoke-bomb crash). The shared detour now publishes one aligned AArch64 branch to a range-checked nearby stub, with a one-instruction trampoline; protection failures roll back (published mappings must remain alive for in-flight threads). See [AUDIO_GEOMETRY.md](AUDIO_GEOMETRY.md) for failure limits and host validation; this publication change has not been device-tested. Original repair verified headless: installs ("metadata reclamation follows …"), 72 fps; we added a once-per-second scan limit. Outside PRs: review source, rebuild `artifacts/` ourselves, merge locally (contributor's commit kept), push main.

## Heuristics eval (2026-10-04)

**Heuristics eval (2026-10-04):** `scripts/eval_heuristics.py "<VR CyberDeck Portable>"` → 19/44 recipes exact, 322/352 fields (most diffs: `frame.unity_text_input` suggested where older catalog recipes lack it; the rest are runtime-only findings: use_alt, vk_shader_fix, focus_hold, sync_guard, multipass). `PATCHED/` copies aren't scored (already converted).

## Proton default (stable vs Experimental)

**Proton default (agent v51, owner's choice 2026-10-05):** the newest **stable** ARM64 Proton is the default for PC VR games (`pick_proton`: asked-for name/alias, else newest installed stable, else any stable). Agent v49-50 defaulted to Experimental; the owner's headset A/B with Rick and Morty (Experimental 69 % reprojected frames, stable felt much smoother) reverted it. The game page's Customize shows `pcvr.proton_tool` as a dropdown (Stable (default) / Experimental = alias `proton-experimental`); `ensure_proton` installs a chosen one (it asks once more for the runtime Steam only names after Proton is installed; Experimental installed unattended on the device in 53 s). A PC VR launch test that fails on stable suggests Experimental (`pipeline.PROTON_TOOL`, `proton_alternative_worth_trying`, button "Try Proton Experimental and reinstall"). Remaining Rick and Morty stutter on head turns = the game rendering below the refresh rate (reprojection), not Proton.

## USB cable link

**USB cable link (verified 2026-10-05, cabled to the dev PC):** `usb0` is a USB **NCM** gadget (configfs g1: `ncm.usb0` + `ffs.adb`, Valve 28de:2460) set up by `usb-ncm-gadget@usb0.service`, which only runs with `ConditionPathExists=|/etc/systemd/system/adbd.service.d/steamos-devkit-enabled` = **Developer Mode only**. `usb-ncm-dnsmasq@usb0` serves DHCP with fixed `dhcp-host` entries: Frame 10.86.200.233, PC **10.86.200.234** (`frame/usb.py` FRAME_USB_IP/PC_USB_IP). Windows 11 binds its built-in "UsbNcm Host Device" (no driver); WSL mirrored sees eth 10.86.200.234. USB is high-speed (USB 2.0): upload 36.9 MB/s vs 11.3 MB/s home Wi-Fi. Frame→PC over the cable worked without a firewall change. `scripts/usb_autotest.py` (Windows via powershell.exe from WSL), `frameport frame usb-check`. Wizard "Set up with a USB cable" (`PairingServer(host=PC_USB_IP)`; the real bootstrap ran end to end over USB). Discovery dedupes by SSH host key and prefers USB > Frame hotspot > network (`discovery.dedupe`); a lost address is re-found by key (`Frame.relocate`, tested with a dead saved address).

## Start activity / QuestCraft / ITR2 pop-in

**Start activity / QuestCraft / ITR2 pop-in (2026-10-05):** WiiCompiled's VR part (`QuestActivity`, SDL, process `:game`) stays behind Lepton's flat window when the launcher is shown (`device.text_input_window`). `frame.start_activity` (opt-in; analysis `extra.vr_activity` = the original APK's activity with Meta's VR category when it isn't the launcher) gives that activity's filter LAUNCHER and every other LAUNCHER becomes INFO, per element (`axml.set_start_activity`); Lepton's own `apk-info-extractor --print-activity-name` (on the Frame under Lepton/liblepton/apk_extractor) then answers QuestActivity. **OVRPort gives every MAIN activity LAUNCHER + VR categories**, so the VR activity must come from the original APK's analysis, not the converted manifest. Lepton has no activity override (`APP_ACTIVITY` is reset when app_metadata.sh is sourced). QuestCraft reaches Minecraft 1.21.5 (Fabric, Vivecraft, Sodium; LTW "Large Thin Wrapper" GL 3.0 on the Frame's Zink) and stops at `OpenGL error 1282` in `WindowFramebuffer` createTexture (Minecraft's crash report in `files/instances/1.21.5/crash-reports/`, Pojlib's `files/latestlog.txt`); the vrclient SIGSEGV / "pthread_mutex_lock called on a destroyed mutex" afterwards is only the shutdown (Unity's render thread still in vrclient while the instance is destroyed). ITR2 "models popping in and out" in game: Unreal reads user config from **internal** storage (`lepton-data/internal/<pkg>/files/UnrealGame/<Project>/<Project>/Saved/Config/Android/Engine.ini`, not external); `r.AllowOcclusionQueries=0` there broke its menus (removed again). Vulkan shim `vk_query_slots` (two slots per occlusion query) never saw ITR2 create an occlusion query pool, and the run with it crashed (Unreal RenderThread SIGSEGV, caught by sentry-native: no tombstone; the backtrace is in `cache/sentry/*/.sentry-native/*.run/*.envelope` with module offsets) - left off. Next suspect: Unreal 5 mobile HZB occlusion reading depth stored with Valve's FDM injection layer; test = launcher `VK_INSTANCE_LAYERS=''` (Lepton then loads neither VALVE_rpo nor VALVE_fdm_injection, Fossilize still). QuestCraft: LTW overrides glGetError and returns 0 with `LIBGL_NOERROR` set; Lepton passes `LEPTON_ENV_<NAME>` to the app as `<NAME>`, so catalog `lepton_env: {LEPTON_ENV_LIBGL_NOERROR: '1'}` gets Minecraft past the GL error 1282; then black picture (see the catalog entry). FrameBridge logs `new layer` once per layer type per process and projection views only for the first frames, so a second session in the same process (Vivecraft after Unity) needs `layer_debug=1`.

## Vulkan validation / ITR2 solved

**Vulkan validation / ITR2 solved (2026-10-05):** Khronos' validation layer can't come from Lepton (its layer dir is in the read-only guest image; the Linux copies on the Frame are glibc) and a global `debug.vulkan.layers` property (via the LEPTON_GFXRECON_FP_PROPS trick) kills the container at boot. Working recipe: bundle `libVkLayer_khronos_validation.so` (Khronos android-binaries release, arm64-v8a, stored uncompressed) into a test APK re-signed with the game's key (`apk.sign.sign`), install with `frameport install <pkg> --apk`, and set the Vulkan shim's `vk_validation=1` (it adds the layer at vkCreateInstance; findings under the logcat tag VALIDATION in launch.log, each message ID capped at 10). ITR2's flicker + windows behind models = Unreal's Qualcomm shader-resolve subpasses with a depth resolve of a single-sampled depth attachment (VUID-04908/-03179); `vk_spec_fixes=1` drops that resolve (its TRANSFER_DST half never applies to ITR2: the main depth is transient). Valve's VALVE_rpo layer removed ITR2's fog (catalog `lepton_env: VK_INSTANCE_LAYERS: VK_LAYER_VALVE_fdm_injection`). `r.ViewDistanceScale=2` in the internal Saved/Config/Android/Engine.ini only exists on the owner's Frame (the catalog can't write internal storage yet). Launch tests: Lepton prints a transient "is not a running context" on the first start after an APK change (agent v54 `not_started` waits 30 s for "Boot complete!").

## GitHub #38/#39: PowerWash Simulator, The Room VR

**GitHub #38/#39 (2026-10-05):** PowerWash Simulator stuck at "Waiting.." on a reporter's Frame = Unity sized the right-eye image rect past its swapchain (rect 268+1656 on a 1920-wide swapchain; the runtime's recommended size differs per Frame) and SteamVR rejected every frame (`xrEndFrame failed -25`, XR_ERROR_SWAPCHAIN_RECT_INVALID), after which the game stopped calling xrEndFrame. FrameBridge `rect_clamp` (default on; sizes noted in xrCreateSwapchain; clamped layers count as swapped) keeps projection/quad rects inside their swapchain; triage `swapchain-rect-invalid`. Not reproducible on the dev Frame with `scale` (Unity's eye sizing isn't just the recommended width); no regression there (72 fps). The adapter's revision was NOT bumped (that would mark every installed game outdated): affected users rebuild. The Room VR (#38) = Unity's render thread SIGSEGV in libgallium_dri.so ~30 ms after the game recreates its eye swapchain with samples=4 (FrameBridge retries samples=1); the game's own code turns MSAA on (not OVRManager); candidate fix: hide GL_EXT_multisampled_render_to_texture (GL shim) - needs the APK to test. The launch test passed both (render thread dead / no frames): triage gap.

## Headset round 2026-10-05 (evening)

**Headset round 2026-10-05 (evening):** I Am Cat's judder = repeated xrLocateViews of one display time returning
slightly different poses → FrameBridge `pose_consistency` (owner-verified, works). Lucky's Tale freeze on touching a
save slot = OVRPort's OpenXR dispatcher converting XR_FB_haptic_amplitude_envelope with ns read as s (GitHub #9) →
xrshim `haptic_fix` (owner-verified); its remaining short pauses were the wear sensor ("HMD off/on" 0.5-1 s in
`logs/eyetracking.txt`, not vrserver.txt) → **`focus_hold` is on by default with 5 s** (owner's choice; C defaults
too). The Room VR works (`unity_no_overlay_copy`). BattleSisters (Unity 2019.4 built-in VR, no libOculusXRPlugin.so)
flooded "outside of frame bounds" ~2400/s and hung the GPU (kernel hangcheck, zink DEVICE LOST) → the ovrpshim frame
wait now also applies to Unity 2019 without the Oculus XR Plugin (`UnityOculusCheck.legacy_loop`). Vader Immortal
(UE4, stuck on the loading image after the intro, GitHub #49): VRP repacks carry a Frida gadget (`libfrda.so`,
config `hijack_responses`), but OVRPort's `patch_clean_up_frida` removes its loadLibrary call, so it never runs on
the Frame; `frame.ovr_trace` (opt-in, `native/ovrtrace`: 1138 exported ovr_* stubs → real loader functions, logcat
tag `fp_ovrtrace`: calls, PopMessage answers, unanswered requests every 10 s) is the next diagnostic.

## Unity built-in VR input (Accounting+, BattleSisters)

**Unity built-in VR input (2026-10-05):** probe builds (`FRAMEPORT_INPUT_PROBE=1 frameport build …`; it logs
trigger/grip crossings too) showed OVRPlugin returns the full Touch state for every controller mask (also Go masks);
presses reach the games' OVRInput. Accounting+ (Il2CppDumper v6.7.46 reads its Unity 2017 metadata v24, run on Windows)
passes stance selection (NewtonVR grip/trigger) but its motion warning waits for `Input.GetMouseButtonDown(0/1)`,
which Lepton never delivers (no focused Android window) → ovrpshim registers its own
`UnityEngine.Input::GetMouseButtonDown(System.Int32)` icall (il2cpp_add_internal_call, after Unity's: resolve first)
that adds a click in the frame a Touch trigger or A/B/X/Y is newly pressed (`unity_oculus_check` revision 3).
BattleSisters (Unity 2019 InputSystem/XR InputDevices via `VrHandInput`): libunity's Oculus module (OVRPlugin
function table: a global pointer at 0x16b57c0 in this build, slots filled by name, e.g. +0x160 GetControllerState,
+0x168 State2, +0xe8 GetNodePresent) only reports controllers when `strncmp(deviceModel, "Oculus", 6) == 0` (next to
the Go check `deviceModel == "Oculus Pacific"`); Lepton's model is "Valve Lepton" → Go/unknown → only Go masks polled,
buttons dead. `unity_oculus_check` revision 4 (`oculus_model_checks`, Unity 2019 only) turns the 17 compares' length
into 0 (`orr w2, wzr, #6` → `mov w2, #0` after the adrp/add of the "Oculus" literal; the string stays: it is also
Unity's VR device name). Device result: libunity now polls `ovrp_GetControllerState(0x3)` (Touch); owner-confirmed
2026-10-06: buttons work. Its first controller vibration then hit OVRPort's haptic-envelope bug (11 GB, OOM kill) →
`haptic_fix` is now suggested for every game with libOVRPlugin.so (settings detect), BattleSisters works. Ruled out before: ProductName, GetNodePresent, the device-model string itself, exports.
Accounting+ works (owner, 2026-10-06).

## Vader Immortal (headless investigation)

**Vader Immortal (UE4, GitHub #49, 2026-10-06, headless):** stuck after the intro on an in-game image (the splash
quad ends ~6 s in; then the game's own projection frames, 72 fps, balanced xrBeginFrame/xrEndFrame). Not the Platform
SDK (`frame.ovr_trace`: only user + entitlement, both answered) and not the repack's Frida gadget (OVRPort's
`patch_clean_up_frida` removes its loadLibrary). It **leaks ~430 GPU mappings (/dev/dri/renderD128) and ~20 MB a
second** (6.5 GB + swap after 5 min, then 26 fps): page-fault stacks (perf -e page-faults, offsets resolved with the
process maps + vrclient.so's own symbols; its text segment is at file offset + 0x4000) end in SteamVR's runtime:
`xrBeginFrame → CSxrCompositorOpenVR::BeginFrame → SubmitExplicitTimingData → CVRCompositorSharedTextures::
BeginGPUTimingCommandBuffer` and `xrEndFrame → CVRCompositorClient::SubmitWithArrayIndexAndTime`. Ruled out: the
layer color scale/bias + image layout structs (FrameBridge `strip_color_bias` 1/2, diagnostic), Valve's Vulkan
layers (VK_INSTANCE_LAYERS=""), array swapchains in general (Lucky's Tale/I Am Cat flat). Kernel tracepoints aren't
allowed for steamos. FrameBridge: `layer_debug` logs xrDestroySwapchain and per-5 s xrBeginFrame/xrEndFrame counts;
`frame_balance` (ends an open frame before the next begin) exists but Vader never leaves one open. Next (built,
not yet run: the Frame slept): `layer_debug` also counts xrAcquire/Wait/ReleaseSwapchainImage per 5 s (hooked only
with layer_debug, after the other conditional hooks) to see whether Vader skips a wait or release: it doesn't (361/361/361 per 5 s).
FrameBridge `snapshot=N` (`snapshot_gl.c`, GLES): every N s the left-eye image the game submits is read back on its
own context and saved as `files/fb_snap_0-7.ppm` (quarter size) — headless launches show a black headset view, this
shows what the game draws. Vader: Lucasfilm logo (an OBB mp4: video works), then its loading card (portrait + segmented
bar) that never advances; the async loader thread sleeps and OBB reads stop (~168 MB of a 2.7 GB pak).

## SteamVR overlay apps

**SteamVR overlay apps (agent v75, 2026-10-10):** host OpenVR overlays (VRApplication_Overlay) are composited over Lepton (Quest) games on the Frame (probe checker seen over 4XVR in the headset view, `native/vroverlay_probe`); Windows ones register through Proton's vrclient too (Temporal Reality's Windows build created its overlays; fpsVR never reaches OpenVR under Proton). Detection `analysis/vroverlay.py` (bundled `.vrmanifest` with `is_dashboard_overlay`, else OpenVR + IVROverlay + self-registration markers, engine Other; Rift fingerprint `RIFT_ANALYSIS`), patch `pcvr.vr_overlay` (param autostart), Linux entry fields `vr_overlay`/`vr_overlay_autostart` (game page switch); agent `register_vr_overlay`/`unregister_vr_overlay`/`launch_vr_overlay` (IVRApplications via ctypes, live; manifest `<anchor>/frameport-overlay.vrmanifest`, the app's own key, binary = launch.sh). **SteamVR on the Frame reads only `binary_path_linux_arm`** (binary_path_linux alone is skipped). Overlay apps: no launch test, no Linux Steam-parent watchdog (`FRAMEPORT_OVERLAY`), not a "running game". A Steam overlay-app shortcut and a game shortcut run together. SteamVR's SetApplicationAutoLaunch didn't persist on the dev Frame (the watch never started by itself) → **agent v76: FramePort's own autostart**: the deployment's `overlay.autostart` is the source of truth; `ensure_host_fixes` (`ensure_overlay_service`) keeps the user service `frameport-vr-overlays.service` (`_vr_overlay_watch`, 5 s polls of the known vrserver's /proc stat) only while an overlay app has autostart on (register/unregister, uninstall, purge and the kill switch `~/.local/share/frameport/vr-overlays.disabled` / `FRAMEPORT_NO_OVERLAY_AUTOSTART` update it); a new vrserver (pid + start time, kept in `~/.cache/frameport-vr-overlays.json` across agent updates) → child `_vr_overlay_round`: wait for IVRApplications, 15 s grace, `launch_vr_overlay` for apps not running (folder process or GetApplicationProcessId); log `~/.local/share/frameport/vr-overlays.log`. Dev Frame: adopted the running vrserver, skipped the already running watch; a start after a real SteamVR start/boot is not yet seen. Details: docs/FRAME_RUNTIME.md "Overlay apps".

## Vader's leak solved / AC Nexus flips

**Vader's leak solved / AC Nexus flips (2026-10-14):** Vader Immortal leaves GL errors pending on its context; SteamVR's GL interop (`openxr_graphics_gl_openvr.cpp`, `xrclient_<pkg>.txt` in Steam's logs: `SXR_GL_CHECK: error:0x502 ... glGenTextures`) takes the stale error as its own failure once, and from then on `CVRCompositorSharedTextures::BeginCommandBuffer` allocates a command buffer every frame (perf page-fault stacks; ~430 renderD128 mappings + 22 MB/s). Adapter `gl_error_clear` (GLES, catalog on for Vader ep1-3) clears them before xrCreateSwapchain/xrBeginFrame/xrEndFrame: flat memory, no SXR_GL_CHECK (headless). The per-game `xrclient_<pkg>.txt` logs are worth reading for runtime errors. The Frame's runtime caps swapchains per session (XR_ERROR_LIMIT_REACHED from ~15; AC Nexus asks for more): flip emulation now turns quads upright in place (scratch VkImage, once per acquire; transfer usage added to single-layer colour swapchains) instead of a shadow swapchain per quad, which ran into the cap (#138). Headless sessions don't draw AC Nexus's panels (adapter `flip_dump` saves before/after images of flipped quads, for headset runs). 2026-10-23: panels that stayed upside down at random were blits that took longer than the 100 ms wait while the GPU was loading; they were counted as failed and flipped again next frame, so both blits ran. A submitted blit now counts as done (log `flip: blit … still running after 100 ms`), and the shared command buffer waits for it before reuse (`vk_cmd_idle`, also fade_quad/cube_standin). Equirect2 layers (type 1000091000, `flip_tag=1 usable=0`) are dropped, not shown upside down.

## Unreal's self-restart

**Unreal's self-restart (agent v90, 2026-10-13):** UE 4.21+ restarts the app once after its first-run shader
precompile ("optimizing" screen; `r.ProgramBinaryCache.RestartAndroidAfterPrecompile`): it starts its own LAUNCHER
activity and calls System.exit(0); Lepton ends the container with the first process (the restarted one with it).
Skipping `AndroidThunkJava_RestartApplication` or the cvar = 0 (accepted via UE4CommandLine.txt `-ini:`) left Sports
Scramble on that level for good. launch.sh (RESTART_BLOCK, `upgrade_launchers` adds it) starts Lepton once more when
`_restart_requested` sees the app's own LAUNCHER start (system log, uid >= 10000) <= 5 s before its exit; first run
kept as `launch-before-restart.log`; `FRAMEPORT_NO_RESTART=1` off. Owner-verified with Sports Scramble's first start (2026-10-13).

## xrWaitFrame runs ahead (Journey of the Gods)

**xrWaitFrame runs ahead (Journey of the Gods, 2026-10-14, owner-verified):** with real frames (headset on) the
Frame's runtime sometimes returns from xrWaitFrame at once with a display time less than one refresh after the frame
just submitted, and then rejects the second submission for that refresh (`VRCompositorError_AlreadySubmitted` in
Steam's `logs/xrclient_<pkg>.txt`; FrameBridge pacing above the display rate). VrApi games that render faster than
the display ran ahead (~120 submits/s at 90 Hz) and the scene slid and snapped back. The VrApi bridge re-waits when
the new display time is under half a period after the last submitted one (`pacing 2s:` counters). Headless runs only
submit empty frames and never show it. Lepton's logcat mirror also missed the whole verifying session (launch.log =
Lepton's own lines only): the keeper now catches a silent mirror too (agent v91).

## Lepton's logcat mirror dies

**Lepton's logcat mirror dies (agent v67, 2026-10-09; v91 2026-10-14):** Lepton's mirror (`podman exec -it <container> logcat -v threadtime,color`) fails three ways: `logcat: Unexpected EOF!` right after the game starts, `Logcat read failure`, or running and writing nothing at all (Lepton 3.0.5: seen twice on 2026-10-14, once in the owner's session, once headless). launch.sh's `_logcat_keeper` reads `podman exec <container> logcat -v threadtime -T <epoch>` itself into launch.log after a failure line or 20 s without a logcat line (from the launcher's start when the mirror never wrote, else from just before its last line), restarts it from where it ended (≤5), stops it when Lepton's coloured lines come back, sets the container's log buffer to 16 MiB (`logcat -G`), and logs every decision to `<base>/logcat-keeper.log`. Checked on the dev Frame: a mirror killed mid-game (hand-over without a gap) and a silent mirror (the whole session recovered).

## Double launch

**Double launch (agent v57, 2026-10-06):** a second Play while Lepton still boots (~10 s with nothing to see) made
the second Lepton stop the first one's container ("Waiting for steamlaunch-<appid> (PID …) to exit", exit 137
"(starting)", "Clearing baked app data due to early exit") and both died (Vader, BattleSisters). launch.sh now takes
`flock` on `<base>/.launch.lock` (fd 9, inherited by Lepton; the watchdog and dashboard helpers close it with
`9>&-`); a second launch exits 0 and logs to `<base>/launch-dup.log`. `upgrade_launchers` adds it to existing
launchers. Verified on the device (second launch during boot: ignored, the first kept running). Vader on its loading
card ignores input too (owner pressed/held every button: presses reach the runtime, the card never changes).

## Issue triage round (2026-10-06)

**Issue triage round (2026-10-06, owner's headset):** vibration in OVRPlugin games = 2 s vibrations updated per frame and
stopped with amplitude 0; FrameBridge turns amplitude 0 into xrStopHapticFeedback (else each buzz ran 2 s), haptic_fix
uses the envelope RMS (was its peak), per-game `haptic_scale` → Creed/The Boys/Jurassic World fine. Lambda1VR (TBXR):
loader named by Build.MANUFACTURER (`frame.tbxr_vendor`), no graphics extension enabled (FrameBridge adds
XR_KHR_opengl_es_enable + asks for the requirements on -50), always multisampled render-to-texture (GL shim gives
single-sampled stand-ins when GL_EXT_multisampled_render_to_texture is hidden) + its xash/ data → works. Jurassic World:
`frame.vrapi_stub`. Metro Awakening: contributor's vkshim fixes + hide_space_warp + no VALVE_rpo (jumping polygons like
ITR2). Pinball FX VR: hide_space_warp (stutters). Eleven: a Meta online request fails after platform init → needs Meta
services. Star Wars Tales: top half black + freeze after loading; ruled out: asset-file paks (never requested), Valve
foveation (off: no change), GL errors (MESA_DEBUG=1: none), EGL_BAD_ACCESS once in OVRPlugin init (harmless); the game's
eye image reads back black. BlazeRush: VrApi bridge exports + ovr*_ToString stubs + avatar stub get it to the menu room
at 72 fps, full input reaches it (diagnostics `input 5s`: Touch type only, sticks 1.0, buttons, poses 0x8f), but the room
draws no controllers and ignores input; avatar loader forced to its "Failed" path (no logged-in user) changed nothing.
`frame.ovr_trace` can't trace Unity games (P/Invoke dlsym; nothing imports ovr_*). Headless: the VrApi bridge's 30 s
head-pose deadline ends VR mode without a worn headset (not a game bug).

## Lepton storage

**Lepton storage (2026-09-30):** each app's /sdcard (= /storage/emulated/0 → `<base>/lepton-data/external`) has `Movies`/`Download`/`Documents` symlinked to the Frame's `~/Videos`/`~/Downloads`/`~/Documents` (liblepton/mounting.sh, only if they exist at start); agent v24 `storage_targets` reads that mapping. Android's MediaProvider canonicalises paths to /home/steamos/... and rejects every file ("doesn't appear under [/system/media...]"), `sm list-volumes` is empty: the media index never works, apps must browse folders. Lepton installs with `adb install -g` (runtime permissions granted, MANAGE_EXTERNAL_STORAGE too). Files: `install/files.py`, `frameport frame send|storage`, GUI Files tab (formerly Frame → Send files).

## SteamVR per-app settings

**SteamVR per-app settings (2026-09-30):** editing steamvr.vrsettings while SteamVR runs is lost; the web API (127.0.0.1:27062 /app/setsettings) needs `x-steamvr-secret`. `native/vrsettings` = `fp_vrsettings.exe` (freestanding, OpenVR `FnTable:IVRSettings_003` as a Utility app, loads SteamVR's bin/win64/openvr_api.dll) sets them live and SteamVR persists them: section `steam.app.<shortcut appid>`, keys `preferredRefreshRate` (float) and `motionSmoothingOverride` (0 global, 1 on, 2 off, 3 always). Steam Link (vrlink) lists the Frame's rates 72/80/90/96/108/120/144 in vrserver.txt and follows the per-app preference ("host preferred N Hz"; whether the key is honoured is unverified in-headset yet). Judder metric: vrcompositor.txt session summary dropped + "Timed out. N total" (Stormland: 0 dropped but 313 timeouts in 2 min); fpsVR (`%LOCALAPPDATA%\fpsVR\*.json`, 0.1 ms histograms) gives p99 CPU/GPU ms. `pcvr.steamvr_tuning` (default on, PC only) applies on Play: highest rate whose budget ≥ p99×1.05, at least one step down, smoothing on.


## Language packs

**Language packs (merged from PR #20, 2026-10-04):** overport's `libovrplatformloader.so` is a dispatcher that `dlopen`s Meta's own loader (`libovrplatformloader_meta.so` / `_meta_q1.so`, also `libpxrplatformloader.so`) and keeps its own message queue; `ovr_LanguagePack_GetCurrent/SetCurrent` are 8-byte `return 0` stubs in it, `ovr_AssetFile_GetList` forwards to Meta's loader. `frame.langpacks` (opt-in, `native/langpack`) serves `<tag>.lang` files from the game's data; `elf.hide_exports` marks the loader's exports STB_LOCAL (bionic and glibc only match GLOBAL/WEAK; verified for glibc, bionic's `is_symbol_global_and_defined` is from memory). `libfp_langpack.so` is built here (`python native/build.py --only langpack`) and committed. Owner-verified 2026-10-04: Deadpool VR with only `en.lang` and the patch on plays English dialogue and runs normally (headless launch tests show 2-6 fps while it loads: not a regression sign). A dispatcher answer that arrives after we answered the timed-out GetList is dropped (one answer per request); games already in a library get `lang_packs` filled after an app update (`library._refresh_data_fields`). The library logs to logcat under the tag `fp_langpack` (dirs looked in, packs found, every language-pack call), so it shows up in the game's `launch.log`; an `ovr_AssetFile_GetList` the dispatcher leaves unanswered for 1.5 s is answered with our packs alone. Deadpool VR (Unreal) accepts a pack only when its `Metadata` equals the game's own version string (`ULanguagePacksSubsystem` compares it with `%s.%s.%s.%s.%s` built from the build info, e.g. `1.0.40.356975.Quest` = versionName; found by disassembling `libUE4.so`): the patch writes the APK's versionName into the library (`@FPMETA@` slot, `with_metadata`), `FRAMEPORT_LANGPACK_META` overrides it. Deadpool VR (2026-10-04, headset): German became selectable with that Metadata, but dialogue stayed silent (even English once reported as a pack) while the path was spelled `/sdcard/Android/obb/<pkg>/x.lang`; with the `/storage/emulated/0/Android/obb/<pkg>/x.lang` spelling (Unreal's own, now listed first in `scan_dirs`) German dialogue plays. `FRAMEPORT_LANGPACK_SKIP=<tags>` or a file `fp_langpack_skip` in the obb folder leaves packs out of the list (experiments).


## Cube swapchains

**Cube swapchains (GitHub #107/#385, 2026-10-17):** the Frame's runtime refuses `faceCount=6`
swapchains (-2, XR_ERROR_RUNTIME_FAILURE). `cube_standin` (default on, `native/adapter/cube_standin.c`)
serves refused cubes with a GLES cube texture in the app's context or a Vulkan cube-compatible image.
This prevents OVRPlugin's empty-image-list crash (Budget Cuts Ultimate; Vulkan UNDERDOGS).

`cube_quads` (int, default 1) now copies mip 0 into six runtime quad swapchains, replacing each cube at
its original layer-list position. Face order is +X, -X, +Y, -Y, +Z, -Z; inward-facing quad right/up axes
match GL cube s/t. Vulkan copies reverse rows for its top-left quad image origin. The cube orientation
rotates the box; its centre follows the stereo view midpoint in the layer's space at displayTime
(falls back to the space origin without valid tracking). Faces are 100 m square at 50 m distance.
Eye visibility and layer flags are retained. Quads submitted by the game retain their order.

The Frame draws quads above **every** projection, regardless of order. This implementation conservatively
drops cubes whenever any projection is submitted, including alpha projections: it supports cube-only
loading screens with optional quad overlays, not skyboxes behind gameplay. Multiple cubes of the same
size/format share six swapchains, using separate array slices so their images cannot overwrite one another.
Each pool is copied once per frame. Other sizes need another six swapchains; runtime allocation refusal
(particularly near its ~15-swapchain cap) frees partial allocations and disables that pool until reset.
Pools are freed on stand-in creation/destruction, xrEndSession and session destruction; stand-ins are also freed at
session destruction. Set `cube_quads=0` for the previous drop-only behaviour.

Look for `cube_quads: created six ... face swapchains (GLES|Vulkan)` and
`cube_quads: submitting cube as six quads (half-size=50m, mip=0)`. Refusals log
`cube_quads: face swapchain creation failed (result=...); dropping cube layers`; mixed frames log
`cube_quads: skipping cube with projection; preserving scene visibility` once.
Host math/settings/artifact checks and Android builds are available; no device test yet. Headset checks
still needed: face seams/orientation, sRGB/alpha, GLES context availability, Vulkan synchronization and
loading-to-gameplay transitions (HardBullet's 1024²/32² Vulkan cubes; Budget Cuts GLES).

## Hardware video decoding

**Hardware video decoding (PR #96, PR #128 by Lucas-Mathieu, adapted 2026-10-10, agent v71):** `native/hevc` = one
OMX plugin `OMX.frameport.{avc,hevc,vp9}.decoder` (FFmpeg v4l2m2m on Iris /dev/video-dec0, FFmpeg software decoders as
fallback inside the component, Vulkan copy for big native surfaces). The connection installs it once per Frame
(`ensure_video_codec` → agent `video_codec_status`/`install_video_codec`: `~/.local/share/frameport/video-codec/versions/
<manifest sha>` + `current` symlink). A Frame keeps the same or a newer `revision` (no flip-flop between PCs) → **bump
`revision` in native/hevc/build.py with every artifact change**; a failed install isn't retried during the connection.
Per game: only launchers of games whose recipe has `frame.hw_video_decode` (install stage, no APK change; suggested
from analysis `media_codec`, ANALYSIS_VERSION 8; deployment.json `hw_video_decode`) get the codec line → the Podman
wrapper (native/hevc/podman.py) mounts the plugin into that container (merged media_codecs.xml in
$XDG_RUNTIME_DIR/frameport-video). Off for every game: Settings → Installing (library `video.hw_decode` → agent
`video_codec_switch` = video-codec/disabled); one game: `FRAMEPORT_NO_HW_VIDEO=1 %command%`. `upgrade_launchers`
converts agent ≤70 launchers (Batman's `<base>/frameport-codec` → keeps the line, folder then removed). Batman: catalog
+ migration `batman_video_patches` (`frame.hw_video_decode` + hidden adapter setting `surface_native`). Iris: 8K
refused (ENOMEM) while any other decoder session is open; SteamVR's vrlinkrunthread holds one ~12 s at game start →
refused opens retry until 20 s after the plugin loaded (else 2 s), then software; VP9 7680x3840 never returned a
picture → VP9 ≤4096x2304; hidden VP9 frames come back as empty capture buffers (bytesused 0, no LAST):
FFmpeg's wrapper ended the EOS drain at the first one (two-pass VP9 lost its last ~25 frames) → build.py requeues
them (codec revision 8; drain ends at LAST, or 1 s after a skipped empty buffer; dev Frame 2026-10-10: 600/600 =
OMX.google.vp9 Y hashes). Rebuilds are deterministic (ext4 + NTFS path
with spaces, two NDK copies → same; rev 8 141de01f…; PR #128's own sources → its 27a2d749…). Not yet run on the device.

## Vulkan shader dump

**Vulkan shader dump (GitHub #140, 2026-10-10):** adapter `vk_shader_dump=1` (Vulkan shim, `native/vkshim/shader_dump.h`;
applies where vk_sanitize can: Unreal/Other arm64) writes each distinct SPIR-V module once to
`files/fp_vk_shaders/<size>_<sha256>.spv` (tmp + rename) and one `index.txt` line per vkCreateShaderModule (`<seq> <ms>
<unix ms> <name> new|known|again|failed`, O_APPEND) to find the module behind a GPU hang. Agent v72 `collect_diag`
returns `shaders` (newest modules of fp_vk_shaders + fp_spirv, ≤4 MB each, base64) → bundle
`games/<pkg>/target/shaders/`. Triage `gpu-hang` suggests both dumps; `triage.graphics_api` (FrameBridge's
xrCreateSwapchain formats: <0x1000 Vulkan, else GL) keeps only the session's API's (`API_ONLY`). Host-tested only.
Agent v74: a reporter's hang came ~10 s after the last new module, so the culprit wasn't among the newest written →
with an index naming modules, `shader_dumps` sends every module of the index's **newest session** (after the last
`# start`), deduplicated, last used first, each ≤4 MB, ≤30 MB in all (`session` = how many it names) + that session's
index lines; no index (fp_spirv) = the old newest-by-mtime 4 MB. One JSON line (~40 MB base64) over the SSH channel
is read whole by `Frame.agent` (fine at that size); `_Writer.fit` drops the least recently used modules last, after
cutting logs, to stay under the 24 MB zip limit.

## Unreal OBB check

**Unreal OBB check (GitHub #159, 2026-10-10):** Epic's DownloaderActivity checks OBB name+size (OBBData), then with `bVerifyOBBOnStartUp=true` CRCs the whole OBB; its screens are invisible in Lepton (stuck at `Displayed …/.DownloaderActivity`, triage `unreal-obb-check-stuck`) → `frame.unreal_skip_obb_check` (analysis `unreal_verify_obb`, ANALYSIS_VERSION 11) flips the manifest boolean (`axml.set_meta_data_bool`); skips only the CRC pass. Host-tested only.

## Issue round 2026-10-12

**Issue round 2026-10-12 (agent v83):** Lepton pastes STEAM_COMPAT_DATA_PATH unquoted into `sh -c "ln -s …/internal/<pkg>
/data/data/<pkg>"` (mounting.sh): a game on a card named "Steam Frame 512g" got no internal storage (no saves/prefs;
Beyond Sandbox's EOS then looped `createKeychain→resetKeychain` forever, 1.85 GB log). Quest launchers with an unsafe
path now `ln -sfn <base> ~/.local/share/frameport/lepton-paths/<pkg>` at every start and give Lepton that path
(`app_dir_lines`/`set_app_dir`/`launcher_base`; upgrade_launchers converts; deployment.json keeps the real base).
Verified on the device with a spaced copy of Stremio (old: the `ln` error; new: /data/data link, shared_prefs
writable). `frame.eos_keychain_loop` (opt-in, triage `eos-keychain-loop`) returns from EOSSDK.resetKeychain. Wander
0.3570 (Unity 6.5, Firebase, Gecko) ran out of bionic's 128 pthread keys → `frame.unity_tls_key_pool` (Klownicle's
approach, #198: libunity's 4 key imports renamed `fp_tkey_*` via `elf.rename_imports`, `libfp_tkeys.so` pools
destructor-less keys behind one real key); device: past Firestore, 72 fps. Cpp2IL releases can't read metadata v107
(Unity 6.5) → `tools/cpp2il.py` falls back to the development CI build (nightly.link) on that error, and since #293 on any release failure (BattleTalent, Unity 2021.3: the release dies exit 134 in CsFileUtils.GetTypeName after making the folders, no .cs; `il2cpp._run` = non-zero exit or no .cs is unreadable; never with FRAMEPORT_CPP2IL; caches holding "every class missing" are re-run). Vermillion's canvas
is an OVROverlay quad (drawn above the hand) → `frame.vermillion_scene_canvas` (PaintableMesh getters → false;
installed build checked, 72 fps headless; headset not yet). Linux apps: `desktop_icon` switch, a deleted desktop copy
stays deleted (#212). Findings: OLAR (#201) RHIThread null deref in libUnreal (`ldr x8,[x19,#792]`, x19=0) not fixed by
hide_space_warp / vk_hide_fdm / no Valve layers; FNAF HW (#135) "freeze" = first-start GL program-binary-cache compile
on Zink (threads busy in zink/disk cache), not a hang; VR4 (#199) reporter's patch.203 OBB truncated. Upload benchmark
(`scripts/upload_bench.py`, #242): home Wi-Fi caps every method at ~11 MB/s; paramiko uses ~3x OpenSSH's CPU; Windows
OpenSSH refuses FramePort's key file (ACL "bad permissions") until its permissions are tightened.

## OpenSSH uploads

**OpenSSH uploads (2026-10-15, #324/#242):** sftp escapes `* ? [` inside double quotes itself (and undoes it for
put's destination), so `native_upload.quote` escapes only `\\`/`"`; Win32 sftp turns every local backslash into `/`
(escapes too) → local paths go as `C:/…`. Logins: `Frame.auth` = the attempt that worked (app_key / user_key = a key
file from Settings → Installing → SSH keys, library `ssh.extra_keys`, CLI `frame ssh-keys` / agent / password).
OpenSSH strict (FramePort + Settings keys, IdentitiesOnly) unless auth was "agent"; a refused strict login retries
relaxed (+ private copies of ~/.ssh/id_{rsa,dsa,ecdsa,ed25519}: OpenSSH skips its defaults once any -i is given, + the
agent); `-v` key lines go into the error. FramePort doesn't add its key to the Frame for this. Windows-tested.

## Meta user id

**Meta user id (GitHub #182, 2026-10-11):** OVRPort's platform loader (runtimes 3.4.3-23204ea and -aa54c3f) answers
`ovr_GetLoggedInUserID` (and two other callers) without a Meta account with a made-up id from a helper (`stp; mov x29,
sp; bl random; …; mov/movk x9 = 0x53444835ec580000 (6e18); add x0, x8, x9; ldp; ret`), kept only in memory → a new id
every start; Walkabout Mini Golf files saves under `Profiles/Oculus/<id>/` and lost all progress. `frame.ovr_stable_user`
(default_on, detect = the game has libovrplatformloader.so) rewrites the helper to `mov x0, <6e18 + 32 bits of
sha256(package, keystore)>; ret`: stable per game and per user (keystores are personal), so online games never see two
FramePort users as one. Found in every PATCHED build checked. Host-tested only; upstream candidate.

## Vivox API 31

**Vivox API 31 (GitHub #101, 2026-10-09):** newer Vivox builds (Green Hell VR) call Android 12 AudioManager
communication-device methods from `com.vivox.sdk.AudioChangeListener` with no SDK check → NoSuchMethodError on Lepton's
Android 11. `frame.vivox_audio_route` (analysis `vivox_api31`, ANALYSIS_VERSION 4) makes every such method return at
once (`Dex.return_early`: return-void / `const/4 v0,0; return v0`; nopping the invoke would leave a move-result the
verifier rejects). Older Vivox (Eleven Table Tennis, BattleSisters) lacks that code and isn't matched. Verified
headless: Vivox initialises, 150 s at ~65-70 fps.
Photon Voice does the same (GitHub #362 Table Troopers, #325): `com.exitgames.photon.audioinaec.AudioInAEC.
updateAudioDevice` (void; only caller of getAvailableCommunicationDevices/setCommunicationDevice, reached from the mic
thread `AudioInAEC$1.run` via access$000; no SDK_INT check left) → sibling patch `frame.photon_voice_audio_route`
(analysis `photon_voice_api31`, ANALYSIS_VERSION 21; shares `vivox_audio_route.patch_class`; Vivox's id, title and
output unchanged, no revision bump). Triage: `vivox-api31` / `photon-voice-api31` now need the SDK's class in the
stack; any other `*CommunicationDevice*` NoSuchMethodError = `audio-api31` (report). Host-tested on the real dex only.

## Steam Input gamepad for 2D apps

**Steam Input gamepad for 2D apps (GitHub #162, agent v72, 2026-10-10):** Lepton's Android only has the Wayland seat's `wayland_touch/keyboard/pointer`. Steam makes its virtual pad (uinput, `/devices/virtual/input`, 28de:11ff, "Microsoft X-Box 360 pad N", ACL for steamos) only while the Frame's controllers are on (controller.txt "Steam Controller reserving XInput slot 0"; gone when they sleep), also with no game running; steamos-manager's 28de:0000 keys device isn't a pad (no BTN_SOUTH). Lepton's container root (system_server too) = steamos (`keep-id:uid=0`), so a bind-mounted `/dev/input/eventN` opens. Opt-in `device.steam_gamepad` (vr_kind none; suggested for `android.hardware.gamepad`/`LEANBACK_LAUNCHER`, analysis `gamepad`, ANALYSIS_VERSION 9): deployment.json `steam_gamepad` → launcher `GAMEPAD_LINE` (after the codec line: exports FRAMEPORT_GAMEPAD + LEPTON_ENV_SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD, puts `agent/bin` first on PATH) → `PODMAN_WRAPPER` (written by ensure_host_fixes; non-`run` calls go straight on; `run` imports the agent's `podman_run_args`; hands on to the next Podman after its own PATH entry with itself removed from PATH, so it chains to the codec wrapper either way round) adds the pads + `Vendor_28de_Product_<pid>.kl` (Xbox 360 layout). Verified headless with a uinput stand-in pad (Stremio's container: EventHub `classes=0x80000141`, our key layout, KeyEvent BUTTON_A dispatched); with Steam's real pad and a game: not yet. Pads made after the start aren't seen until the next one.

## Multiview programs on flat framebuffers (Doom3Quest)

**Multiview programs on flat framebuffers (GitHub #77, Doom3Quest, 2026-10-09, prototype):** Mesa enforces OVR_multiview's
"program num_views == draw framebuffer views" rule (`draw_validate.c`, the draw is dropped silently); Qualcomm doesn't.
Doom3Quest compiles every VS with `layout(num_views=2) in;` and draws its HUD/PDA into 2D-texture FBOs → black. Opt-in
`frame.gl_multiview_fbo` (analysis `gl_multiview_libs`, ANALYSIS_VERSION 6; own-engine GLES only): `native/glmv` =
`libfpglmv.so` (12 chars = "libGLESv3.so": libdoom3.so's one `.rodata` dlopen string is rewritten in place, its qgl*
table comes from dlsym on that handle; also first DT_NEEDED for its direct gl*/egl* imports). Such draws use a lazily
built single-view twin (view 0, uniforms copied per draw); eglMakeCurrent resets the per-thread cache. Host-tested only
(rewriter on all 19 Doom3Quest shaders + glmv.c against a stand-in GL, `tests/test_gl_multiview_fbo.py`); the host's
Mesa 23.2 llvmpipe has no OVR_multiview. Headset-verified by Xandrix1987 (2026-10-09: HUD/PDA shown, no fps drop) -> in the Doom3Quest catalog recipe.

## BlazeRush

**BlazeRush (GitHub #57, Klownicle's verified guide, 2026-10-10; FramePort's version headless-tested only):**
`frame.blazerush` (1.0.349 only: exec-segment SHA + every word checked, offsets from PT_LOAD) NOPs the avatar-readiness
wait (0x4b2dbc), RETs the avatar renderer (0x4b2adc), renames `cache/shader_cache` → `cache/br_shader_v1`. GL shim
(now also exports glShaderSource/glLinkProgram/glProgramBinary/glDeleteProgram/glDraw*: pass-through unless a setting
is on) `gl_int_attribs` (integer inputs fed via glVertexAttribPointer re-set with glVertexAttribIPointer per draw,
reflection cached per EGL context + program, dropped on link/binary/delete) and `gl_highp_markers` (vertex shaders with
every '|' marker: mediump→highp except on in/out/uniform lines); host test `tests/test_blazerush.py` with a stand-in
GL. FOV: the game reads VrApi props 7/8 right after vrapi_Initialize (no session; the Frame has no
XR_EPIC_view_configuration_fov) → the bridge saves the live FOV (also located before tracking, headless: 118.87×120.04
on the dev Frame) in `files/framebridge-vrapi-fov.txt` and answers with it at the next start (log `FOV properties: …
(saved by an earlier session)`). `device.config_sync` (catalog field `config_sync`, agent v78): user_config.xml
r_3dwidth/height, r_width/height from the runtime eye size (FrameBridge logs `runtime recommended eye size`) × scale,
written at finalize, set_settings and by launch.sh before Lepton starts (`_config_sync`). Headless runs at scale 1.7
end in `zink: DEVICE LOST` right after the bridge's 30 s no-tracking deadline tears the session down (gpu fault at the
teardown; not at scale 1.0): a headless-only artifact, the headset never hits that deadline. Launch tests count an exit/crash after
such a line as a pass (triage.yaml `headless_end`, finding `headless-end`; GitHub #57 "Failed to launch" on install).

## Session triage

**Session triage (agent v70, 2026-10-09):** launch tests never reach FOCUSED, so real play sessions are triaged
too. list_installed gives each game `last_play` {start, end, test} from `<anchor>/plays.log` (launch tests write a
`test <unix>` line first → test sessions are only marked); `session_log` returns the newest session's launch.log
(`<base>/session.log`, ≤4 MB: head + tail + FrameBridge/crash lines between) + that session's logcat-crash.log +
`journalctl -k` GPU lines ("kernel: …"). The GUI's connection refresh (`app._check_sessions` → `pipeline.sessions_due`,
background thread; sessions >7 days old are skipped) runs `pipeline.triage_session` = `validate/session.analyze`
(triage.yaml signatures incl. `space-warp-used` (info, `question:`, never auto-applied), `gpu-hang` (`report: true`)
+ computed `slow-frames` (pacing windows vs the nearest refresh rate) and `focus-dips` (FrameBridge always logs
`focus: lost` / `focus: back after N ms`, session_fixes.c)) → library `last_session` / `last_session_checked` → game
page "Last session" callout (Try this fix / Rebuild with this fix / Yes-No question / Report / Dismiss). Suggestions may
be values (`adapter.scale=0.85`, `triage.split_suggestion`); adapter-only fixes are pushed live (`apply_suggestions_live`
→ agent set_settings, like the Game settings dialog). CLI `frameport session <pkg> [--apply]`. PC VR (Proton) launchers
log no session end, so they aren't triaged yet. pac_hints stays triage-only: a survey of all 68 dump APKs found unpaired
PAC hints in 21 libraries of 18 games (OpenSSL's 38/40 in most UE4 libUE4.so and libEOSSDK.so, UE5 libUnreal.so ~400
vs +2-3, libass, libopencv, …), most of them games that work, so default-on would rewrite many working builds.

## Install links / FrameDrop button protocol

**Install links / FrameDrop button protocol (2026-10-07, not yet clicked end to end from a browser):** FrameDrop
(framedropvr.com, a closed-source Windows sideloader) defines "Install with FrameDrop" buttons:
`https://framedropvr.com/install?manifest=<url>|url=<file>` → that page opens `framedrop://install?…` (1.6 s, else its
home page); manifest `{"schema":"framedrop.install/v1","name","files":[{"url","sha256"}]}` (name = Steam title; .apk
or Linux .zip). `deeplink.py` (no Flet) parses framedrop://, frameport:// and the pasted https link, enforces its
rules (https; http only on loopback; no credentials; no LAN/loopback/link-local IPs, also after DNS and redirects; URL
ends in a file name), caps manifests at 256 KiB, ignores non-hex sha256 (FrameDrop's own example has a placeholder),
downloads into `<data>/downloads/<slug>-<hash>/` (OBBs → `obb/` next to the APK, `.part` removed on cancel/mismatch);
`pipeline.add_from_link` routes APK / Linux / exe and sets `title` + `title_locked` + `link` (a bare file link keeps
FramePort's title). GUI: `views/link_dialog.py` (always asks first), Add games → "Install from a link…", CLI
`frameport open-link [--yes --no-install --gui]`. **A `flet build` bundle can't take a URL argument** (the Flutter
host treats any argv as a developer page URL), so `urlhandler.py` registers a script, not FramePort.exe: Windows
HKCU `Software\Classes\{framedrop,frameport}` → hidden PowerShell `frameport-link-handler.ps1`; WSL (source runs)
the same keys → `wsl.exe -d <distro> -e sh frameport-link-handler.sh`; Linux `frameport-links.desktop` +
`xdg-mime`; macOS unsupported (Apple Events, paste instead). The script drops the link into `<data>/links/*.link`
and starts FramePort unless `<data>/gui.alive` is < 10 s old (`gui.starting` stops double starts); the GUI's
`_watch_links` thread touches the heartbeat and opens links (newest window session). Settings → Install links: one
switch per scheme (`links.framedrop`, `links.frameport`, default on); a scheme another program owns (FrameDrop) is
only taken with "Use FramePort for these links" (`register([s], force=True)`); off removes only FramePort's own
registration (`MARK` in the command). Never registered with FRAMEPORT_HOME/FRAMEPORT_NO_LINK_HANDLER (tests,
screenshots). Same round: files dropped on the Library (`ui/dropped.py`, bundles only like the Files tab), "Add a
Windows program (.exe)…" (`pipeline.add_windows_exe`: exe in Downloads/home/drive root copied alone into
`<data>/windows-apps/<slug>/`), patch `device.display_mode` (Automatic / VR / Flat window → `InstallContext.display`,
`installer.show_window`). FramePort-only manifest extension `"frameport": {"description", "icon"}` (bad values ignored; icon: same URL rules,
≤2 MiB, Pillow-checked, ≥32 px, saved as PNG in `<data>/downloads/icons/`, shown in the question by asset URL, then
`sources.apply_custom(pkg, "icon")` unless `.picked` exists; description fills `details.description` only when
empty). Bare file links get a title guessed from the file name (`title_from_filename`: version/arch dropped, package
names → last part). Windows test of 0.12.1.dev191 (2026-10-07): a button click opened the dialog; but a click right
after closing FramePort did nothing (the closed window's heartbeat was < 10 s old) → the handler scripts now wait
up to 4 s for the link file to be taken before trusting the heartbeat, the window's CLOSE event / atexit delete
`gui.alive`, and the Windows command runs under `conhost.exe --headless` (plain `-WindowStyle Hidden` flashed a
console). Screens: `scripts/ui_smoke.py --links --fake-frame --game <pkg>` (tall pages: `--viewport
1280x7000`; Flutter's popup menu ignores Escape).
Demo link: the homepage's example button = `deeplink.DEMO_MANIFEST` (https://frameport.app/demo/cool-game.json, published
from site/public/demo/); every form (https button page, frameport://, framedrop://, pasted) → `InstallRequest.demo`,
no network (fetch_manifest returns `demo_manifest()`, download refuses), `link_dialog.show_demo` (Install =
`easter.demo_install`: Cool Game's cover out of the portal; eggs off → "Nice, it works!"), CLI just says so.
Link updates (2026-10-17): manifest `version` (top level, or `frameport.version`, which wins; ≤64 chars) is stored in
the entry's `link` record (+ `direct`); `pipeline.check_link_updates` (GUI start via `app._check_link_updates`, every
6 h: setting `links.last_update_check`, off with `links.check_updates`) re-fetches web manifests that named a version
(`deeplink.can_check_updates`: not bare file links, local manifests or the demo) and sets/clears `link_update`
{version, checked}; versions compare with `deeplink.compare_versions` (numbers, then release > suffix). UI: Library
`link_bar` + game page callout → `link_dialog.update_from_link` (fetch again, one confirmation, `download_and_install`
per game; add_from_link clears the offer and keeps a name the user gave). CLI `frameport link-updates [--install]`.


## XR_FB_color_space (#336)

OutRun Quest VR (`org.outrun.quest.vr`, its own Khronos loader, no OVRPlugin) logs "Rec.709 color space unavailable" right after xrCreateInstance and quits ~20 s later without a session: the Frame's runtime (and OVRPort's dispatcher) list no XR_FB_color_space. Adapter setting `fb_color_space` (off by default, `native/adapter/color_space.c`) offers the extension when the runtime lacks it, strips it from the runtime's create info, fills XrSystemColorSpacePropertiesFB with Rec.709 and answers xrEnumerateColorSpacesFB / xrSetColorSpaceFB (no conversion: the runtime has no API for it). The functions are reachable through FrameBridge's xrGetInstanceProcAddr and `framebridge_extension_proc` (xrshim); OVRPort's fixed dispatcher table can't forward them, hence off by default. Not yet tried on the device (the build was not downloaded: it ships Sega's game data). Log: `color_space: emulating XR_FB_color_space`.

## Per-game headset identity (#373, 2026-10-17)

Owner decision: all Quest / OVRPort conversions default to Quest 3; Customize → Headset model offers Quest 3,
Quest 3S, Quest Pro and Quest 2. Rebuild and reinstall after changing it. Rift, Linux and 2D apps are excluded.
Target: Asgard's Wrath 2's **Enhanced Rendering Features** and **90 Hz Mode**. This implementation has static and
host tests; unlocking those options and actual 90 Hz presentation still need an owner headset check.

Device evidence supplied by the owner (headless Transformers, OVRPort runtime 3.4.3-aa54c3f): Lepton's image already
sets `ro.product.model=Lepton`, manufacturer `Valve`, device `lepton_arm64_only`. A `LEPTON_GFXRECON_*` boot-property
line cannot replace existing write-once `ro.*` values. Removed the WIP headset property injection. Android
`Build.MODEL` stays Lepton. A general app-start dex/reflection override is follow-up: the existing `Dex.return_early`
tooling replaces method bodies, but does not supply a universal early startup hook that safely sets Build fields.

OVRPlugin logged `venderId 10291, systemName Oculus Quest2` even with FrameBridge rewriting the name. The APK's
`lib/arm64-v8a/libopenxr_loader.so` dispatcher sits above FrameBridge: `xrGetSystemProperties` at VA `0x18554`
overwrites the name (`0x18620`–`0x18638`, string at file/VA `0x6034`) and returns its own
`XR_META_headset_id` (`0x186e8` loads address `0x29338`, `0x18778`–`0x1877c` copies 16 bytes;
UUID file offset `0x21338`). Vendor id remains 10291. A lower-layer-only override cannot fix those answers.

Disassembly of the supplied Unreal `libOVRPlugin.so`:

- `ovrp_GetSystemHeadsetType` at `0x251730` calls `ovrp_GetSystemHeadsetType2` at `0x247bb0`, which calls the
  `0x2590b0` thunk → `0x292f80`.
- System properties' name is copied to the global std::string `0x62d328` at `0x261b30`–`0x261b3c`.
  `0x261b9c`–`0x261bb4` stores the headset-id-present flag (`0x62d2c0`) and UUID (`0x62d2c1`).
- `0x292f80` checks that flag. When present it compares the UUID, in inline 64-bit constants, returning
  Quest 1 = 8 (`0x293134`), Quest 2 = 9 (`0x2931ac`), Pro = 10 (`0x2931b8`),
  Quest 3 = 11 (`0x2931c4`), Quest 3S = 12 (`0x293060`–`0x29309c`). Unknown UUID → 9.
- Only without the UUID does `0x2930a8` fall back to systemName (older Quest/Pro names, default 9).
  `0x292d60` is a separate product-name normalizer: it recognizes newer Meta names but returns the generic
  `Meta Quest` for known Quest products. It is not the headset-type selector. Android Build is not consulted
  in the traced headset-type path.
- Cross-check: the available armv7 OVRPlugin's Thumb entry `0x120c5c` → thunk `0x12bc38` → `0x148ac8`
  likewise tests UUID first (16-byte comparisons), then name at `0x148b1a`. This older build recognizes through
  Quest 3 (return 11 at `0x148b14`), not Quest 3S. It is consistent with the Unreal mapping for Quest 2/Pro/3.

Raw XrUuidEXT byte order (hyphens just group sequential bytes; do not swap GUID fields):

| Model | UUID | Unreal headset type |
| --- | --- | --- |
| Quest 2 | `3064f239-15ed-4722-a659-5ea1ecd899c2` | 9 |
| Quest Pro | `b58443c9-df86-40cd-8918-2a278e18ab71` | 10 |
| Quest 3 | `082746ba-a071-4cf7-8f87-bade5cce43ce` | 11 |
| Quest 3S | `9510492b-0d42-4872-bcf4-fb1a8def6f0e` | 12 |

`device.headset_model` now applies at APK stage after FrameBridge. It verifies original name, UUID and the
instructions loading both in the known dispatcher before writing either. Unknown builds fail with an explicit
unsupported-dispatcher message and a Quest 2 escape hatch. Same-size data edits only: NUL-padded 14-byte name and
16-byte UUID. Quest Pro uses the short dispatcher label `Quest Pro` because `Meta Quest Pro` plus NUL needs 15 bytes;
its UUID selects type 10. Quest 2 changes no dispatcher bytes and adds no embedded headset setting (native default preserves the old name).
FrameBridge retains the full standard name override
for apps calling it directly; setting 2 preserves its original result. No UUID extension is invented in FrameBridge.

Owner verification: rebuild/reinstall Asgard's Wrath 2 with Quest 3 selected. Capture the OVRPlugin line containing
`venderId 10291, systemName Meta Quest 3`, and its `HeadsetId:` line (16 hex bytes matching Quest 3 above).
Capture any game/OVRPlugin headset-type line: expect `Meta_Quest_3` / `Quest_3` / numeric 11 if the build logs it;
absence of such a line is not proof of failure. In the headset check both named graphics options become available,
can be enabled and persist after restart. Check actual runtime refresh/pacing separately when selecting 90 Hz;
identity alone does not implement refresh-rate support. Rebuild with Quest 2 for the control: original `Oculus Quest2`,
original Quest 2 UUID, type 9 if logged, and previous option visibility. Also smoke-test a Unity OVRPlugin game
with the default Quest 3 choice for regressions. No device access was used during implementation.

## Mixed reality: scanned rooms

Live floor coverage (agent v104) is enabled by default for manual and game-requested scans; the capture
payload accepts `preview: false`, and `frameport frame room scan --no-preview` disables it. A world-locked,
upward-facing 6 m square at floor height follows the current raw-to-STAGE calibration and stays centred on the
scan's first recorded camera position. Every 16th depth vertex contributes to raw-space 10 cm voxels; at most once
per second, newly occupied cells redraw a 256×256 RGBA map in one colour (FramePort's secondary blue), each cell a
filled 10 cm square, only the opacity telling floor (<0.3 m, faint), raised surfaces (0.3–1.5 m) and tall surfaces
(>1.5 m, strongest, the room's outline) apart; unseen transparent. Overlays have no depth (they draw over hands and
passthrough), so the map stays faint; three colours in 3x3 blotches clashed in the owner's first run. It shows
coverage, not a validated room boundary. IVROverlay_028 slots 1/3/20/22/33/43/62 (OpenVR 2.15.6 header)
create/destroy/sort order (1, below the scan prompts' 10)/set width/set absolute STAGE transform/show/upload raw
pixels. Each upload is 256 KiB (at most 256 KiB/s); timing for the first
20 uploads and every tenth thereafter, plus failures, goes to `rooms/scans/scan-*.fprs.log` and the built room’s `scan.log`. Overlay errors or an
upload taking over 50 ms disable the preview while capture continues; headset appearance and upload cost still
need device validation.

When an MR game asks for a room scan, FramePort scans in the headset and FrameBridge loads the new room before
answering the game. If no scanner answers within five seconds, the game gets the stored room. Set
`scene_scan_request=0` to always use the stored room. A scan that starts but fails keeps the old room and reports
a capture failure to the game.

**Mixed reality: scanned rooms (agent v92, 2026-10-16, branch mr-scene; docs/mr/):** Quest MR games read the room
through Meta's scene APIs; the Frame offers none, and Valve's chaperone is no room (2D outline raised to 2.43 m, often
the default 1 m circle -> the Android runtime's STAGE rect falls back to 1x1 m; XRService's plane detection is
horizontal-only, its persistent map sparse ORB, its XRIPC single-client; cameras are XRService's alone,
IVRTrackedCamera gives no frames). **The passthrough's RoomView 3D depth mesh is readable** (docs/mr/PHASE0.md:
IVRBlockQueue_005 `/xrservice/roomview3d/meshData|meshLayout`, metadata through the C++ `IVRPaths_002` on the block
handle, `meshIsUpToDate` on the queue handle; 132x128 fan, ~30/s; raw->standing moves during a session -> convert
with the final one), so FramePort scans rooms itself: agent `room_capture` (Background/Overlay OpenVR app, prompts =
artifacts/roomscan/*.png via IVROverlay_028 slots 1/3/20/22/35/43/44/63, `scripts/mr/render_prompts.py`) -> `.fprs`
dump -> `fp_roomscan` (native/roomscan, freestanding, offline) -> `room.txt` + `room-mesh.bin`
(docs/mr/ROOM_FORMAT.md; outline = positive shoelace in x-z, wall inward normal (-dz, dx)) in
`~/.local/share/frameport/rooms/<universe>/`; launch.sh copies it next to settings.conf (`_room_sync`, games with
`scene_emul`); FrameBridge (`scene_room.c/scene_meta.c/scene_anchors.c`) serves N walls, volumes (BOUNDED_3D),
planes, GLOBAL_MESH (XR_META_spatial_entity_mesh), XR_META discovery/persistence (newer Meta SDKs and MRUK), anchors
saved in `files/framebridge-anchors.txt`; all through xrshim (OVRPort's dispatcher drops unknown META calls).
`scene_emul` is now suggested for every USE_SCENE/USE_ANCHOR_API game (migration `scene_emul_all`; NOPE and Pinball
FX had none). PC: Frame page "Room" card, `frameport frame room scan|status|from-boundary|import|export|build|show`.
Real data (owner's room, 2026-10-16): floor at 0, table surfaces, orthogonal walls found; the ceiling needs "look up".

Scan on request (agent v103): scene-emulation launchers run `_scan_watch` alongside the game. FrameBridge's
`framebridge-scan-request` in the game's external files directory gets an ack, then the existing prompted headset
capture/build flow saves and copies the new room before an `ok` result; errors, cancellation and timeout return
`failed`. Captures share a global lock, stale protocol files are cleared on launch, and the watcher stops with the
launcher. Logs: `<base>/scan-watch.log`. Device validation is still required.

PR #246 (@alicecatpaw, merged into mr-scene 2026-10-18): FrameBridge trims frames to maxLayerCount (Punch Fit sent
21+ layers; projection layers and FrameBridge's fade quad always stay), drops quads with zero size or an empty image
rect, and logs up to 10 quads with pose and swapchain. Two Unity patches, both only from a catalog recipe or the
user's choice: `frame.meta_depth_occlusion` (Depth API occlusion off; Laser Dance's lasers vanished up close) and
`frame.unity_scene_active_rooms` (OVRSceneManager's player-in-room check). The PR's own META discovery and boundary
visibility code was replaced by this branch's (`scene_meta.c`). The PR's recipes were checked on the author's Frame
only; open question: does OVRPort's dispatcher advertise XR_META_environment_depth (then a FrameBridge setting that
hides it would fix depth occlusion for every engine).

## Vulkan pipelines refused: Fossilize and Turnip (Transformers, #349)

Transformers Beyond Reality: Redux (Unreal, Vulkan) ended its first frames with Unreal's "Shader compilation failures
are Fatal." (2026-10-17, headless on the dev Frame). `vk_pipeline_log=1` showed every graphics pipeline failing in
pairs: the precache attempt with `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` (flags 0x100) answers
`1000297000` (= VK_PIPELINE_COMPILE_REQUIRED, normal), the real compile answers `-1` (VK_ERROR_OUT_OF_HOST_MEMORY)
in the same millisecond, and the shim never logged a single success. The dumped SPIR-V was valid (spirv-val), one
module a constant-colour fragment shader. Ruled out in one run each: the app's VkPipelineCache, its
VkAllocationCallbacks (retry without either/both), Valve's VALVE_rpo/VALVE_fdm_injection (`device.foveation: off`).
Khronos' validation layer added a crash of its own (null deref in its vkCreateRenderPass2 on the game's create info,
the shim changed nothing there): don't use it for this game. Removing `VK_LAYER_fossilize` from GraphicsEnv's debug
layer list (`android::GraphicsEnv::setDebugLayers`, as `native/zinkfix` adds its layer) made the pipelines compile:
360 modules instead of 10, frames submitted. Two Fossilize instances are still created by other threads before the
engine loads the shim; they aren't the renderer's. About six pipelines with the game's multiview vertex shader
(module 12944, ViewIndex) still fail in Turnip; a blank fragment shader alone doesn't help, blank vertex + fragment
does. Settings `vk_no_fossilize` + `vk_pipeline_fallback` (catalog entry): headless RUNNING after 72 s at 72 fps.
Headset check pending (some objects may be missing).

## Quest controller mode (`quest_buttons`, 2026-10-17)

Adapter setting `quest_buttons` (0 SteamVR default, 1 Mirror, 2 Quest pair; contract docs/input/QUEST_BUTTONS.md,
user page docs/CONTROLLERS.md). FrameBridge enables `XR_VALVE_frame_controller_interaction` and, after a game's Touch
suggestion succeeds, suggests `/interaction_profiles/valve/frame_controller_valve` bindings translated from the
accepted Touch set (SteamVR uses a Frame binding before its own Touch remap: Touch B/Y on all three top buttons).

Headless check on the device (I Am Cat, Unity + OVRPlugin, mode 2, `input_diag=1`): the runtime offers and enables the
extension in a Lepton app, and accepts the generated Frame suggestion (`quest_buttons: frame_controller bindings
(mode 2, 27 bindings, 7 dropped): 0`); the game's Touch (34 paths) and hand-interaction suggestions were accepted as
before; 72 fps, no new errors. Translations as in the JSON at the time: left `x/*` -> `dpad_down/*`, `y/*` -> `dpad_left/*`,
`menu/click` -> `view/click`. Not provable headless (tracking needs the headset worn): that SteamVR then reads the
buttons from the Frame binding; one launch in the headset settles it.

Pitfall seen during the test: OVRPort runs from two sessions on the same data folder share
`overport-workspace/dec/working.apk` and fail at once (`OVRPort failed (exit 1)`, `FileAlreadyExistsException` in
OVRPort's own output, which the CLI doesn't print without a verbose reporter). Build one game at a time per data folder.

Headset round 2026-10-18 (owner, TestQuestControllers): the button mapping works. Owner's corrections, now in the JSON:
Quest's second button sits up and toward the other hand, so Quest pair = left Y on d-pad right, right B on X (Mirror
follows: X/A = bottom + outer, Y/B = top + inner). The d-pad has one capacitive sensor: a thumb on it touches every
direction, so X and Y touch light together (left as is, hardware limit); the right face buttons sense touch singly.
SteamVR's own Touch remap (`/opt/steamvr/drivers/frame_controller/resources/input/frame_controller_remapping.json`,
layout `oculus_touch`) is the diagram's "SteamVR default": X -> d-pad down, Y -> d-pad left/right/up, A -> A,
B -> B/X/Y, Menu -> View, plus trigger click -> bumper. Games with their own Valve binding file in that folder
(Beat Saber, Batman, Accounting+, In Death, ...) use that file instead.


## Fixed layer resolution / Batman (#406, #394)

Batman: Arkham Shadow (`com.camouflaj.manta`, Unity + OVRPlugin, OpenXR/Vulkan) submits
`2768x2768`, then corner-anchored `rect=0,32 2736x2736`, `rect=0,64 2704x2704` inside the
same swapchain. On SteamOS 0.5.6 Valve's FDM injection covers the full swapchain: shrinking
viewports can move the sharp centre towards the top right and expose a coarse white right edge.
The runtime recommendation driving OVRPlugin's dynamic resolution remains unverified.

`adapter.fixed_resolution=1` (default off, Batman recipe on) overrides
`xrGetRecommendedLayerResolutionMETA`: tracked full swapchain dimensions, `isValid=XR_TRUE`.
The existing rect_clamp size table already records successful xrCreateSwapchain calls and
retires entries at destruction. Projection views must have matching sizes; quads are supported;
unknown swapchains, mixed sizes and other layer types retain the runtime behavior. Submitted
rectangles are never enlarged after rendering. Full resolution may increase GPU load.

The cached OVRPort 3.4.3-aa54c3f dispatcher contains both the extension and function name
(and a runtime-missing diagnostic). Its fixed routing table is not a guarantee that every
version forwards the call unchanged. Enable the existing arm64 xrshim for this setting:
OVRPlugin's loader lookup reaches `framebridge_extension_proc` directly, while native OpenXR
loaders use the adapter's xrGetInstanceProcAddr hook. No extension hiding: keeping the
recommendation API available avoids relying on an unverified OVRPlugin fallback. The adapter
revision stays unchanged because the new setting is opt-in; the catalog requires 1.2.2 so older
remote clients keep their bundled recipe (`recommend.catalog.unusable_reason`). Rebuild and
reinstall Batman to receive the binary and shim routing; this is not a settings-only fix for
an old APK. Device/headset verification is pending.

Headless check: expect `extension shim: xrGetRecommendedLayerResolutionMETA -> FrameBridge`
for OVRPlugin, then up to five `fixed_resolution: recommended WxH -> 2768x2768 (runtime=...)`
lines. Compare the size to the actual eye swapchain creation log; scaling can change it.
`projection view 0 rect change: ... rect=0,0 2768x2768` (and view 1) should remain full size
through gameplay/loading transitions. Rect changes continue to log at most once per second
per eye, comparing against the last logged state so a throttled final change is still shown.
No recommendation logs means this API path was not exercised: do not claim the fix worked.
Headset check: sharp region centred in both eyes, no white right edge, correct menus/loading
fades, and acceptable pacing with full-resolution GPU load.

This is separate from #303's `foveation_storage_fix` (clearing SUBSAMPLED allocation flags;
see PLAYBOOK and FRAME_RUNTIME), which fixes black/coarse storage corruption. It also leaves
#394's `FOVE_LEVEL=lo/med/hi` presets intact (commits 01ad5e6b, d7f0d514): those select the
full-swapchain density pattern, not the game's dynamic viewport.
## Embedded Khronos OpenXR loaders (GitHub #254)

PrimedGun v2 (`org.primedgun.v2`, 2.0.0-alpha.4) embeds the Khronos loader in
`libmetroid_prime_port.so` (no loader DT_NEEDED). Its diagnostics show both Android
broker queries failing, then `/vendor/etc/openxr/1/active_runtime.json`, runtime
`SteamVR/OpenXR`, and no FrameBridge messages. The supplied `openxr_android.cpp`
initializes the loader with the VM and SDL activity. This is Vulkan OpenXR; retain
the catalog's removal of `frame.vk_sanitize` (the game assembles the Vulkan soname).

`frame.static_openxr_loader` is general: analysis v23 detects arm64 game libraries
with Khronos manifest strings and negotiation/manifest markers. Complete manifest
paths (including ABI variants) are replaced in `.rodata`, NUL padded, with
`/sdcard/fp_openxr_runtime.json`. Khronos also builds paths from fragments: the
verified `platform_utils.hpp` layout uses five partition prefixes, `/etc/openxr/`,
the major API version and `/active_runtime[.aarch64].json`. For this complete table,
prefixes become `/sd` and the subdir becomes `card/fp/`; all candidates then resolve
to `/sdcard/fp/1/active_runtime[.aarch64].json`. The undecorated manifest is written;
a missing ABI-specific file falls back to it. No offsets or sizes change. **Pooled
partition strings can also be used by game code**; this cannot be ruled out from
synthetic tests. Only the complete known fragment table is accepted. Other layouts
need analysis/rewrite support before they can be patched.

Both broker authorities become `org.fp_null.openxr.*`, causing provider lookup to
fail and the loader to fall back to files even if a broker is later installed.
Only the game's copy is changed. Agent v108 writes both manifest spellings into
`<base>/lepton-data/external` (the game's private `/sdcard`), records
`static_openxr_loader` in deployment.json, and repairs them before every launch.
Launcher upgrades preserve that repair. A configuration-only finalize also writes
them. The payload is `{"file_format_version":"1.0.0","runtime":{"library_path":"libfp_xrruntime.so","name":"FramePort"}}`.

The shim exports negotiation and initialization, loads FrameBridge by soname,
forwards Android loader initialization through its GIPA, and returns that GIPA to
the outer loader. FrameBridge then loads `libopenxr_loader_original.so`, an
unmodified Khronos loader reused from OVRPort's downloaded generic runtime.
This works for recipes with `overport: False`: the patch supplies the original
loader before `frame.adapter` installs FrameBridge, its settings payload and
foveation support. Settings still use `settings.conf` through the normal launcher.
No Meta dispatcher is needed. The loader's unmodified system paths keep it from
recursing through the game manifest. Android linker namespaces, symbol binding
between the two loaders, runtime handles across the chain, and FrameBridge's
behavior without the OVRPort dispatcher still require a device check.

Source verification: Khronos OpenXR-SDK-Source
[`release-1.0.34/src/common/platform_utils.hpp`](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/release-1.0.34/src/common/platform_utils.hpp)
and the same fragment layout in release-1.1.53;
[`manifest_file.cpp`](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/release-1.0.34/src/loader/manifest_file.cpp)
leaves a bare library filename unchanged for dlopen and tries brokers before files;
[`runtime_interface.cpp`](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/release-1.0.34/src/loader/runtime_interface.cpp)
forwards loader initialization to the runtime export before negotiation (or through
GIPA afterwards). The reporter's diagnostic ELF summary contains no embedded
loader version or raw string table; the exact game's loader revision and binary
rewrite remain unverified. Host tests cover complete and assembled synthetic ELF
layouts, idempotence, unchanged other bytes, APK apply/validate with no OVRPort,
negotiation rejection, Android init forwarding and missing-chain failure. No Frame
connection or device test was performed.

**Crash at Play (GitHub #254, 2026-10-23):** the reporter's 1.3.0 build crashed right after `xrCreateInstance`
(stack overflow in SDLThread: FrameBridge GIPA → `libopenxr_loader_original.so` → the game's library → FrameBridge
…). Lepton mounts Valve's implicit layer manifest at `/vendor/etc/openxr/1/api_layers/implicit.d/`, and the log shows
`XR_APILAYER_VALVE_fdm_injection` loaded by BOTH loaders: one shared layer library stacked into two chains, so the
inner chain's next call landed in the game loader's code. The rewrite now also turns the game loader's implicit-layer
suffix `/api_layers/implicit.d` into `/api_layers/fp_none.d` (Khronos builds `<partition>/etc` + `openxr/1` + suffix),
so only FrameBridge's inner loader loads implicit layers, as in every other FramePort game (patch revision 2). Not
device-verified: headless, PrimedGun stays in its own launcher without the game's disc image, so the catalog keeps
the patch off until a headset check.

Headless acceptance after rebuilding and installing with this development build:
`uv run frameport test org.primedgun.v2`. Expect `fp_xrruntime: chain resolved:
FrameBridge`, `fp_xrruntime: xrInitializeLoaderKHR forwarded to FrameBridge: 0`,
`fp_xrruntime: negotiated runtime interface 1: FrameBridge`, then FrameBridge's
`scale=... loader=...libopenxr_loader_original.so`, `xrCreateInstance` and pacing /
`new layer:` lines. The outer loader should log the private sdcard manifest, the
inner loader the real vendor manifest. Require RUNNING and submitted frames;
headset rendering and controls need separate confirmation.
