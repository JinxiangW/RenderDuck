How do I move the camera or bypass a pass in a capture?
=====================================================

The local D3D11 replay supports camera constants and pass overrides. Changes affect replay and the displayed textures; they are not written to the RDC file. Camera configurations are saved in a JSON file beside the capture. Pass rules supplied by a version-2 recipe are saved with its camera configuration; standalone manual pass edits remain session-only.

Free camera
-----------

Automatic configuration
~~~~~~~~~~~~~~~~~~~~~~~

Open :guilabel:`Free camera...` in Texture Viewer and import an agent-authored JSON
through :guilabel:`Recipe...`. :guilabel:`Auto configure` validates that recipe and scans
actual shader reads and bindings. Without a recipe it displays an instruction to import
one or ask an agent to analyse the capture; no game-specific scanner runs automatically.
Once a recipe is supplied, the bundled Python runs without MCP, an extension, PySide2 or NumPy.

A fully supported result is applied automatically. When history, auxiliary constants or
unknown camera layouts are encountered, the report explains the incomplete coverage;
choose :guilabel:`Apply confirmed configuration` to accept the known subset. Unaccepted results,
cancelled scans and failed scans leave the existing configuration intact. Other cameras
are excluded. Resource IDs, shader IDs, slots and event ranges are discovered afresh in
each capture; a new scene does not reuse the previous capture's IDs.

Both :guilabel:`Apply confirmed configuration` and :guilabel:`Apply and enable camera` show
application progress: preparing camera data, replaying the scene, and refreshing the image.
Replay uses an indeterminate progress bar because it has no reliable percentage estimate.
Duplicate applications are disabled. Success closes the configuration window and focuses
the texture; failure keeps the window open with the error. Sidecar saving continues in the
background, and a failure also appears on the camera toolbar.

The final Present output is selected when it is unambiguous. Otherwise the current display
is retained. Only passes explicitly listed in the recipe are skipped or bypassed; the script does not infer them. Motion vectors, temporal
history and engine-specific auxiliary fields can still cause artifacts after moving.

Manual configuration
~~~~~~~~~~~~~~~~~~~~

1. Select a representative scene draw, then choose :guilabel:`Free camera...` in Texture Viewer.
2. Add fields with :guilabel:`From reflection...`, or enter shader ID, stage, CB slot and byte offset manually. Offsets are relative to the bound constant-buffer range. Assign each field its meaning: View, Projection, ViewProjection, an inverse matrix, or CameraPosition.
3. Set the inclusive event range for each field. Include the relevant depth, scene and lighting passes; avoid shadow or reflection cameras unless they also need the change.
4. Specify storage (row-major/column-major) and multiplication convention (``M * v``/``v * M``) separately. Reflection supplies storage, but does not establish the multiplication convention.
5. Select an event with the mapped View/Projection bindings and click :guilabel:`Read View / Projection`. Without reflection, paste the matrices into the two text boxes. Those boxes use mathematical rows with column vectors, regardless of the shader layout.
6. Choose :guilabel:`Apply and enable camera`. On success, the window closes and the texture receives focus.

Agent-authored camera recipes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Open :guilabel:`Recipe...` in the Free camera window. Paste or import a JSON
recipe, then choose :guilabel:`Use and run`. The main window also exposes :guilabel:`Auto configure`. :guilabel:`Use recipe` stores the layout without scanning.
This uses the bundled ``freecamera_recipe.py`` with the embedded Python interpreter;
MCP is optional once the layout has been supplied.

The agent identifies the CB slots, byte offsets, field meanings and matrix conventions
from shader instructions and buffer values. The script reads the matrices from the capture,
checks their relationships, scans actual shader reads/bindings, and generates shader IDs
and event ranges. It never executes code from the JSON. Names and reflection types are
helpful but not required if the shader's bytecode still exposes CB declarations and reads.

A minimal example is::

  {
    "version": 1,
    "name": "Verified camera layout",
    "row_major": true,
    "row_vector": true,
    "forward": "+Z",
    "camera": {
      "view": {"slot": 9, "offset": 64},
      "inverse_view": {"slot": 9, "offset": 128},
      "projection": {"slot": 9, "offset": 320}
    },
    "layouts": [{
      "slot": 9,
      "stages": ["VS", "PS", "CS"],
      "fields": [{"semantic": "View", "offset": 64},
                 {"semantic": "InverseView", "offset": 128}]
    }],
    "unresolved": ["Other camera-dependent fields have not been classified."]
  }

