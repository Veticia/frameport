# Offline room scanner (scan stream)

`fp_roomscan` converts recorded FPRS v1 meshes to the [room contract](ROOM_FORMAT.md), including the optional
wall label adopted from upstream commit `8cb2783`:
`room.txt` and `room-mesh.bin`, refined by default (see "Refinement" below), plus the unrefined `room-raw.txt` +
`room-mesh-raw.bin` and `scan.json` (head path, settings, per-pass report, label decisions, door/closet
suggestions; ROOM_FORMAT.md "Raw files and scan.json"). It does not capture RoomView queues, contact a headset, install rooms, or modify
the current-room symlink. Scanner work changes `native/roomscan/`, its build target and artifact/checksum,
`scripts/mr/synth_scan.py`, `tests/test_roomscan.py`, and this note. The upstream contract commit also brings its
existing prompt assets and renderer unchanged. No runtime, agent, patch, or analysis versions were bumped.

## Build and run

Use the cached NDK; no downloads are needed for this target:

```sh
python native/build.py --only roomscan
```

The result is `artifacts/linux-arm64-bin/fp_roomscan`, a static aarch64 Linux ELF with its own `_start`, raw Linux
syscalls, and no interpreter, dynamic section, libc, libm, UAPI headers, generated tables, or new dependencies.
`platform.h` is the reconstruction code's entire I/O/allocation boundary. The same eight C sources compile on the
host:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno -DFP_HOSTED native/roomscan/*.c -o /tmp/fp_roomscan
python scripts/mr/synth_scan.py /tmp/room.fprs
/tmp/fp_roomscan --offline /tmp/room.fprs --out /tmp/scanned-room --universe 0 --json
```

The output directory's parent must exist; the scanner creates the final directory if needed. It always prints
exactly one JSON summary, whether `--json` is supplied or omitted. Success exits 0, errors exit 2 with
`{"ok":false,"error":"..."}`. `--help` prints usage and exits 0. Successful summaries contain:

```json
{"ok":true,"frames":30,"skipped_frames":0,"far_vertices":0,"discontinuity_triangles":0,
 "walls":4,"detected_walls":4,"volumes":2,"floor_y":-0.0023,
 "ceiling_y":2.5992,"coverage_percent":100.0,"hint_used":false,"triangles":70997,
 "accepted_triangles":118804,"rejected_triangles":704,"heads":28,
 "refine":{"triangles":70997,"kept":7159,"structural":46121,"near_walls":148,"outside":0,"small_pieces":38,
  "thin_pieces":0,"erased":0,"walked":203,"tops":1896,"panel_triangles":12,"truncated":0,"walked_skipped":false,
  "floor_y":-0.0051,"ceiling_y":2.6028,"boxes":2,"relabelled":1,"removed_boxes":0,"suggestions":0,
  "plane_shifts":[{"kind":"wall","wall":0,"shift":-0.0004},...,{"kind":"ceiling","shift":-0.0021}],
  "output_triangles":7171,"output_volumes":2},"warnings":[],
 "timings_ms":{"read":0,"fusion":0,"model":0,"mesh":0,"refine":0,"total":0}}
```

The example above illustrates the schema; actual counts and timings depend on the input. Heights are absolute
STAGE coordinates. `frames` includes skipped frames; `skipped_frames`, `far_vertices`, and
`discontinuity_triangles` count input exclusions once, not once per pass. `walls` counts output polygon
edges including synthesized edges; `detected_walls` counts fitted physical planes. Coverage means the percentage
of the output perimeter supported by observed wall planes, **not camera coverage of all room surfaces**. Timings
include parsing, fusion, structural/furniture modelling, mesh generation, refinement, and total time including
output. The scanner fields describe the unrefined scan (`triangles`, `volumes`, `ceiling_y` are the raw files');
`refine` is the refinement report (same keys as scan.json's `report`) plus the refined `output_triangles` and
`output_volumes`. `heads` counts the head-path points kept for the walked-volume pass.

All frame headers, timestamps/vertices/poses, approximately unit quaternions, counts, indices and EOF boundaries
are checked before publication. A non-finite timestamp/pose/vertex or invalid tracking quaternion skips the entire
frame after consuming and structurally validating its record, with a count and warning. A bad frame never erases
usable frames elsewhere in the dump. Vertices outside the camera-relative range (default 4 m) and every triangle
touching them are discarded. Triangles with a camera-depth jump over `--depth-jump + --depth-ratio * nearest_depth`
are rejected before fusion; this complements long-edge and grazing rejection. Frame counts are capped at
1 million vertices / 3 million indices;
oversized records fail rather than allocating from untrusted counts. A header-only dump or one without usable
triangles fails. Empty frames may accompany useful frames. Structural corruption (truncation, unsupported header,
bad counts or indices) remains fatal, preserves any existing output byte for byte and creates no output directory.
UUIDs come from `getrandom`, fresh per scan; mesh geometry is repeatable.

All files are computed first, then each is written to an exclusive, randomly named temporary file and renamed, in
the order `room-mesh-raw.bin`, `room-raw.txt`, `scan.json`, `room-mesh.bin`, `room.txt` (meshes before their room
text, `room.txt` last). A write failure removes the temporary files. These are atomic **file** replacements, not a
multi-file filesystem transaction: the installer should scan into a fresh staging directory
and publish that directory/symlink after success. Do not run concurrent writers into the same output directory.
There is no fsync-based power-loss durability promise. Neither scan nor build writes application data outside
the worktree's `FRAMEPORT_HOME`; fixtures and test output use temporary directories.

## Reconstruction

1. The first streaming pass validates every frame, rotates its mesh by the supplied quaternion, and translates
   it into STAGE. It rejects degenerate triangles, excessively long edges, out-of-range points, and grazing
   surfaces. Winding is corrected toward the observing camera. A deterministic bounded reservoir of 32,768
   accepted triangle centroids determines median position and padded bounds. No frames are retained across reads.
2. The second pass rasterizes accepted triangles at 0.75 voxel spacing. Per-cell surfels average positions and
   inward normals, weighted by area and squared incidence. A capped accumulated weight lets later views correct
   earlier ones. The grid is sized from the sample bounds and capped at 8 x 8 x 4 metres about the data median.
   Unobserved cells have zero weight. This implementation uses a bounded dense backing array for the sparse
   observations, rather than a hash table; the 4.2-million-cell hard cap bounds all later work. Complete regular
   fans are recognized from their index topology, without hardcoding a camera resolution. Their normals come from
   12 x 12 neighbourhood covariance plane fits, rejecting patches above `--patch-residual` RMS; irregular meshes
   retain triangle normals. Each cell counts contributing frames once and needs `--min-views` evidence, capped at
   ten percent of usable frames (rounded down, at least one) for brief captures. This suppresses transient stereo
   geometry without requiring a short scan to see each surface five times.
3. Horizontal normal/height histograms select the dominant floor within the STAGE floor band and a ceiling above
   the minimum ceiling height. Missing floor uses STAGE y=0 with a warning; missing ceiling uses floor plus
   `--ceiling-default`, warning exactly `ceiling not seen`. A ceiling must also have at least 15% as many supported
   cells as all upward-facing horizontal surfaces, so a few isolated down-facing patches do not invent a ceiling.
4. RANSAC fits near-vertical planes using distance and normal agreement, then refines them by position covariance.
   Minimum height, span, and area reject furniture faces. Wall directions within the snap tolerance of the
   dominant wall's orthogonal directions are snapped, with offsets refitted. Directed wall segments are connected
   by nearest endpoints and adjacent line intersections. This preserves the L-shaped fixture's re-entrant corner.
   Parallel same-facing planes within 25 cm / approximately 14° merge, suppressing duplicate softened faces.
   Planes below 20% of the strongest original plane's area are discarded. When at least 65% of candidate wall area
   supports an orthogonal family, directions outside 12° of that family are discarded (`--snap-degrees 0` disables
   this orthogonal consensus filter). Large gaps close along observed endpoints. Ambiguous endpoints with three
   or four planes can use inward half-plane intersections; concave rooms retain segment ordering. Otherwise a
   warned observed-bounds rectangle is used. Polygon checks reject crossing edges. The output follows the contract's positive
   shoelace ordering in (x,z), with left-facing inward normals.
5. Furniture tops are clustered separately from vertical faces, within supported horizontal height bands. A
   softened edge connecting a tabletop to a wall can no longer absorb the whole room into one box. Top boxes use
   a minimum-area horizontal orientation search and the median surface height; vertical screen candidates use
   face-direction harmonics / PCA. Bounds trim the outer 2% to suppress stereo halos. Top boxes extend down to the
   floor (the footprint and lower extent remain geometric estimates). Weak/sparse top patches are discarded.
   Labels follow height, area, long-side (with two voxels of measurement tolerance),
   thinness and near-wall rules, in order BED, COUCH, TABLE, SCREEN, STORAGE, OTHER. Only volumes are emitted;
   optional table plane records are not required. Floor/ceiling/wall surfaces are excluded before clustering.
6. Surfel splats form an incidence/area-weighted signed distance field within three voxels of each surface. The
   sign is positive in observed free room space. Marching cubes traces zero crossings on cube faces, resolving
   ambiguous faces from their centre sign. It fans closed contours and orients triangles toward the positive
   gradient. Unknown corners suppress extraction rather than bridging unobserved space. If necessary, extraction
   is repeated on a coarser lattice (strides 2–4) until at most 200,000 triangles remain, with a decimation warning;
   excess beyond that fails. Vertices are unshared triangle vertices, so at most 600,000 vertices. Structural
   planes are semantic anchors, not substituted for the observed global mesh.

## Refinement

After the mesh, the scanner cleans the room the way the owner tuned it in the room-scan cleanup page (2026-10-18);
`native/roomscan/refine.c` and `rules.c` port that page's `refine()` and label rules pass for pass (local parity
check on the owner's recording: identical counts and bit-identical refined meshes, see below). `--refine 0`
switches it off: room.txt and room-mesh.bin are then byte-for-byte what the scanner wrote before refinement existed
(the raw files and scan.json are still written, so refinement can be switched on later without a new scan).

The input is exactly what the raw files hold: the raw room is written as text and read back (doubles parsed from the
4-decimal text, like the page reads it), the head path is rounded the same way. Therefore `--refine-only <dir>`
on the raw files reproduces the scan's refined files byte for byte. The mesh is welded at 1 mm (quantized, as the
page does) for the passes that need topology; the output is again unshared triangle vertices.

1. **Label rules** (`rules.c`, `--label-rules 0` off) on the scanner's volumes, using the raw room (walls, outline,
   floor): a box that holds >= 60% of two boxes whose centres are >= 1 m apart covers several things (removed); a
   box >= 75% inside a bigger box with a top no higher (+5 cm) is part of it (removed); a centre > 10 cm outside the
   outline is behind a wall (removed). A tall thin slab from the floor (thinner than 22 cm, >= 1.8 m high, bottom
   <= 0.3 m, at most 0.5 m behind the wall line) becomes a door (<= 1.1 m wide) or closet **suggestion** in
   scan.json (box removed, nothing added: the GUI decides). Thin (< 22 cm) upright boxes are SCREEN only when
   raised >= 0.3 m, top <= 2.2 m, width/height >= 1.2, within 15 degrees of the nearest wall (when closer than
   0.35 m) and flat: the raw mesh's depth spread inside the box <= 3 cm (with >= 30 triangles; fewer = unknown,
   accepted); otherwise removed. Then BED (top 0.25-0.65 m, area >= 2 m^2, long side 1.8-3 m), COUCH (top
   0.35-0.6 m, long side >= 1.4 m), TABLE (top 0.6-1.1 m), STORAGE (>= 1.1 m high, >= 25 cm deep, within 0.35 m of a
   wall); anything taller than 1.1 m otherwise is removed, the rest is OTHER. Every box gets a reason in scan.json.
   These thresholds are constants in `rules.c` (the page's `RULES0`).
2. **Furniture**: triangles within 10 cm of a ruled box (after the rules) are furniture; the passes below never
   classify, clear or walk them away, and the tubes pass keeps pieces that are mostly furniture.
3. **Planes**: seen walls (`WALL_FACE` records), floor and ceiling. Each plane's offset moves to the median signed
   distance of the non-furniture triangles facing it (normal within `--facing-deg`) within `--band` of it (walls:
   within their length + band), when there are more than 50. An unseen ceiling (floor + 2.5 m) moves to the real
   one; the re-estimated ceiling is written to the refined room.txt (ceiling record and wall tops).
4. **Structural triangles**: facing a plane, not furniture, behind it by less than `--band`, in front of it by less
   than `--band-in` (walls), `--floor-in`, `--ceil-in`; in a wall's top `--top-strip` below the ceiling the full
   band applies in front too (the depth bends where wall meets ceiling). The nearest qualifying plane owns it.
5. **Erase spheres** (`--erase "x,y,z,r;..."`, from the GUI's eraser) drop every triangle whose centroid is inside.
6. **Walked volume** (`--walked` = radius R, 0 off): where the user stood, nothing solid can be. Non-furniture
   triangles from 0.2 m above the floor up to 0.25 m above a head position, within R horizontally of it, go.
   Needs the head path (scan.json); without it the pass is skipped (`walked_skipped`).
7. **Walls** (`--walls`): `panels` (default) removes the structural triangles, then non-furniture triangles closer
   than `--near` to any plane (within the wall's extent), and adds flat panels: one quad per seen wall (moved by its
   offset shift) from floor to ceiling, and the outline ear-clipped at the floor and the ceiling heights, all wound
   counter-clockwise seen from inside. `snap` instead projects every vertex of a structural triangle onto its plane
   (the last owner wins). `off` keeps them.
8. **Furniture tops** (`--tops`, 0 off): upward-facing (normal y > 0.8) triangles within `--tops` of a box top and
   inside its footprint are flattened onto it.
9. **Clip**: centroids further than `--clip` outside the outline (`--clip` + `--clip-open` past an unseen wall), or
   further than `--clip` above the ceiling / below the floor plane go. `--clip 1 --clip-open 2` switches it off.
10. **Islands**: connected pieces smaller than `--island` m^2 go (`--protect 1` keeps furniture triangles).
11. **Thin floating pieces** (`--tubes`, 0 off): pieces of >= 10 triangles, < 1 m^2, at most half furniture, whose
    longest principal axis (vertex PCA, std) is >= `--tubes` times the second and the second <= `--tube-thick`.
12. **Smoothing**: `--smooth` Taubin iterations (lambda 0.5, mu -0.53) over the kept triangles.

Output: the kept scan triangles, then the panels (`refine.panel_triangles`), at most 200,000 triangles (more kept
triangles would be cut, `truncated`, with a warning). The refinement adds about 20 ms on the host for the owner's
64k-triangle scan; its buffers (welding hash, double positions, union-find, adjacency) stay below ~170 MiB for a
200k-triangle mesh and the fusion grid is freed before it runs, so peak memory is still the fusion phase's.

`--refine-only <room dir> [refinement options] [--json]` runs only steps 1-12 from `room-raw.txt` +
`room-mesh-raw.bin` (+ the head path from `scan.json`) and rewrites scan.json, room-mesh.bin and room.txt. A folder
from an older scanner without raw files: its room.txt + mesh become the raw files first (the room text is rewritten
with the mesh record pointing at `room-mesh-raw.bin`), a warning says so, and there is no head path. Its JSON:
`{"ok":true,"refine_only":true,"raw_triangles":..,"volumes":..,"floor_y":..,"ceiling_y":..,"heads":..,
"refine":{...},"warnings":[...],"timings_ms":{"refine":..,"total":..}}`.

Local parity (2026-10-18, private recording `room1.fprs`, not in the repository): the reference page's functions run
under node on this scanner's raw output give the same per-pass counts, the same label decisions and reasons, and
bit-identical refined meshes for the defaults, snap, off + protect, erase spheres + tubes 2, and a wide-band setting
(one triangle differs there: a facing test exactly at the threshold), and the same on an older 128k-triangle room
folder. Distance ties at outline corners decide which wall's clip allowance applies, so the corner distance uses
V8's Math.hypot algorithm.

Peak allocation stays below 260 MiB, including maximum frame buffers, the grid, model worklists and mesh buffers
(the latter phases do not retain frame buffers). Default synthetic scenes use a much smaller grid. Thresholds
are CLI options below or named constants next to their algorithms. No room-specific coordinates or dimensions are
used; on-device quality and speed still need validation.

## Tuning

Distances are metres, areas m², volumes m³. `--universe` accepts the full unsigned 64-bit range; 0 means unknown.

| Option | Default | Accepted range / meaning |
| --- | ---: | --- |
| `--voxel` | 0.05 | 0.04–0.20, spatial resolution; lower values cost more memory/work |
| `--ceiling-default` | 2.5 | 1.8–3.8, height above floor when ceiling is unseen |
| `--truncation` | 3 | 1–5, TSDF band in voxel units |
| `--max-edge` | 0.6 | 0.05–1.5, reject longer triangle edges |
| `--grazing` | 0.12 | 0.01–0.9, minimum absolute camera/normal cosine |
| `--range` | 4 | 1–10, maximum camera-relative vertex distance; >4 m opts back into inpainted depth |
| `--depth-jump` | 0.25 | 0.02–1, absolute part of a triangle's maximum depth discontinuity |
| `--depth-ratio` | 0.12 | 0–0.5, additional fraction of nearest vertex depth |
| `--min-views` | 5 | 1–16, temporal support target; brief scans cap it as described above |
| `--patch-residual` | 0.03 | 0.005–0.1, maximum RMS residual of a regular-fan normal patch |
| `--plane-distance` | 0.045 | 0.01–0.15, point-to-plane inlier tolerance |
| `--min-plane-area` | 0.45 | 0.1–4, minimum occupied plane area estimate |
| `--wall-height` | 1.5 | 0.5–3, minimum observed vertical wall span |
| `--snap-degrees` | 5 | 0–15, orthogonal snap tolerance |
| `--floor-band` | 0.35 | 0.05–1, floor search distance from STAGE y=0 |
| `--ceiling-min` | 1.8 | 1.5–3, minimum absolute ceiling y |
| `--min-volume` | 0.05 | 0.005–1, reject smaller furniture boxes |
| `--cluster-gap` | 0.10 | 0.04–0.20, voxel neighbourhood gap (rounded up to cells) |
| `--gap` | 0.65 | 0.1–1.5, maximum adjacent-wall endpoint extrapolation |
| `--horizontal` | 0.85 | 0.6–0.99, minimum floor/ceiling/top normal cosine |

Refinement (all distances metres; defaults = the owner's settings):

| Option | Default | Accepted range / meaning |
| --- | ---: | --- |
| `--refine` | 1 | 0/1; 0 = the unrefined output in room.txt/room-mesh.bin |
| `--walls` | panels | `off`, `snap` (project onto the planes) or `panels` (flat wall/floor/ceiling panels) |
| `--band` | 0.35 | 0.02–1, structural band behind a surface; plane re-estimation window |
| `--facing-deg` | 60 | 5–89, maximum angle between a triangle's normal and the surface's |
| `--band-in` | 0.12 | 0–1, band in front of a wall (room side) |
| `--floor-in` | 0.08 | 0–1, band above the floor |
| `--ceil-in` | 0.25 | 0–1, band below the ceiling |
| `--top-strip` | 0.5 | 0–2, top part of a wall where the full band applies in front too |
| `--near` | 0.03 | 0–0.3, panels: also clear non-furniture scan this close to a plane |
| `--clip` | 0.05 | 0–1, clip distance outside the outline / beyond floor and ceiling |
| `--clip-open` | 0.10 | 0–2, extra allowance past unseen walls (`--clip 1 --clip-open 2` = no clipping) |
| `--island` | 0.20 | 0–5 m^2, minimum connected piece area (0 off) |
| `--smooth` | 8 | 0–50 integer, Taubin iterations |
| `--tops` | 0.10 | 0–0.5, flatten upward scan this close to a box top (0 off) |
| `--protect` | 0 | 0/1, islands never remove furniture triangles |
| `--tubes` | 3 | 0–20, elongation (PCA axis ratio) of a thin floating piece (0 off) |
| `--tube-thick` | 0.15 | 0.01–1, maximum second PCA axis (std) of a thin floating piece |
| `--walked` | 0.40 | 0 or 0.05–1.5, walked-volume radius around the head path (0 off) |
| `--label-rules` | 1 | 0/1, label rules on the scanner's volumes |
| `--erase` | none | `"x,y,z,r;..."`, up to 64 spheres (r 0–5 m) whose scan is removed |

Additional constants in `model.c` include RANSAC trials, normal agreement, wall span, furniture margins,
label height/area thresholds, box trimming and circle recognition. Fusion sampling/weight limits are in
`fusion.c`; mesh splat radii/decimation limits in `mesh.c`; hard resource limits in `scan.h`.

`--hint outline.txt` accepts one `x z` point per line, decimal/scientific notation, optional `#` comments, either
winding, at most 96 points / 8 KiB. At least eight points with radial coefficient of variation below 5% count as
the default circle and are ignored. A non-circular hint is used only when walls are incomplete **and every
observed wall has a compatible hint edge** within `--gap`. Complete scans are never constrained to chaperone.
Incompatible inset play boundaries are ignored with a warning. Hint-completed edges retain synthesized status.

## Limits for integration

- Synthesized wall records now append `INVISIBLE_WALL_FACE` as their optional eighth field. Observed walls omit
  the label and default to `WALL_FACE`; comments are no longer used to carry semantics. This matches upstream's
  authorized contract change exactly; adapters need that same contract revision.
- A private RoomView recording was used for offline robustness work. It, its derived geometry, and its results
  stay outside committed files. Synthetic fixtures independently exercise the input characteristics. They are
  regressions, not proof
  of headset accuracy. Furniture labels are geometric heuristics; adjacent/touching objects can merge, occluded
  objects may split, top-only clusters have inferred depth, and thin fragments can be screen candidates. Nearby
  parallel walls and small wall jogs can merge under coarse depth uncertainty. Large disconnected coplanar wall segments are
  currently fitted together; complicated rooms can fall back to a warned rectangle.
- Cropped or sparse captures can produce an incomplete mesh and synthesized outline. Surfaces are not filled
  through unseen regions. Furniture is not an accessibility/safety boundary. Rooms beyond the grid cap are
  cropped with a warning. Default-circle recognition may also ignore an intentionally circular boundary.
- Output paths are fixed by the contract. The plumbing stream should stage a new directory and switch `current`
  after scanner success, preserving the previous room for rollback. Assign the recording's actual universe id;
  this standalone scanner cannot query SteamVR or validate universe alignment.

## Offline validation

`tests/test_roomscan.py` builds with the host compiler (skips only if none exists). Its tests cover the noisy
4 x 3 x 2.6 m room with table/couch, six-wall L, moving and rotated/translated mesh poses, ceiling fallback, a single
frame, missing walls, circular/matching hints, correct UUIDs/wire layout, interior mesh winding, triangle budget,
repeatable mesh bytes with fresh UUIDs, truncated/corrupt dumps, invalid counts/indices/poses and invalid CLI values.
Refinement tests: a floating pole and a column in the walked path (`synth_scan.py` `TUBE`/`GHOST`, `extra=`)
are meshed by the scanner and removed by the tubes / walked passes (each kept with its pass off, the table top kept);
panels wind into the room on the re-estimated planes; `--refine-only` reproduces a scan's files byte for byte;
`--refine 0` equals the raw output (and refining it later equals the default); scan.json settings/report/heads; head
path decimation to <= 2000 points; erase spheres; the label rules on a hand-written older room folder (merged,
nested, behind a wall, SCREEN from a flat panel, a door suggestion with wall and position, unknown records kept,
idempotent, `--label-rules 0`); invalid refinement options.
The test module takes about fourteen seconds including synthetic rendering and compilation (under the 60 s budget).
New cases cover non-finite frames before/after usable frames, per-vertex far rejection, depth jumps below the edge
limit, weak ceiling evidence, wall labels and a complete camera-fan scan. The deterministic stdlib renderer now
defaults to a 132 x 128 full index fan (99,822 indices), +Z forward / -Y image up, with poses mapping to STAGE.
It adds separable depth smoothing, spatially correlated noise, far inpainting, two initial NaN poses, 3 Hz timestamps
and head-height drift from 0.9 to 1.7 m. `--profile legacy` retains the smaller 110° x 76° test cone. New CLI controls
are `--profile`, `--invalid-start`, `--smoothing` and optional resolution overrides. Use `--invalid-start 0` for a
usable one-frame fixture; default RoomView warm-up frames otherwise have no valid tracking.

```sh
UV_OFFLINE=1 UV_CACHE_DIR=/tmp/frameport-uv-cache uv run pytest -q tests/test_roomscan.py
UV_OFFLINE=1 UV_CACHE_DIR=/tmp/frameport-uv-cache uv run ruff check scripts/mr/synth_scan.py tests/test_roomscan.py
```

The synthetic camera fan passes AddressSanitizer + UndefinedBehaviorSanitizer. Two consecutive final native builds
produce identical SHA-256 `2cc16b90abcd3d359f1658c581172bd650017bf2725e24268915f50904c000ec` (97,808 bytes, with refinement),
matching `artifacts/SHA256SUMS`. ELF inspection confirms aarch64, little-endian, no PT_INTERP / PT_DYNAMIC, and no
embedded local build paths.

The required full `uv run pytest -q` run completed with **1,788 passed, 40 skipped, 11 failed, 16 errors** in 79 s.
All 27 failures/errors are the sandbox's `PermissionError: [Errno 1] Operation not permitted` on socket creation
in existing HTTP/pairing/live-stream tests (`test_deeplink`, `test_devkit`, `test_install_links_extra`,
`test_livestream`, `test_setup_url`, `test_ui_state`). This environment forbids sockets, including loopback.
The unchanged baseline had the identical 27 failures/errors before the scanner tests were added (1,747 passed).
Consequently the whole-suite-pass requirement cannot be satisfied here; rerun it outside that restriction at
integration. No tests were changed/skipped to hide the restriction and no network/device access was attempted.
No patch/settings/catalog/triage files were touched, so the conditional patch documentation regeneration is
unnecessary.

## Ten-minute capture budget (synthetic benchmark)

The scanner is streaming and single-threaded. On an Intel Core i7-14700K host, a **synthetic-only** 1,800-frame
132 x 128 dump (1,083,751,208 bytes, equivalent to ten minutes at 3 Hz) took **7.41 s elapsed / 7.08 s CPU**, with
**27,208 KiB maximum RSS** at the default voxel size. The file repeats a 30-view synthetic sweep, including its
warm-up frames; it tests parsing/fusion throughput rather than 1,800 independent viewpoints. Geometry-dependent
model/mesh work and storage caches affect timing. No private recording or derived room data is in that benchmark.

Reproduce with bounded copies rather than loading a gigabyte into Python:

```sh
python scripts/mr/synth_scan.py /tmp/roomview.fprs
python - <<'PY'
import shutil
with open('/tmp/roomview.fprs', 'rb') as source, open('/tmp/roomscan-1800.fprs', 'wb') as out:
    out.write(source.read(8))
    for repeat in range(60):
        source.seek(8)
        shutil.copyfileobj(source, out, 1024 * 1024)
PY
/usr/bin/time -v /tmp/fp_roomscan --offline /tmp/roomscan-1800.fprs --out /tmp/roomscan-benchmark --json
```

For the Frame's Cortex-A720/X4-class cores, budgeting **3–5 times the host CPU time plus storage** gives roughly
**23–38 s** for this workload, leaving headroom under the requested ~60 s. This is an estimate, not an ARM
measurement or an 8-core speedup claim. Worst-case allocations are bounded below 260 MiB independently of frame
count (48-byte cells, 4.2-million-cell cap, bounded frame buffers/worklists), below both the original 300 MB and
follow-up 600 MB limits. A maximum-sized complex room can add model/mesh work beyond this small synthetic sweep.
The maintainer must time a ten-minute real capture on the Frame and check CPU time, wall time and RSS before
claiming the target is achieved on-device. Hardware square root instructions replace iterative division; neither
build links libm. The build and host command both require `-fno-math-errno` for that intrinsic.

## Exact device checks (maintainer only; none executed by this stream)

1. On the development PC, build, then render the canonical fixtures:

   ```sh
   python native/build.py --only roomscan
   python scripts/mr/synth_scan.py /tmp/box.fprs
   python scripts/mr/synth_scan.py /tmp/l.fprs --shape l
   python scripts/mr/synth_scan.py /tmp/down.fprs --no-ceiling
   python scripts/mr/synth_scan.py /tmp/one.fprs --frames 1 --invalid-start 0
   scp artifacts/linux-arm64-bin/fp_roomscan /tmp/box.fprs /tmp/l.fprs /tmp/down.fprs /tmp/one.fprs steamos@<FRAME>:~/Downloads/
   ```

2. In a terminal **on the Frame**, use an isolated directory; no game or current room is changed:

   ```sh
   mkdir -p "$HOME/.local/share/frameport/roomscan-test"
   cd "$HOME/.local/share/frameport/roomscan-test"
   cp "$HOME/Downloads/fp_roomscan" ./fp_roomscan
   chmod 700 ./fp_roomscan
   ./fp_roomscan --help
   ./fp_roomscan --offline "$HOME/Downloads/box.fprs" --out box --universe 0 --json > box.json
   cat box.json
   ./fp_roomscan --offline "$HOME/Downloads/l.fprs" --out l --json > l.json
   ./fp_roomscan --offline "$HOME/Downloads/down.fprs" --out down --ceiling-default 2.7 --json > down.json
   ./fp_roomscan --offline "$HOME/Downloads/one.fprs" --out one --json > one.json
   ```

   Expected JSON: box has `ok:true`, `walls:4`, `detected_walls:4`, `volumes:2`, floor within 5 cm of 0,
   ceiling within 5 cm of 2.6, coverage 100; the only expected box warnings concern skipped warm-up frames and
   out-of-range vertices. `box/room.txt` includes TABLE and COUCH volume records.
   L has six walls and the concave corner near (0,0), rather than a rectangle. Down includes exactly the warning
   string `ceiling not seen` (other warnings are expected), with ceiling minus floor 2.7. One includes
   `few frames; scan more viewpoints`. Every mesh should have at most 200,000 triangles. JSON is the scanner log;
   there are no expected stderr status lines.

3. Verify sidecars, wall labels and the corrupt-input exit status on the Frame:

   ```sh
   python3 - <<'PY'
   import json, pathlib, struct
   for name in ('box', 'l', 'down', 'one'):
       report = json.loads(pathlib.Path(name + '.json').read_text())
       data = pathlib.Path(name, 'room-mesh.bin').read_bytes()
       magic, version, nv, ni = struct.unpack_from('<4sIII', data)
       assert report['ok'] and magic == b'FPRM' and version == 1
       assert ni % 3 == 0 and 0 < ni // 3 <= 200000
       assert len(data) == 16 + nv * 12 + ni * 4
       for line in pathlib.Path(name, 'room.txt').read_text().splitlines():
           if line.startswith('wall '):
               fields = line.split()
               assert len(fields) in (8, 9)
               if len(fields) == 9:
                   assert fields[-1] == 'INVISIBLE_WALL_FACE'
       print(name, nv, ni // 3, report['warnings'])
   pathlib.Path('broken.fprs').write_bytes(b'FPRS\x01\0\0\0\0')
   PY
   ./fp_roomscan --offline broken.fprs --out broken --json
   echo "$?"
   test ! -e broken/room.txt
   test ! -e broken/room-mesh.bin
   ```

   Expected: `{"ok":false,"error":"truncated frame header"}`, exit status 2, no `broken` outputs.

4. Record actual RoomView dumps with the maintainer's recorder (FPRS v1, mesh-to-STAGE poses, stable universe).
   Copy the recording to `~/Downloads/capture.fprs`. Look around slowly for about a minute, including floor,
   ceiling, every wall, and furniture from several directions. With the **recorded universe id** assigned below:

   ```sh
   scan_universe=0  # replace 0 with the recorder's actual decimal universe id
   /usr/bin/time -v ./fp_roomscan --offline "$HOME/Downloads/capture.fprs" --out real --universe "$scan_universe" --json > real.json 2> real-timing.log
   cat real.json real-timing.log
   cat real/room.txt
   ```

   Measure wall offsets/yaws, ceiling and table top physically and compare to the records (target 5 cm / 2°).
   Check resident memory below 300 MB, sensible coverage/warnings, tabletop labels and no false wall/furniture
   blobs from smeared edges. Repeat with `--max-edge 0.35 --grazing 0.2` into `real-stricter` if edges smear;
   try `--voxel 0.08 --plane-distance 0.07` into `real-coarser` if depth is noisy. Keep both JSON logs and raw
   dumps **outside git** for threshold calibration. These are proposed tuning probes, not established device fixes.

5. Repeat the synthetic 1,800-frame benchmark from the preceding section on the Frame, using the same command
   and `/usr/bin/time -v`. Then test a ten-minute independent real capture. Target: about 60 s elapsed or less,
   less than 600 MB RSS, successful JSON and bounded mesh size. Record CPU time as well as wall time so storage
   overhead is visible. With difficult stereo edges compare `--min-views 8 --patch-residual 0.02` in a fresh output
   directory; short captures cap view support automatically. Include the JSON's exclusion counters in triage;
   skipped frames may occur anywhere in the capture. Keep all personal results outside git.

6. After merging plumbing/emulation, publish this staged room through that stream's room-install path and try
   Demeter, then an MRUK discovery/mesh game. Check room loading, furniture alignment, and warning/fallback
   behaviour in-headset; the scanner alone cannot verify the Meta API path. Do not replace `rooms/current` by hand
   during these standalone checks.

Undo standalone checks: remove only the isolated `~/.local/share/frameport/roomscan-test` directory and the five
test files copied to `~/Downloads` (plus `capture.fprs` if no longer needed). No Steam settings, games, agent,
room universe folder, or current-room symlink were changed by these steps.
