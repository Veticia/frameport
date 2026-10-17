# Rift through FEX: what RiftLift and xrizer teach this branch

Reviewed 2026-10-17: [Villagers654/RiftLift](https://github.com/Villagers654/RiftLift) at `20a91ff` (0.11.1.2, MIT;
a Revive-derived Oculus runtime + Meta sign-in/downloads, Windows and Linux/GE-Proton) and
[Supreeeme/xrizer](https://github.com/Supreeeme/xrizer) at `63fc7eb` (OpenVR implemented on OpenXR, Rust,
**GPL-3.0**: ideas only, no code copied). RiftLift lists Lone Echo, Lone Echo 2, Stormland, First Contact and Vader
Immortal I as rendering on Linux (desktop GPUs, x86_64; not under FEX, not on a Frame).

## Adopted on this branch

| From | What | Where |
|---|---|---|
| RiftLift `detection.py` | LibOVR / Platform SDK / OpenVR / Unity facts from the game's files + delay imports | `fexrift_compat.oculus_facts` |
| RiftLift `_select_runtime_backend` | Route by capabilities, never a title list: OpenVR builds without the Platform SDK skip Revive *and* Meta's runtime (no OVRServer/OVRLibrarian under FEX) | `vr_route`, launch.sh `FP_META_RUNTIME` |
| RiftLift `oculus_launch_arguments` | Engine VR switch when the arguments don't choose one (Unreal `-vr`; Unity `-vrmode`, here `OpenVR` on the direct route) | `vr_args` (unverified games only) |
| RiftLift `DEBUG_WINE_CHANNELS` | One opt-in Wine debug profile (loader, wintrust/chain, openxr/vrclient) instead of ad-hoc WINEDEBUG edits | `<anchor>/debug-wine` |

Details and limits: [compat.md](compat.md#route-and-vr-arguments-from-the-builds-files). None of it is
headset-verified yet; First Contact's verified launch is unchanged.

## Not adopted (FramePort rules)

- **RiftLift's Platform SDK shim** (`compat/riftlift-platform-shim.c`) replaces `LibOVRPlatformImpl64_1.dll` and
  answers `ovr_Entitlement`, login and user-proof requests locally (fixed user id). RiftLift gates *downloads* on
  ownership, but at run time this is an entitlement bypass. FramePort never changes entitlement or Platform SDK
  answers: this branch keeps Meta's own runtime answering them (and "Missing entitlement" stays a correct result).
- **Making the redirected runtime load past the Oculus signature check.** RiftLift's runtime patches the game exe's
  import table (`PatchMainImport`: `LoadLibrary*`, `GetModuleHandle*`, `OpenEventW`) because Detours on kernel32
  alone may miss calls that Wine binds straight to kernelbase. That would explain our 2026-10-17 local finding
  ("Revive's LoadLibrary hooks don't take effect even on x86_64 Wine"; untested). Combined with
  `GetModuleHandle("LibOVRRT64_1.dll")` returning the Revive module, the static LibOVR shim can end up using a
  runtime that never passed its Authenticode check, which CLAUDE.md rules out.
  The branch doesn't need it: Meta's signed `LibOVRRT64_1.dll` (on PATH) loads normally while OVRServer runs.
  An IAT-patched `LoadLibrary`/`OpenEventW` *without* the GetModuleHandle part would be a pure runtime substitution
  (like `pcvr.libovr_redirect` on main) and is worth a separate owner decision for main's Proton-arm64 path.

## Worth doing next (owner decisions)

1. **Meta sign-in and owned games without the Electron app** (RiftLift `meta_auth.py`, `entitlements.py`): Meta's
   native-SSO flow (`auth.meta.com/native_sso/confirm` → `meta.graph.meta.com`) on the PC, then
   `graph.oculus.com/graphql` (`active_entitlements`, PC items) lists owned Rift apps. FramePort could offer a
   "your Meta library" list and install owned games from Meta's CDN with `meta_fetch_app` (segment + file SHA-256 are
   already checked). The runtime in the Frame prefix still needs its own signed-in session for the Platform SDK, so
   this replaces the *library and download* half of `fex login`, not the runtime sign-in.
2. **An OpenXR route for OVRPlugin builds** (RiftLift `openxr-layer`, xrizer's design): newer Unity/Unreal Oculus
   integrations talk OpenXR and refuse runtimes not named "Oculus"; RiftLift's layer reports `runtimeName` "Oculus"
   to clients named "Oculus VR Plugin". On the Frame this also needs the x86_64 build of FramePort's timefix layer
   (SteamVR accepts only OpenXR 1.0) and a fix for wineopenxr's `openxr_vulkan_instance_extensions` error.
3. **DXVK fence fix** (RiftLift `components/dxvk/monotonic-fence-signals.patch`): skips non-increasing timeline
   semaphore signals from shared D3D11 fences. Only relevant if a Revive-composited game shows VK device-lost or
   fence validation errors; GE's DXVK is unpatched.
4. **xrizer** itself doesn't fit the Frame: SteamVR is the native OpenVR runtime there, and xrizer would only
   add a translation (OpenVR → OpenXR → SteamVR). Its input layer (action manifests, legacy input, per-controller
   profiles) is a good reference if Revive's Touch bindings need work on the Frame controllers.
