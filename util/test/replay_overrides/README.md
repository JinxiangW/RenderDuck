# D3D11 replay override regression tests

These tests capture an owned D3D11 fixture and compare replayed images with an independently rendered camera reference. They require Windows, a D3D11 GPU, Visual Studio C++ tools and a matching build of `riderduck.dll` / `pymodules/riderduck.pyd`. The launcher uses the repository's bundled Python 3.6 runtime.

From the repository root:

```powershell
./util/test/replay_overrides/run.ps1 -Runtime ./artifacts/freecamera-performance/bin
```

To also test the real Qt interface, provide the Qt SDK used to build `qriderduck.exe`:

```powershell
./util/test/replay_overrides/run.ps1 -Runtime ./artifacts/freecamera-performance/bin -Gui -QtPrefix D:/Qt/5.9.4/msvc2015_64
```

The runner writes build/capture logs, replay results and screenshots under `artifacts/replay-overrides-tests` (override with `-Output`). The GUI test opens and closes its own application instance. It requires native Windows Qt output; the offscreen Qt plugin cannot provide the D3D11 swapchain used by Texture Viewer.

For the focused mouse regression, use `-MouseOnly -QtPrefix PATH`. It sends Qt
events to `CustomPaintWidgetInternal`, the actual mouse receiver, instead of only
the outer `render` widget. It checks right-drag rotation without pixel picking,
release/lost-release/focus cancellation, capture reload and render-child
replacement, and restoration of normal picking when the camera is disabled.
It also covers world-up yaw/pitch from an initially pitched or rolled camera,
full yaw turns, absolute pitch limits, both handedness conventions, +/-Z forward,
vertical initial views and preservation of camera position. It also tests screen-space
left/right under mirrored projections, both forward/handedness conventions and homogeneous
sign; all six world-up axes, Q/E and W directions, yaw/pitch limits, sidecar restore disabled
and the legacy +Y default.
It does not inject OS mouse input or operate another application window.

## Automatic configuration and application

The discovery algorithm has CPU-only tests:

```powershell
python util/test/replay_overrides/test_camera_scan.py
python util/test/replay_overrides/test_camera_recipe.py
```

For an owned OW recipe workflow, run:

```powershell
./util/test/replay_overrides/run_auto.ps1 -Runtime ./artifacts/freecamera-neutral/bin -Capture C:/Caps/ow/range.rdc -Output ./artifacts/camera-recipe-test -Recipe ./docs/freecamera-profiles/ow/overwatch-range-v2.json
```

Use a fresh output directory. The runner copies the RDC and changes only the owned copy's
sidecar. It covers the neutral editor, no-recipe guidance, fingerprints, cancellation,
partial confirmation, camera/Pass application, reset, capture switching and v1/v2/v3 sidecar
restoration with overrides disabled. `-Config PATH` runs a shorter Apply-only check.
There is no implicit game layout. `test_camera_scan.py` retains CPU regression coverage for
the old explicit layout scanner; that scanner is no longer bundled or invoked by the UI.

`scan_recipe.py RUNTIME CAPTURE RECIPE OUTPUT` expands a recipe headlessly.
`compare_recipe.py RUNTIME CAPTURE CONFIG RECIPE OUTPUT` records the OW original,
camera-only, camera+bypass, and restored images plus relevant colour-chain snapshots.
The native GUI fixture also compares a nonidentity ModelView's colour/depth with the
independently rendered camera reference. `verify.py` covers combined-rule validation and
runtime-failure rollback in addition to separate camera/pass operations.

## Performance comparison

`benchmark_camera.py` accepts `RUNTIME CAPTURE CONFIG OUTPUT [COUNT=30]` and runs with the
matching runtime's `pyhost.exe`/Python 3.6. Point `PYTHONHOME` and the front of `PATH` at
that runtime, use owned capture copies, and run GPU measurements sequentially. The script
warms up, records CPU submission and GPU-completed timings, and stores colour/depth hashes.
It reports rather than hides non-deterministic captures (`reset_exact` can be false).

`RIDERDUCK_BENCHMARK_LEGACY_CAMERA=1` restores linear rule traversal and whole-buffer copies
for diagnostic counters. Leave it unset for normal use. The benchmark-only variables
`RIDERDUCK_BENCHMARK_FIXED_CAMERA=1` and `RIDERDUCK_BENCHMARK_DUMP=1` produce fixed-pose
repeat controls and raw pixel dumps. These dumps can be large and may be deleted after
recording the comparison results.

Commit the test sources and runners in this directory. Generated RDCs, sidecars, binaries,
screenshots and logs belong under the ignored `artifacts/` directory.

## Coverage

- Colour and depth comparison with a directly rendered translated camera.
- Mixed camera/material constants, repeated updates, dynamic maps, indirect and instanced draws, deferred contexts and D3D11.1 CB subranges.
- Vertex, pixel and compute constant overrides, and exact restoration of baseline images.
- Binding windows extending past allocation (out-of-bounds zero reads), overlapping-write order, compact-copy counters, index reuse and target-event validation.
- PS/CS pass bypass, consecutive rules, multiple mappings, same-resource snapshots, bilinear scaling, Float to UNorm conversion, nonzero mips and array slices.
- Downstream tone mapping and UI, split replay and event navigation, overlap/subresource/MSAA validation.
- Skipped clears, copies, mip generation, multisample resolve and discard.
- GUI reflection selection, matrix reading, keyboard/vertical/mouse movement, focus loss, rotation, reset, pass configuration, event labels and capture closure.

The fixture does not test game-specific matrix conventions or promise a particular frame rate. It deliberately keeps the capture and all resources local.
