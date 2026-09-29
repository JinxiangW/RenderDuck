import hashlib
import json
import os
import sys

root = sys.argv[1]
def load(path):
    with open(path, encoding='utf-8') as f: return json.load(f)
def only(folder, suffix):
    paths = [os.path.join(folder, f) for f in os.listdir(folder) if f.endswith(suffix)]
    assert len(paths) == 1, paths
    return paths[0]
def md5(path):
    with open(path, 'rb') as f: return hashlib.md5(f.read()).hexdigest()
off, on = [os.path.join(root, m) for m in ('off', 'on')]
assert not any('shader-metadata' in f for f in os.listdir(off)), 'disabled diagnostics emitted a report'
a, b = load(os.path.join(off, 'replay.json')), load(os.path.join(on, 'replay.json'))
assert a['image_hashes'] == b['image_hashes'], 'diagnostics changed rendering'
for mode in (off, on):
    first, second = load(os.path.join(mode, 'replay.json')), load(os.path.join(mode, 'reopened.json'))
    assert first == second, 'metadata changed on reopen'
for blob in ('named.dxbc', 'stripped.dxbc'):
    assert md5(os.path.join(off, blob)) == md5(os.path.join(on, blob)), 'compiler output changed'
capture = load(only(on, '.shader-metadata.json'))
replay = load(only(on, '.shader-metadata.replay.json'))
assert capture['dropped_records'] == 0
events = [o['event'] for o in capture['observations']]
phases = {e['phase'] for e in events}
assert {'D3DCompile','D3DCompile2','D3DCompileFromFile','D3DStripShader','create_input','serialise_input'} <= phases, phases
named, stripped, offline = md5(os.path.join(on,'named.dxbc')), md5(os.path.join(on,'stripped.dxbc')), md5(os.path.join(root,'offline.dxbc'))
assert named != stripped != offline
assert capture['blobs'][named]['has_rdef']
assert not capture['blobs'][stripped]['has_rdef']
assert not capture['blobs'][offline]['has_rdef']
assert any(e['phase']=='D3DStripShader' and e['input_md5']==named and e['output_md5']==stripped for e in events)
assert {e['phase'] for e in events if e['input_md5']==offline} == {'create_input','serialise_input'}
created = {(e['resource'],e['input_md5']) for e in events if e['phase']=='create_input'}
serialised = {(e['resource'],e['input_md5']) for e in events if e['phase']=='serialise_input'}
assert created == serialised, 'API input differs from resource serialisation input'
observed_outputs = {e['input_md5'] for e in events if e['phase'].startswith('D3DCompile')}
observed_outputs.update(e['output_md5'] for e in events if e['phase'] == 'D3DStripShader')
assert all(digest in observed_outputs or digest == offline for _, digest in created), 'compiler/strip output differs from CreateShader input'
for row in replay['observations']:
    e = row['event']
    assert e['phase'] == 'deserialize'
    assert (e['resource'],e['input_md5']) in serialised, e
deserialised_hashes = {row['event']['input_md5'] for row in replay['observations']}
assert all(s['bytecode_md5'] in deserialised_hashes for s in replay['replay_reflection']), 'reflection parser changed bytecode without reported enrichment'
threads={e['thread'] for e in events if e['phase']=='D3DCompile'}
assert len(threads)>=5, threads
metrics = {mode:load(os.path.join(root,mode,'capture.log')) for mode in ('off','on')}
late = metrics['on']['late_compiler_present']
assert sum(o['count'] for o in capture['observations'] if o['event']['phase']=='D3DCompile') == 18 + int(late), 'unexpected nested/internal compilation attribution'
if late:
    assert not metrics['on']['compiler43_preloaded'], 'fixture did not exercise late DLL loading'
    assert any(e['phase']=='D3DCompile' and e['compiler_version']==43 for e in events)
assert len(created) == 10 + int(late), 'tool-owned shader creation leaked into application diagnostics'
assert len({digest for resource, digest in created}) < len(created), 'fixture did not exercise shared bytecode with separate resource IDs'
assert any(blob['has_debug'] for blob in capture['blobs'].values()), 'fixture did not exercise debug data retention'
assert len(replay['observations']) == len(created), 'fixture did not include every shader stage in the RDC'
assert any(b['name_source']=='shader_reflection' for s in replay['replay_reflection'] for b in s['cbuffers'])
assert any(b['name_source']=='instruction_fallback' for s in replay['replay_reflection'] for b in s['cbuffers'])
result = dict(passed=True, checks=['names and matrix layouts survive reopen','runtime strip trace',
    'prestripped shader has no observed reflection origin','shared Buffer retains distinct shader cbuffer names',
    'on/off bytecode and pixels identical','all compiler entry points','multithread dedup and no nested/internal compiles'],
    capture_report_bytes=os.path.getsize(only(on,'.shader-metadata.json')),
    replay_report_bytes=os.path.getsize(only(on,'.shader-metadata.replay.json')),
    diagnostic_observation_ms=capture['observation_ms'],summary_bytes=capture['summary_bytes'],
    process_measurements=metrics, late_compiler_test='passed' if late else 'skipped: d3dcompiler_43.dll unavailable')
with open(os.path.join(root,'result.json'),'w') as f:json.dump(result,f,indent=2)
print(json.dumps(result,indent=2))
