# Quest controller mode: native stream

## Original implementation

- `native/adapter/quest_buttons.c` parses the numeric adapter setting. Only 0, 1 and 2 are accepted; malformed,
  fractional and out-of-range values select 0 and log `quest_buttons: invalid setting; using 0`.
- `xrCreateInstance` reuses the existing next-layer extension enumeration. Modes 1 and 2 enable
  `XR_VALVE_frame_controller_interaction` only when offered, without adding a second copy when the game requested
  it. An unavailable/failed enumeration disables this feature. Failed instance creation clears the enabled flag.
- `suggest_frame()` in `session_fixes.c` translates successful Touch suggestions using the tables corresponding to
  `quest_buttons.json`. It preserves action handles and click/touch components, supports both hands, poses,
  thumbstick vector/components and haptics, and counts unknown/dropped source bindings. Each input produces at most
  two Frame bindings. Mode 0 adds no runtime calls or feature logs.
- Touch Pro/Plus fallback translates the filtered Touch-shaped copy after Touch succeeds. Proximity-emulation
  additions are deliberately excluded. A later Touch suggestion submits a fresh Frame set. Frame suggestion errors
  are logged without changing the game's result. A successful game-owned Frame suggestion prevents subsequent
  automatic Frame suggestions; if it arrives after an automatic suggestion, its runtime call replaces that set.
- With the setting and extension enabled, `xrGetCurrentInteractionProfile` translates Valve profiles to Touch even
  with `controller_fix=0`. Existing `profile_remap` then still reports the game's Touch Pro/Plus profile when needed.
  Input diagnostics continue to show the runtime's original profile and additionally log every translated binding.
- Both shipped adapter architectures were rebuilt, with updated `artifacts/SHA256SUMS`. No agent, analysis, patch
  or setting revision was changed. Contract files and the VrApi bridge are unchanged.

## Original offline validation

Commands run with `UV_CACHE_DIR=/tmp/frameport-qb-native-uv` to keep uv cache writes inside the writable sandbox;
`--offline --no-sync` uses the existing worktree environment without fetching dependencies.

```sh
FRAMEPORT_NATIVE_TESTS=1 uv run --offline --no-sync pytest -q tests/test_quest_buttons.py tests/test_profile_report.py
uv run --offline --no-sync python native/build.py --only adapter
uv run --offline --no-sync python scripts/patch_docs.py --check
uv run --offline --no-sync pytest -q
```

The new host harness includes `quest_buttons.c`, `input_diag.c`, and the complete `session_fixes.c`, plus verbatim
copies of the adapter's exported create-instance/current-profile functions. It compiles with host `cc -Werror`.
Python derives expected Frame bindings from the unchanged JSON contract, independently of the C mapping table.
The targeted run passed 73 tests, including the existing profile-report tests. Two consecutive adapter builds
produced identical SHA-256 hashes for both architectures; the manifest matches both files. Documentation checks pass.

The cached NDK/OpenXR inputs were already present. During the builds, the worktree's ignored `native/.cache` was
temporarily a local directory containing input symlinks, so shader intermediates were written only inside this
worktree. The original shared-cache symlink was restored afterward. No inputs were fetched or changed and no device
was contacted.

Additional native baseline issue: `FRAMEPORT_NATIVE_TESTS=1 ... tests/test_input_diag.py` has an existing failure in
`test_logs_each_finding_once_and_nothing_when_off`. Its harness requests `input_diag_hook("xrCreateAction")`, which
the baseline implementation no longer exposes: action diagnostics moved to `session_fixes.c`. Compiling the harness
against pristine `25e282d`'s `input_diag.c` reproduced the same line-85 failure. That unrelated behavior/test was left
unchanged.

Full-suite result: **1,993 passed, 116 skipped, 12 failed, 16 errors**. All 28 failing/erroring cases stop at socket
creation with `PermissionError: [Errno 1] Operation not permitted`: local HTTP fixtures in deep links, link updates,
devkit, livestream, setup URL, and pairing UI tests. The sandbox disallows even loopback sockets. No feature
assertion failed. The required unfiltered command was run; a green unfiltered run still requires a host that permits
local sockets. The socket restriction was not relaxed.