Offsets are bytes relative to the bound CB window. ``row_major`` describes storage;
``row_vector`` means the shader uses ``v * M``. Both are mandatory recipe defaults.
Camera locators and individual fields can override these conventions. A camera locator
can specify ``stage`` when View and Projection come from different stages.
``inverse_view`` is optional. Fields support the meanings listed in the manual table;
``projection_offset`` optionally locates an independent projection in that field's CB.
A layout must contain a view-dependent field: matching Projection alone is insufficient
to establish camera ownership.

For combined per-object matrices, add a field such as
``{"semantic":"ModelView","offset":0,"model_offset":192}``.
The script verifies ``ModelView = View * Model`` using mathematical column-vector matrices,
then retains the object's transform in each generated field. A changed object transform
splits the event range. ``model_row_major`` and ``model_row_vector`` override the Model
storage convention if it differs from ModelView. This does not support arbitrary packed,
relative or combined MVP layouts; they need further explicit adaptation.

The current selection need not be a draw. Without ``reference_event``, the script searches
for a unique camera. If several views match, it reports candidate events and requires a
capture-specific ``reference_event``; it does not guess which is the main view. Portable
export removes this hint. Every new capture is scanned again, so no shader/resource IDs or
expanded event intervals are reused. Slots and offsets remain part of the verified layout;
shader permutations that change them need another layout or recipe revision.

Nonempty ``unresolved`` or unsupported dynamic indexing produces a partial result. Review
the report and choose :guilabel:`Apply confirmed configuration`. Successful matching proves only
the declared fields, not every camera-dependent operation in the game. Unknown history and
auxiliary fields remain untouched. The examples under ``docs/freecamera-profiles`` are deliberately partial for this reason.
Manual edits to the generated table do not rewrite the recipe.

Version 2 adds an explicit ``passes`` array. Each entry has ``first_event``, ``last_event``,
``mode`` (``skip`` or ``colour_bypass``), and ``reason``. Bypass entries additionally have
``colours`` mappings with ``source`` / ``destination`` ResourceId strings and optional
``source_mip``, ``source_slice``, ``source_format``, ``destination_mip``,
``destination_slice``, ``destination_format`` (zero defaults). Existing D3D11 format,
subresource and overlap restrictions apply. A v2 empty array clears the pass list.

Recipes with ``reference_event`` or any Pass rules require ``capture.size`` and
``capture.sha256`` (the entire RDC). The worker verifies this fingerprint before discovery.
A mismatch is rejected. Version-1 camera-only recipes remain readable. An old standalone recipe with an event
hint must add the fingerprint or remove that hint before running. Old authenticated
sidecars supply their existing capture fingerprint to restored recipes automatically. Pass endpoints must
identify actual actions, not synthetic grouping markers.

:guilabel:`Export full recipe...` retains capture references and Pass rules.
:guilabel:`Export camera layout...` removes ``reference_event``, ``capture`` and all Pass
rules; another capture needs fresh analysis before reusing concrete IDs. The version-2
camera and Pass list are applied together. Disabling or resetting an applied v2 recipe
clears both rule sets; legacy camera-only configurations keep the separate manual-pass behavior.

Double-click a Value, Stage, Storage or Multiply cell to choose its value. The editor is
created only for that cell, keeping large configurations responsive. Successful Apply now
closes the window and focuses the texture automatically.

WASD moves, Q moves down, E moves up, arrow keys or a right-button drag rotates, and Shift increases speed. Use the speed control. The Camera tab provides World up (+/-X, +/-Y, +/-Z) and Camera forward (-Z/+Z); apply to use changed conventions. Movement stops on focus loss. :guilabel:`Reset view` removes camera overrides and replays the original view. The toolbar reports when replay is still updating.

Rotation uses the selected world up for yaw and the horizontal camera right axis for
pitch, with no roll. Q/E moves along that world up; W/S follows camera-local forward. Enabling the camera removes existing roll while keeping its
position and viewing direction. Absolute pitch is limited to +/-89 degrees;
captures looking exactly vertically are moved just inside that limit. Mirrored
camera coordinate conventions are preserved. Horizontal translation, arrow yaw and mouse
yaw follow projected screen-right, including projections with a negative horizontal scale.
Handedness is inferred from View, and projection mirroring from Projection; selecting world
up does not transpose matrices or convert the engine's world coordinates.

