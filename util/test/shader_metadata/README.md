# D3D11 shader metadata diagnostics tests

The owned Windows fixture verifies name provenance without changing shader outputs or
installing a game-specific camera preset. It requires the Windows SDK, Visual Studio C++
tools, a D3D11 GPU and a matching RenderDuck runtime including Python 3.6 bindings.

```powershell
./util/test/shader_metadata/run.ps1 -Runtime ./artifacts/shader-metadata/bin -Output ./artifacts/shader-metadata/verification
```

Use a fresh output directory. The runner generates its own small RDCs and shaders there;
it never opens the user's current GUI window. Capture/replay run in separate processes,
and replay is repeated to verify persistence. Diagnostic mode is restored after the run.

Coverage:

- Normal cbuffer/variable names and a row-major float4x4 survive capture and reopen.
- Buffer narrow/wide/non-NUL-terminated names, rename, clear and generated fallback names.
- One physical Buffer, two different shader-local cbuffer declarations.
- Runtime reflection/debug stripping and a separately precompiled stripped shader.
- All six shader stages and geometry stream output; shared bytecode keeps separate resource IDs.
- Compile, Compile2, CompileFromFile, worker-thread compilation, nested-call suppression and
  RiderDuck-owned shader exclusion. Late loading of compiler 43 is tested when installed;
  its absence is explicitly reported as a skipped environment-dependent case.
- Disabled mode produces no diagnostic reports. Deterministic named/stripped shader bytes
  and final rendered pixels agree between enabled and disabled runs. Embedded PDB data is
  tested for per-capture retention, not byte equality across independent compilations.

`analyze.py` needs only standard Python and summarises native reports:

```powershell
python util/test/shader_metadata/analyze.py example.rdc.shader-metadata.json --replay-report example.rdc.shader-metadata.replay.json --output comparison.json
```

`result.json` records observer time, report sizes, process timings and peak working sets.
For overhead measurements, reuse the compiled fixture and alternate enabled/disabled
process runs. Startup/driver/compiler noise can exceed the diagnostic cost; report all
samples and distinguish total wall time from summed observer time. Observation time
includes lock wait, while summary text size excludes container/allocator overhead.

Add `-DefaultOn` when validating a dedicated build made with
`/p:RiderDuckDiagShaderMetadataDefault=1`. The enabled leg then removes the environment
variable entirely and verifies `build_default_enabled=true` and an empty
`environment_override` in the native report. The disabled leg still tests explicit `=0`.

The runtime switch is `RIDERDUCK_DIAG_SHADER_METADATA`: `1` enables, `0` disables, and
otherwise the build default applies. Ordinary builds default off. The native
capture report goes next to the newly produced RDC. Reopening with the switch enabled
writes a separate replay report, leaving the capture report and RDC unchanged.