An additional run deselecting exactly those 28 unsupported cases passed: **1,993 passed, 116 skipped, 28 deselected**.

## Integration questions and limits

- The contract has no missing mapping needed by this implementation. The right controller's Menu button has no
  mapping in modes 1/2; only left Touch `menu/click` maps to Frame `view/click`. Unknown paths, including menu touch
  and proximity, are intentionally dropped.
- The game's own Frame bindings win only after its Frame suggestion succeeds. A rejected Frame suggestion does
  not suppress automatic translation. This avoids disabling working input because the game submitted an invalid set.
- Headless/host tests cannot establish SteamVR's actual preference for these suggested bindings, physical button
  layout, capacitive touch behavior, haptics, or whether the installed Frame runtime offers the extension. These
  require the maintainer's checks below. An empty translated set is submitted and its result logged/ignored like
  any other Frame suggestion; the runtime can reject it.

## Maintainer device checks (not executed)

Use an isolated `.home` already containing a test library entry, its signing key/toolchain, and a paired test Frame.
Use a normal Quest OpenXR game and a VrApi game using the bridge. Repeat with a game that suggests only Touch
Pro/Plus if available. Replace the sample package name with each game's actual package. Stop other games first.

1. Rebuild/reinstall the test game so the new adapter is actually inside the APK. A setting update alone cannot
   upgrade an already installed adapter. Keep the original `controller_fix` and `input_diag` values for restoration.

   ```sh
   export FRAMEPORT_HOME="$PWD/.home"
   export PKG=com.example.game
   uv run frameport install "$PKG" --force
   ```

2. Collect a headless startup log for each mode, with `controller_fix=0` and diagnostics enabled. Each launch test
   stops its game before the next iteration. The script uses the remembered Frame in this isolated home.

   ```sh
   for MODE in 0 1 2; do
       export MODE
       uv run frameport settings "$PKG" quest_buttons="$MODE" input_diag=1 controller_fix=0
       uv run python - <<'PY'
   import os
   from pathlib import Path
   from frameport.cli import _target
   from frameport.core.events import Reporter

   target = _target(None)
   try:
       result, log = target.launch_test(os.environ["PKG"], Reporter(), seconds=30)
       output = Path(os.environ["FRAMEPORT_HOME"]) / f"quest-buttons-mode-{os.environ['MODE']}.log"
       output.write_text(log)
       print(output)
       for line in log.splitlines():
           if "quest_buttons" in line or "controller profile" in line:
               print(line)
   finally:
       target.frame.close()
   PY
   done
   ```

   Expected mode 0: no `quest_buttons:` or `input_diag: quest_buttons` lines. Modes 1/2, if offered:

   ```text
   quest_buttons: extension XR_VALVE_frame_controller_interaction enabled
   quest_buttons: frame_controller bindings (mode 1, K bindings, D dropped): 0
   input_diag: quest_buttons /user/hand/left/input/x/click -> /user/hand/left/input/dpad_down/click
   input_diag: quest_buttons /user/hand/left/input/x/click -> /user/hand/left/input/dpad_right/click
   input_diag: quest_buttons /user/hand/left/input/menu/click -> /user/hand/left/input/view/click
   ```

   `K`/`D` depend on the game. Mode 2 reports `mode 2` and only the first X mapping above. Face-button touch paths
   keep `/touch`. Frame suggestions must return `0`; a negative result indicates runtime rejection to investigate.
   A Touch Pro/Plus-only game should also log `controller profile ... -> oculus/touch_controller` and a Frame binding
   summary. If the runtime lacks the extension, expect
   `quest_buttons: extension XR_VALVE_frame_controller_interaction not offered by the runtime`, with no translated
   bindings; input should retain its previous behavior.

