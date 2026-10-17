# App stream: Quest controller buttons

## Implementation

- Added `adapter.quest_buttons` (integer, default 0), common Controllers choice with SteamVR default / Mirror /
  Quest pair. It is available on both bridge ABIs, is not an XR shim setting and has no detection suggestion.
  No agent, patch, setting or analysis revision was changed.
- `patches.settings.adapter_settings(..., defaults=...)` and the headset identity wrapper stay pure.
  `core.controllers.effective_adapter_settings` supplies the library preference at build, install, live suggestion
  and dialog boundaries. Recipes override it, including an explicit 0. APK `libframe_settings.so` and installed
  `settings.conf` receive the effective value.
- Recipe/APK/configuration fingerprints do not read the global preference. The existing agent updates its
  configuration fingerprint during `set_settings`; `game_update.configuration_matches` therefore accepts all
  three inherited controller values, while still checking explicit overrides and every other configuration field.
  An offline test exercises the real agent handler to prevent a global apply making the library outdated.
- Game settings has a **Use global default (mode)** option. Saving unchanged inherited settings keeps the recipe
  unchanged; choosing SteamVR default stores an explicit 0. Reset restores the effective global value.
- Settings → Controllers stores `input.quest_buttons`, shows a diagram for the choice, and offers **Apply to
  installed games now** while connected. The existing FIFO Frame tool job pushes only `quest_buttons` through
  `target.set_settings`. It skips non-Quest games, flat Android apps known to the library, and recipe overrides.
  A library-less install with an override listed in its deployment is also skipped. Cancellation is checked per game.
- The CLI already has `settings <pkg> quest_buttons=N` (runtime only) and `recipe <pkg> --set quest_buttons=N`
  (persistent recipe). No global CLI option was added: there is no existing general library configuration command,
  and `settings` specifically writes an installed game's settings. The global preference is available in the GUI.
- `scripts/controller_diagram.py` reads the unmodified JSON contract and literal Portal theme colours. It generates
  the full four-panel SVG, three bundled compact SVGs and the marked Markdown table. `--check` checks all outputs;
  `--compact --mode N [--out path]` generates just one pair. Bundled images use the existing glyph asset copy and
  URL path, with no image bytes or filesystem reads in the diagram render helper.
- Added the user guide and README/install/playbook/runtime links, regenerated the patch docs, site map and
  translation template. The only agent edit corrects the old D-pad comment. No shared smoke script was edited.

## Offline checks and visual review

Commands use the existing venv without resolving packages. The default uv cache is read-only in this sandbox;
plain `uv run` attempted an editable metadata refresh requiring unavailable cached build dependencies.

```sh
export UV_CACHE_DIR=/tmp/frameport-qb-uv
export UV_NO_SYNC=1
uv run pytest -q
uv run python scripts/patch_docs.py --check
uv run python scripts/controller_diagram.py --check
uv run python scripts/i18n_extract.py --check
```

The focused settings, update, translation, patch and UI tests passed (147 passed, 2 skipped before the final
agent-fingerprint regression; the follow-up controller/update/UI-state run passed 89 tests, with the existing
socket test deselected). Final full suite: **2015 passed, 45 skipped, 12 failed, 16 setup errors** (67.76 s). All 28 remaining failures/errors
are existing local-socket tests rejected by the sandbox (deeplink, devkit, install links, link updates, livestream,
setup URL and pairing). The translation failure from the first run was fixed. The changed feature's tests pass;
the full suite cannot be reported as passing in this environment. The log is local at `/tmp/qb-app-final-pytest.log`.
Patch docs, diagram and translation generation checks pass.

The real Flet smoke command was attempted:

```sh
FRAMEPORT_NO_UPDATE_CHECK=1 FRAMEPORT_NO_ANNOUNCEMENTS=1 \
  uv run python scripts/ui_smoke.py --out .shots --fake-frame \
  --game com.BithellGames.Arcsmith --only tools,game-settings-dialog
```

It cannot start here: the sandbox rejects even creation/binding of loopback sockets (`PermissionError: [Errno 1]
Operation not permitted`). Flet reports it cannot bind its server on port 8557. Consequently no Settings or Game
settings PNG was produced or inspected. This is an outstanding maintainer validation step, not a passed UI check.
No Frame or external network connection was attempted.

The diagrams were rendered offline with ffmpeg and visually inspected. Both are readable, with correct left/right
positions and dimmed unbound buttons. Local, uncommitted review PNGs:

- `.shots/quest-buttons.png` — all four panels.
- `.shots/quest-buttons-compact.png` — the app's Mirror image.

