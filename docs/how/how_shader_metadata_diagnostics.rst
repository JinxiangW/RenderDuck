How do I diagnose missing constant-buffer names?
===============================================

The optional local D3D11 metadata diagnostic distinguishes resource debug names from
shader-local cbuffer and variable names. It does not recover unavailable names or change
shader compilation, stripping, bytecode, render commands or the RDC format.

Enable before launching the application
--------------------------------------

Normal builds default to disabled. A dedicated diagnostic build can be produced with the
MSBuild property ``RiderDuckDiagShaderMetadataDefault=1``; it defaults to enabled even when
injected with Global Hook into a process with no diagnostic environment variable.
``RIDERDUCK_DIAG_SHADER_METADATA=1`` explicitly enables either build and ``=0`` explicitly
disables either build. Otherwise the build default applies. The setting is latched per
process. The Capture Dialog's environment-variable settings can be used. Attaching after
shader compilation cannot observe earlier compiler calls.

When using an environment override with Global Hook, the game inherits its launcher's environment, not the RenderDuck GUI's.
Set the variable in the environment that starts the game/launcher. An already running
launcher must be restarted from that environment for its new child processes to inherit it.
This diagnostic does not modify system-wide environment variables.

After a successful capture, ``example.rdc.shader-metadata.json`` is written beside the RDC.
Launch the replay frontend with the same variable set to produce
``example.rdc.shader-metadata.replay.json`` when reopening it. Replay reporting does not
modify the RDC or the original capture report. Failure to write a report is logged and
does not invalidate the capture.

What is observed
----------------

* Successful outermost ``D3DCompile``, ``D3DCompile2`` and ``D3DCompileFromFile`` calls.
* Successful ``D3DStripShader`` calls, including input/output hashes and strip flags.
* Successful D3D11 shader creation inputs and inputs to resource-record serialisation.
* Bytecode actually deserialised from the RDC, and the resulting replay reflection.
* Narrow/wide Buffer debug-name calls, including rename/clear observations, and final
  replay resource names. A resource name is never substituted for a shader's cbuffer name.

Compiler DLL names ``d3dcompiler_33.dll`` through ``d3dcompiler_47.dll`` are registered using
the existing DLL hook mechanism. Each version has its own forwarding pointers; dynamically
loaded libraries are supported. DLL versions/exports absent from the process need not be
present. Renamed/custom compilers, statically linked compiler code and DXC are outside this
diagnostic's observation coverage. RiderDuck's own shader-cache compilation/creation is
suppressed, and nested compiler implementation calls are attributed to the outer call.

The reports contain complete-bytecode MD5 fingerprints, DXBC chunk presence, reflected CB
names, variable counts/layout summaries, thread IDs and call counts. MD5 is a diagnostic
identity check, not a security guarantee. Distinct complete bytecode blobs are summarised
once; resource associations remain separate. Source text and bytecode payloads are not
dumped. Storage limits are reported through ``dropped_records``; nonzero means the report
is incomplete. ``summary_bytes`` counts retained blob-summary text, not total process RAM.

Interpreting the results
------------------------

Use ``util/test/shader_metadata/analyze.py CAPTURE_REPORT --replay-report REPLAY_REPORT``
to print a compact comparison; ``--output PATH`` also saves it as JSON.

* ``reflection_available_at_create``: original reflection was present at shader creation.
* ``reflection_removed_by_observed_runtime_strip``: a recorded strip call removed reflection
  before that exact bytecode reached shader creation.
* ``no_reflection_observed_at_create_origin_unknown``: creation received no reflection and
  this process did not observe its removal. This does **not** prove offline compilation.

``serialise_input`` means a resource record was prepared, not that the resource necessarily
appears in this frame. Only ``deserialize`` after reopening confirms RDC inclusion. A created
shader absent from replay may simply be unreferenced. Bytecode and names should be compared
at matching stages, not inferred from total record counts.

Replay cbuffer ``name_source`` identifies ``shader_reflection`` or ``instruction_fallback``.
When debug information is available without RDEF, it reports
``debug_information_or_instruction_fallback`` conservatively. Separate debug files can affect
replay reflection; this diagnostic does not embed them or guarantee their availability on
another machine. Buffer names separately identify application names and generated fallbacks.

Debug PDB chunks may contain compilation-specific identifiers. Independently compiled debug
blobs can therefore differ even when RDEF, executable instructions and rendered pixels agree.
Within one capture, the exact API output, creation input and deserialised bytes can still be
compared without normalisation.

Validation
----------

See ``util/test/shader_metadata/README.md`` for the owned fixture and report comparison.
An existing stripped RDC cannot reveal compiler calls that happened before it was captured.
To decide whether a real game has earlier names worth retaining, collect a new diagnostic
capture from process startup. This feature reports evidence; it does not yet save stripped
reflection back into the RDC or enable automatic camera recognition for unknown games.