3. For each mode, set it with the command below, then Play from the FramePort library or Steam while wearing the
   headset. Exit through Steam before changing modes; settings are read at process startup.

   ```sh
   uv run frameport settings "$PKG" quest_buttons=1 input_diag=1 controller_fix=0
   ```

   Mirror: left down/right = Touch X; left up/left = Touch Y; right A/X = Touch A; right B/Y = Touch B. Quest pair:
   left down = X, left left = Y, right A = A, right B = B; the unused face buttons should not activate those actions.
   In both modes left View activates Touch Menu. Check thumbstick movement/click/touch, trigger/squeeze values,
   trigger touch, tracking/aim, and haptic feedback. Verify both hands remain connected with `controller_fix=0`.
   Mode 0 should match the previous SteamVR mapping. Capacitive-touch verification needs a game that exposes finger
   animation or an input sample; absence of a suggested touch action is not a failure of this mapping.

4. Restore settings after stopping the game. The example restores the usual defaults; substitute recorded original
   values if different. Reinstalling the prior known-good APK also removes the new adapter if an artifact rollback
   is needed; setting 0 alone disables the feature on the next launch.

   ```sh
   uv run frameport settings "$PKG" quest_buttons=0 input_diag=0 controller_fix=1
   ```

For explicit current-profile verification in an OpenXR input sample, call `xrGetCurrentInteractionProfile` for
`/user/hand/left` and `/user/hand/right`, then `xrPathToString` on each non-null result after action-set attachment.
With modes 1/2 and `controller_fix=0`, expect `/interaction_profiles/oculus/touch_controller`, or the game's own
Touch Pro/Plus profile when its binding was remapped. Existing `input_diag` profile lines intentionally show the raw
runtime answer, so they cannot alone verify the profile returned to the game.

## Round 2

The review fixes supersede the original proximity exclusion, broad Valve-profile rewrite, empty-set submission,
and failed-instance state handling described above:

1. Added bare `grip`/`aim` to the JSON common mapping and `FRAME_COMMON`; no other contract fields changed.
2. Current-profile rewriting with `controller_fix=0` requires the exact Frame profile and a successful generated
   binding set still in effect. A successful game-owned Frame suggestion ends that ownership; failed suggestions
   leave the last accepted set in effect. Existing `controller_fix=1` behavior is retained.
3. Frame bindings are generated at the point a Touch set succeeds, including proximity-emulation touch bindings.
   Direct Touch, Touch Pro/Plus remapping, and rejected-expansion fallback all use the effective accepted set.
4. An all-dropped translation makes no Frame suggestion and logs
   `quest_buttons: frame_controller bindings (mode N, 0 bindings, D dropped): not suggested`. An earlier accepted
   generated set can remain stale; it is intentionally retained because an empty replacement is invalid.
5. Extension decisions remain local until instance creation succeeds. Only successful creation publishes the active
   instance and resets binding ownership/Touch-remap state; a failed second attempt preserves the live first state.
6. One mutex serializes each complete suggestion transaction, including fallback/remapping, Frame generation, and
   flags. Instance-state publication and current-profile reporting use the same mutex. Internal helpers call the
   next layer directly and never reacquire it.

Regression tests reproduced items 1–5 before the fixes (21 failed cases). Afterward, the requested host run passed
97 tests, including timeout-bounded nested proximity fallback and Touch Pro remapping. Diagram regeneration made no
changes; controller-diagram and patch-documentation checks pass. Both adapter architectures were rebuilt twice with
identical hashes matching `SHA256SUMS`. The full suite finished with 2,015 passed, 140 skipped, 12 failed and 16 errors;
all 28 failures/errors are the same sandbox-denied local socket creation described above.

Device follow-up: repeat the install/log/physical checks above, adding `proximity_emul=2` to the mode-1/mode-2 settings
command for a game exposing finger proximity. Expect translated proximity actions on face-button/thumbstick touch
and trigger touch, and working finger animation. In an OpenXR sample with `controller_fix=0`, also query profiles
before any generated set, after a successful generated set, and after the app's own successful Frame suggestion:
expect Frame, Touch, Frame respectively. Another Valve profile must stay unchanged. Restore the recorded original
`proximity_emul` setting along with the other settings. No device operations were performed in this worktree.