The Impeccable mechanical detector returned no findings on the changed UI files; targeted Ruff and whitespace
checks passed.

## Capture the actual UI on a machine that permits loopback

Use a test library in `FRAMEPORT_HOME` containing a Quest game. `ui_smoke.py` has no Controllers section option,
so this temporary wrapper reuses its real browser/driver and scrolls the two relevant surfaces. It does not change
tracked files. Set `pkg` to a Quest game in that test library first.

```sh
pkg=com.example.questgame
cat > .home/capture_controllers.py <<'PY'
import sys
import time
sys.path.insert(0, 'scripts')
import ui_smoke as smoke

original_driver = smoke.driver

def driver(app, steps, *args):
    adjusted = []
    for name, action in steps:
        if name == 'tools':
            def controllers(a, original=action):
                original(a)
                time.sleep(1)
                a.page.run_task(a.settings_view.show_section, 'controllers')
            adjusted.append(('settings-controllers', controllers))
        elif name == 'game-settings-dialog':
            def game_settings(a, original=action):
                original(a)
                time.sleep(1)
                dialog = next(d for d in reversed(a.page._dialogs.controls)
                              if d.open and type(d).__name__ == 'AlertDialog')
                a.page.run_task(dialog.content.content.scroll_to, offset=400, duration=0)
            adjusted.append(('game-settings-controllers', game_settings))
        else:
            adjusted.append((name, action))
    return original_driver(app, adjusted, *args)

smoke.driver = driver
raise SystemExit(smoke.main())
PY
FRAMEPORT_NO_UPDATE_CHECK=1 FRAMEPORT_NO_ANNOUNCEMENTS=1 \
  uv run python .home/capture_controllers.py --out .shots/controllers \
  --fake-frame --game "$pkg" --only tools,game-settings-dialog
```

Inspect `.shots/controllers/01-settings-controllers.png` and
`.shots/controllers/02-game-settings-controllers.png` at 1280×820; repeat with `--viewport 1024x768`. Verify the
choice text, diagram, explanation and apply button fit; scroll a little in the dialog if the game's relevant
Picture settings make offset 400 miss the row. Also choose all three modes and the global option in the actual
GUI, checking that the diagram changes and re-opening preserves inheritance or the explicit override.

## Exact device verification after native integration

These steps are for the maintainer; none were executed in this stream. Merge the native stream and its adapter
artifacts first. Use a Quest game that binds all four Touch face buttons (and, separately, an available 32-bit
bridge game). Set `pkg` to its real library package; keep the same `FRAMEPORT_HOME` used for that test library and
its paired Frame. Close the game before rebuilding/installing.

1. Build/install the integrated adapter once. With global mode Mirror and the game's **Use global default**
   selected, run:

   ```sh
   pkg=com.example.questgame
   uv run frameport build "$pkg"
   uv run frameport install "$pkg" --force --apk-only
   uv run frameport settings "$pkg" input_diag=1
   ```

   Confirm the build's `lib/<abi>/libframe_settings.so` contains `quest_buttons=1`. The following PC-side inspection
   reads the APK path recorded by the build:

   ```sh
   uv run python - "$pkg" <<'PY'
   import sys, zipfile
   from frameport.core import library
   with zipfile.ZipFile(library.game(sys.argv[1])['build']['apk']) as apk:
       for name in apk.namelist():
           if name.endswith('/libframe_settings.so'):
               print(name, apk.read(name).decode())
   PY
   ```

2. For each mode, write it and launch from Steam/FramePort Play while wearing the headset:

   ```sh
   uv run frameport settings "$pkg" quest_buttons=1 input_diag=1
   # Close the game, then repeat with quest_buttons=2, then quest_buttons=0.
   ```

   On the Frame, set `pkg` to the same package and locate its actual base (works for internal storage or another
   drive). After each launch inspect:

   ```sh
   pkg=com.example.questgame
   base=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["base"])' \
     "$HOME/Applications/quest-frame/$pkg/deployment.json")
   grep '^quest_buttons=' "$base/settings.conf"
   grep -E 'quest_buttons:|input_diag: quest_buttons' "$base/launch.log"
   ```

   Expected for modes 1/2:

   ```text
   quest_buttons: extension XR_VALVE_frame_controller_interaction enabled
   quest_buttons: frame_controller bindings (mode N, K bindings, D dropped): <XrResult>
   input_diag: quest_buttons <touch path> -> <frame path>
   ```

   `N` is 1 or 2, `K` is nonzero, and the Frame suggestion result must indicate success. Input diagnostics must
   match the JSON paths. For mode 0 there should be no new FrameBridge Frame suggestion. Check the physical face
   buttons against every row of `docs/CONTROLLERS.md`: Mirror repeats X/A on bottom+inner, Y/B on top+outer;
   Quest pair leaves left up/right and right X/Y unbound. Left View sends Menu in every mode. Check touch actions
   where the game exposes them, triggers, grip pressure, sticks, poses and haptics. Bumpers are unbound. Repeat on
   the 32-bit bridge ABI. Existing custom SteamVR bindings should still take precedence.