Each matching draw/dispatch receives a temporary constant buffer. Non-camera bytes retain their values at that event, including material, object and bone data. The original binding is restored afterwards. Projection and its original jitter are retained unless explicitly mapped to a different base projection.

The optional :guilabel:`Projection` cell on each field contains 16 mathematical row-ordered
values. An empty cell uses Base Projection. Automatic configuration preserves the jittered
and non-jittered projection variants separately.

Saving and reopening
~~~~~~~~~~~~~~~~~~~~

A successful camera application atomically saves ``<capture>.rdc.freecamera.json`` beside
the RDC. The file contains schema version 3 (versions 1 and 2 remain readable), the capture's
complete SHA-256 and byte length, baseline matrices, field mappings, per-object Model
matrices, the recipe and its confirmed Pass rules, speed, forward convention and display target. It does
not contain the moved pose or enabled state. Movement never rewrites this file.

On reopening, RenderDuck verifies the complete fingerprint, configuration version and
field/resource validity in the background. A valid configuration fills the Camera tab;
the original display is retained, with camera and Pass rules disabled, until the user enables the configuration. Enabling starts from
the configured baseline. Corrupt files, mismatched captures and unsupported versions are
reported and not applied. Saving failures are reported without disabling the current camera.

An edited/imported recipe is persisted in this sidecar only after successful application.
Use the recipe editor's export button to save an unexecuted portable recipe separately.

Move the JSON together with its RDC. If renaming the RDC, rename the JSON to match.
A new capture from the same game needs another scan. Captures configured before this
feature need one successful application in the new build to create their sidecar.

Pass skipping and colour bypass
-------------------------------

Right-click a marker group or event in Event Browser and choose :guilabel:`Skip / bypass pass...`. The event range can also be entered manually in the :guilabel:`Passes` tab.

* :guilabel:`Normal` leaves the range enabled.
* :guilabel:`Skip` suppresses draws, dispatches, clears, resource copies, resolves, mip generation and discards. Parsing, binding, uploads and query structure continue.
* :guilabel:`Colour bypass` also snapshots a selected input at range entry and writes it to a selected output at range exit.

For TAA, select the scene colour before TAA as input and the texture normally produced by TAA as output. Later tone mapping and UI can then read that output. Set the mip, slice and view formats as needed. Multiple rows with the same range and mode define multiple colour mappings. Overlapping skip ranges are allowed; ranges involving a colour bypass must not overlap another rule.

View formats are numeric D3D11 ``DXGI_FORMAT`` values. Zero uses the resource format. Common linear views are 2 (R32G32B32A32_FLOAT), 10 (R16G16B16A16_FLOAT), 28 (R8G8B8A8_UNORM), and 87 (B8G8R8A8_UNORM). Typeless resources need an explicit compatible view. The first version supports single-sample 2D Float/UNorm colour views, including selected array slices and mips. Depth, compressed, integer and sRGB views are rejected. No exposure or gamma transform is added.

Matching dimensions and formats use a copy; otherwise a colour blit uses bilinear sampling. This provides a basic spatial resize when bypassing a temporal upscaler. A snapshot permits the input and output to be the same resource.

Click :guilabel:`Apply passes` to replay with the new rules. Event Browser marks affected rows with ``[Skipped]`` or ``[Bypass]``. :guilabel:`Restore all passes` removes all pass rules. Closing the capture clears active overrides; saved camera configuration can be restored on reopening.

Scripting
---------

The TextureViewer UI interface additionally exposes ``GetReplayCameraState``
(JSON), ``ConfigureReplayCamera`` (JSON configuration) and ``ResetReplayCamera``.
These are UI-thread methods used by RenderDoc MCP to populate the same visible
form as manual configuration. State includes replay completion and error status;
wait until ``busy`` and ``pending_update`` are false and check ``last_error``.
``flip_y`` changes the displayed texture orientation only.
``world_up`` optionally selects ``+X``, ``-X``, ``+Y``, ``-Y``, ``+Z`` or ``-Z`` in full
configurations and v1/v2 recipes. It defaults to ``+Y`` for legacy configurations and is
saved in the sidecar. ``forward`` remains the camera-local ``-Z`` / ``+Z`` convention.
State exposes ``world_up_axes`` so clients can detect this capability. These settings control
navigation and the no-roll constraint; camera field meanings still need independent verification.

Each field optionally accepts ``projection`` (16 mathematical row-ordered values);
omitting it retains the existing global-projection behavior. ``output_rid`` chooses a
display texture and optional ``event_id`` chooses its event when enabling. State also
includes ``scanning``, ``scan_report``, ``saving_configuration`` and
``configuration_status``, plus ``applying``, ``apply_phase``, ``last_update_ms`` and
``apply_timings_ms`` (preparation, replay-worker and UI publication measurements).
``pending_update`` stays true throughout an application, including its preparation and
display refresh. Applying through this API uses the same sidecar save path as
the manual controls. ``enabled: false`` loads fields without enabling; native sidecar
restore also preserves the displayed image.

For camera-relative rendering, ``RelativeViewProjection`` computes projection
times the view rotation with its translation removed. Pair it with
``CameraPosition`` when the shader subtracts the camera position from world
positions. ``InverseRelativeViewProjection`` supplies the inverse. These fields
still require a verified buffer layout and projection convention.

``ModelView`` accepts a required ``model`` array of 16 mathematical row-ordered values
and writes ``newView * model`` before the usual shader convention conversion.

``ConfigureReplayCamera({"recipe": recipe, "run_recipe": true, "show": true})`` stores
and runs a recipe; use ``run_recipe: false`` to populate the window only. A separate
``{"apply_recipe_result": true}`` accepts a completed partial result. State exposes
``recipe_supported``, ``recipe``, ``recipe_result_ready``, ``scan_partial`` and
``scan_field_count``. Poll ``scanning`` through completion and inspect ``last_error`` and
``scan_report`` before accepting. RenderDoc MCP exposes these routes as
``configure_replay_camera_recipe`` and ``apply_replay_camera_recipe``. Always target the
intended ``window_id`` and supply ``expected_capture`` when automating a live window.

The replay controller exposes ``SetReplayConstantOverrides``, ``ClearReplayConstantOverrides``, ``SetReplayPassOverrides`` and ``ClearReplayPassOverrides``. They return an empty string on success or an error message. Invalid configurations leave previously applied rules intact; a runtime constant-binding mismatch is reported after replay. Other graphics APIs and remote replay return an unsupported error.

``SetReplayOverrides(constants, passes, targetEventId=0)`` validates and installs both rule
sets before one forced replay. Validation failure keeps the previous rules. A runtime
binding failure restores both previous rule sets and the previous event before returning
an error. Ordinary camera movement continues to update constants only. Discovery temporarily
removes both rule sets and restores them after each batch, including cancellation/failure.

``SetReplayConstantOverrides(overrides, targetEventId=0)`` optionally installs constants and
replays a target event in one operation. Zero retains the current event, preserving existing
callers. ``GetReplayOverrideStats()`` returns JSON counters for rule comparisons, Buffer copy
bytes/calls, temporary allocations and index rebuilds since the last constant application.

The D3D11 path indexes rules by shader, stage and slot, retaining original order for
overlapping writes. It copies the current shader-visible range to a compact buffer while
preserving out-of-bounds zero reads. Contents are recopied at every matching event, including
after uploads and deferred commands; only allocations and rule indices are reused.
Identical projections share matrix calculations. These optimisations keep full-resolution
rendering and all configured passes. A complete scene replay is still needed for each pose.

For development comparisons only, ``RIDERDUCK_BENCHMARK_LEGACY_CAMERA=1`` selects the previous
linear rule scan and whole-buffer copying. Normal operation leaves this variable unset.

Use ``ReplayConstantOverride`` for shader/stage/slot/event-range byte patches, and ``ReplayPassOverride`` with ``ReplayColourMapping`` entries for pass bypass. Calls belong on the replay thread, as with other replay-controller methods.

Limits
------

The capture only contains submitted GPU work. Moving the camera cannot recover CPU-culled objects, regenerate the engine's transparent sorting or LOD choices, or infer special camera-relative layouts. Mapping only a combined per-object MVP requires additional adaptation.

Colour bypass supplies only the selected outputs. It does not regenerate temporal history, motion vectors, auxiliary textures, exposure conventions or engine-specific encodings. Camera-dependent shadows and screen-space effects may still need adaptation. Replay speed depends on the capture and GPU; input updates are coalesced rather than queued indefinitely.