3. Verify global apply with two installed games: one following the global choice, one explicitly set to SteamVR
   default (0). Set Settings → Controllers to Quest pair; click **Apply to installed games now**. Expect one FIFO
   Frame tool job, `quest_buttons=2` only on inherited games, and the explicit game's 0 retained. Check that neither
   game gains an Update ready badge solely from the global change/apply. Re-open Game settings: the inherited
   choice says **Use global default (Quest pair)**. Relaunch to verify the mode; a running game keeps its old mode.
   Also test changing an explicit mode and then resetting to the global choice.

4. If an extension-less runtime is available, modes 1/2 must log
   `quest_buttons: extension XR_VALVE_frame_controller_interaction not offered by the runtime` and retain the
   SteamVR default mapping without a launch failure. Otherwise leave this case to the native offline tests.

5. Undo: set the global choice to SteamVR default and apply to inherited installed games. Restore any pre-test
   per-game choices in Game settings, and disable diagnostics:

   ```sh
   uv run frameport settings "$pkg" quest_buttons=0 input_diag=0
   ```

   A persistent test override made with the recipe CLI can be removed with
   `uv run frameport recipe "$pkg" --disable adapter.quest_buttons`; then select the global choice and save in
   Game settings to push its current effective value.

## Contract questions / remaining integration work

- The contract translates successful Touch suggestions. A game suggesting only its own Frame bindings is
  unaffected. If a game suggests both Touch and its own Frame profile, ordering/priority is not specified by the
  contract; verify with the native stream before claiming both-profile games are unaffected. The user guide
  qualifies this limit accordingly.
- No device claims were added. The runtime reference marks new-mode verification pending integration.
- A direct CLI `settings` write is intentionally temporary relative to the recipe/global preference. Use
  `recipe --set quest_buttons=N` or the dialog for a persistent per-game override.
- Actual UI screenshots and the socket-dependent full-suite tests require a less restricted local environment.

## Round 2

- Global apply still writes every inherited Quest install, but separates games ready for the next launch from
  games that need **Update on Frame first**. The job result supplies the completion toast, so both show the same
  counts. The Settings explanation includes the one-time update requirement.
- `_build_inputs_changed` also checks the converter and source APK, so it cannot decide adapter support alone.
  Its manifest reader was extracted into `build.native_checksums` and is shared by `_build_inputs` and the
  controller check. Only adapter entries are compared. Missing/different checksums or an unknown/different
  installed APK are reported as needing an update; unrelated artifact changes do not affect this count. This is
  deliberately conservative when an older adapter already supports the feature but differs from the bundled one.
- The section reads its default once into a local snapshot, uses that for the dropdown and preview, and updates it
  on selection. Regression tests check the single read and that the existing preview control changes in place.
- The user guide now covers game-owned Frame bindings/profile preservation, bare binding paths and continued
  proximity emulation. This supersedes the earlier question about games suggesting both Frame and Touch profiles.
  The maintainer's triangle arrows and shared JSON were left unchanged.
- Focused tests: **135 passed, 2 skipped** (`test_controller_preferences.py`, `test_patches.py`,
  `test_game_update.py`). Patch-doc, translation-template and controller-diagram checks pass; the translation
  template was regenerated. Targeted Ruff and whitespace checks pass.
- Device follow-up: on an old/unknown install, choose Mirror and apply globally. Expect its setting to be pushed but
  the result/toast to count it under **needs Update on Frame first**. Update that game once, apply again, then
  relaunch: expect it in the next-launch count and the existing mode-1 binding logs described above. Keep a second
  game with an explicit override and verify it is skipped throughout. Undo by choosing SteamVR default and applying
  again; restore any per-game test overrides. No device or external network access was used in this round.
- Final whole suite: **2029 passed, 116 skipped, 12 failed, 16 setup errors** (67.52 s). The remaining failures
  and errors are the existing local-socket tests blocked by the sandbox, as in Round 1. Full log:
  `/tmp/qb-app-round2-pytest.log`. No test skips or socket workarounds were added.
