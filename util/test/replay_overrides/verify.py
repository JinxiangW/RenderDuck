"""Run with the built application's Python: pyhost.exe verify.py RUNTIME CAPTURE."""
import os
import sys
import struct
import array
import json

runtime = os.path.abspath(sys.argv[1])
sys.path.insert(0, os.path.join(runtime, 'pymodules'))
import riderduck as rd

rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
assert cap.OpenFile(sys.argv[2], '', None) == rd.ResultCode.Succeeded
status, replay = cap.OpenCapture(rd.ReplayOptions(), None)
assert status == rd.ResultCode.Succeeded, str(status)
results = []

def flatten(actions):
    for a in actions:
        yield a
        yield from flatten(a.children)

actions = list(flatten(replay.GetRootActions()))
markers = {a.customName: a for a in actions if a.flags & rd.ActionFlags.PushMarker}
resource_list = list(replay.GetResources())
resources = {r.name: rd.ResourceId(r.resourceId) for r in resource_list}

def bounds(name):
    a = markers[name]
    nodes = list(flatten([a]))
    return a.eventId, max([n.eventId for n in nodes] + [e.eventId for n in nodes for e in n.events])

def at(name):
    replay.SetFrameEvent(bounds(name)[1], True)

def pixels(name, mip=0, slice=0):
    sub = rd.Subresource(); sub.mip = mip; sub.slice = slice
    raw = replay.GetTextureData(resources[name], sub)
    out = array.array('f'); out.frombytes(bytes(raw)); return out

def close(a, b, tolerance=1e-6):
    assert len(a) == len(b), (len(a), len(b))
    error = max(abs(x-y) for x,y in zip(a,b))
    assert error <= tolerance, 'maximum pixel difference {}'.format(error)

def passed(label):
    results.append(label); print('PASS: ' + label, flush=True)

def set_passes(passes):
    error = replay.SetReplayPassOverrides(passes)
    assert not error, error

def rule(name, mappings=None):
    p = rd.ReplayPassOverride(); p.firstEvent, p.lastEvent = bounds(name)
    p.bypass = mappings is not None
    if mappings:
        colours = []
        for src, dst in mappings:
            c = rd.ReplayColourMapping(); c.source = resources[src]; c.destination = resources[dst]
            colours.append(c)
        p.colours = colours
    return p

try:
    at('Scene'); baseline = pixels('Scene')
    # Ensure the fixture actually rasterises geometry instead of only comparing clear colours.
    assert max(baseline[0::4]) > 0.5 and max(baseline[1::4]) > 0.5 and max(baseline[2::4]) > 0.5
    at('Reference'); reference = pixels('Reference'); reference_depth = pixels('ReferenceDepth')
    assert baseline != reference
    scene_draws = [a for a in flatten(markers['Scene'].children) if a.flags & rd.ActionFlags.Drawcall]
    replay.SetFrameEvent(scene_draws[0].eventId, True)
    shader = replay.GetPipelineState().GetShader(rd.ShaderStage.Vertex)
    view = [1.0 if i % 5 == 0 else 0.0 for i in range(16)]; view[3] = 0.35
    patch = rd.ReplayConstantOverride(); patch.firstEvent, patch.lastEvent = bounds('Scene')
    patch.shader = shader; patch.stage = rd.ShaderStage.Vertex; patch.slot = 0
    patch.data = struct.pack('16f', *view)
    assert not replay.SetReplayConstantOverrides([patch])
    at('Scene'); close(pixels('Scene'), reference)
    close(pixels('SceneDepth'), reference_depth)
    passed('camera matches direct engine reference; mixed fields, repeated uploads, indirect, instanced and deferred draws')
    assert not replay.ClearReplayConstantOverrides()
    at('Scene'); close(pixels('Scene'), baseline, 0)
    passed('clear camera restores exact baseline')

    ranged_patch = rd.ReplayConstantOverride(); ranged_patch.shader = shader
    ranged_patch.firstEvent, ranged_patch.lastEvent = bounds('RangedScene'); ranged_patch.data = patch.data
    assert not replay.SetReplayConstantOverrides([ranged_patch])
    at('RangedScene'); close(pixels('RangedScene'), reference)
    close(pixels('RangeDepth'), reference_depth)
    passed('D3D11.1 binding offset and size')
    assert not replay.ClearReplayConstantOverrides()

    at('TAA_PS'); original_taa = pixels('TAA_PS')
    ps_draw = next(a for a in flatten(markers['TAA_PS'].children) if a.flags & rd.ActionFlags.Drawcall)
    replay.SetFrameEvent(ps_draw.eventId, True)
    ps_patch = rd.ReplayConstantOverride(); ps_patch.firstEvent, ps_patch.lastEvent = bounds('TAA_PS')
    ps_patch.shader = replay.GetPipelineState().GetShader(rd.ShaderStage.Pixel)
    ps_patch.stage = rd.ShaderStage.Pixel; ps_patch.slot = 1; ps_patch.data = struct.pack('f', 0)
    assert not replay.SetReplayConstantOverrides([ps_patch])
    at('TAA_PS'); close(pixels('TAA_PS'), baseline)
    assert not replay.ClearReplayConstantOverrides()
    passed('pixel stage constant patch and restore')
    set_passes([rule('TAA_PS', [('Scene', 'TAA_PS')])])
    at('TAA_PS'); close(pixels('TAA_PS'), baseline)
    passed('pixel shader TAA bypass')

    cs_draw = next(a for a in flatten(markers['TAA_CS'].children) if a.flags & rd.ActionFlags.Dispatch)
    replay.SetFrameEvent(cs_draw.eventId, True)
    cs_patch = rd.ReplayConstantOverride(); cs_patch.firstEvent, cs_patch.lastEvent = bounds('TAA_CS')
    cs_patch.shader = replay.GetPipelineState().GetShader(rd.ShaderStage.Compute)
    cs_patch.stage = rd.ShaderStage.Compute; cs_patch.slot = 1; cs_patch.byteOffset = 4; cs_patch.data = struct.pack('f', 0)
    assert not replay.SetReplayConstantOverrides([cs_patch])
    at('TAA_CS'); close(pixels('TAA_CS'), baseline)
    assert not replay.ClearReplayConstantOverrides()
    passed('compute stage constant patch preserves dispatch state')

    both = [rule('TAA_PS', [('Scene', 'TAA_PS')]), rule('TAA_CS', [('TAA_PS', 'TAA_CS')])]
    set_passes(both); at('TAA_CS'); close(pixels('TAA_CS'), baseline)
    passed('consecutive pixel/compute bypasses')

    all_passes = both + [rule('Upscale_TAA', [('TAA_CS', 'Upscaled')])]
    set_passes(all_passes)
    assert not replay.SetReplayConstantOverrides([patch])
    at('UI'); expected = pixels('ExpectedFinal'); close(pixels('Final'), expected)
    assert tuple(expected[:4]) == (1, 1, 0, 1)
    passed('camera plus bypass plus bilinear upscaling preserves downstream tone mapping and UI')

    set_passes(all_passes + [rule('InPlace', [('Final', 'Final')])])
    at('InPlace'); close(pixels('Final'), expected)
    passed('same-resource bypass snapshots input before pass')

    set_passes(all_passes + [rule('CopiesAndClears')])
    at('CopiesAndClears'); close(pixels('Scene'), reference)
    passed('skip suppresses clears and copies while preserving later replay')

    # Multiple mappings and invalid configurations must leave previously applied rules intact.
    p = rule('TAA_PS', [('Scene', 'TAA_PS'), ('Scene', 'CopyScratch')])
    set_passes([p]); at('TAA_PS'); close(pixels('TAA_PS'), reference); close(pixels('CopyScratch'), reference)
    passed('multiple colour outputs')
    overlap = rule('TAA_PS')
    assert replay.SetReplayPassOverrides([p, overlap])
    at('TAA_PS'); close(pixels('TAA_PS'), reference)
    invalid = rule('TAA_PS', [('Scene', 'TAA_PS')]); invalid_colour = invalid.colours[0]; invalid_colour.sourceMip = 99; invalid.colours = [invalid_colour]
    assert replay.SetReplayPassOverrides([invalid])
    passed('invalid overlap and subresource configurations rejected atomically')

    # Exercise the WithoutDraw/OnlyDraw split on both sides and inside the bypass interval.
    set_passes([p])
    first, last = bounds('TAA_PS')
    for eid in [first-1, first, last-1, last, bounds('TAA_CS')[1], last, first, last]:
        replay.SetFrameEvent(eid, True)
        if eid >= last:
            close(pixels('TAA_PS'), reference)
    passed('event navigation and split replay boundaries')

    assert not replay.ClearReplayPassOverrides()
    assert not replay.ClearReplayConstantOverrides()
    at('TAA_PS'); close(pixels('TAA_PS'), original_taa, 0)
    at('Scene'); close(pixels('Scene'), baseline, 0)
    passed('restore all returns exact original images')

    mapping = rd.ReplayColourMapping(); mapping.source = resources['ArraySource']; mapping.destination = resources['ArrayDest']
    mapping.sourceMip = mapping.destinationMip = 1; mapping.sourceSlice = 1
    p = rule('ArrayPass', [('ArraySource', 'ArrayDest')]); p.colours = [mapping]
    set_passes([p]); at('ArrayPass'); close(pixels('ArrayDest', 1), pixels('ArraySource', 1, 1))
    passed('nonzero mip and array slice colour bypass')

    p = rule('ConvertPass', [('ArraySource', 'Converted')]); mapping.destination = resources['Converted']; mapping.destinationMip = 0; p.colours = [mapping]
    set_passes([p]); at('ConvertPass')
    raw = bytes(replay.GetTextureData(resources['Converted'], rd.Subresource()))
    assert len(raw) == 64*64*4
    assert all(abs(value - [64, 128, 191, 255][i % 4]) <= 1 for i, value in enumerate(raw))
    passed('float to UNorm conversion with scaling')

    assert replay.SetReplayPassOverrides([rule('ConvertPass', [('MSAA', 'Converted')])])
    passed('MSAA colour bypass rejected')
    set_passes([rule('GenerateMips')]); at('GenerateMips')
    assert tuple(pixels('MipTexture', 1)[:4]) == (1, 0, 0, 1)
    set_passes([]); at('GenerateMips')
    assert tuple(pixels('MipTexture', 1)[:4]) == (0, 0, 1, 1)
    passed('skip suppresses mip generation')
    set_passes([rule('Resolve')]); at('ConvertPass')
    before = bytes(replay.GetTextureData(resources['Converted'], rd.Subresource()))
    at('Resolve'); assert bytes(replay.GetTextureData(resources['Converted'], rd.Subresource())) == before
    set_passes([]); at('Resolve')
    assert bytes(replay.GetTextureData(resources['Converted'], rd.Subresource()))[:4] == bytes([0, 0, 255, 255])
    passed('skip suppresses multisample resolve')
    set_passes([rule('Discard')]); at('Discard')
    assert tuple(pixels('CopyScratch')[:4]) == (.25, .5, .75, 1)
    passed('skip preserves resource contents across discard')
    assert not replay.ClearReplayPassOverrides()
    # The bind window can extend beyond allocation; the shader explicitly reads c64.
    extended = rd.ReplayConstantOverride()
    extended.shader = shader
    extended.firstEvent, extended.lastEvent = bounds('WindowPastBuffer')
    extended.data = patch.data
    assert not replay.SetReplayConstantOverrides([extended], bounds('WindowPastBuffer')[1])
    close(pixels('RangedScene'), reference)
    close(pixels('RangeDepth'), reference_depth)
    stats = json.loads(replay.GetReplayOverrideStats())
    assert stats['copy_bytes'] == 3 * 2 * (1024 - 256), stats
    assert not replay.SetReplayConstantOverrides([extended], bounds('WindowPastBuffer')[1])
    assert json.loads(replay.GetReplayOverrideStats())['index_rebuilds'] == 0
    passed('window beyond allocation preserves zero reads; compact copy bytes and index reuse')
    # Overlapping writes are ordered: later bytes must win, even inside the indexed bucket.
    later = rd.ReplayConstantOverride()
    later.shader = shader
    later.firstEvent, later.lastEvent = extended.firstEvent, extended.lastEvent
    later.byteOffset = 12
    later.data = struct.pack('f', 0)
    assert not replay.SetReplayConstantOverrides([extended, later], bounds('WindowPastBuffer')[1])
    close(pixels('RangedScene'), baseline)
    assert not replay.SetReplayConstantOverrides([later, extended], bounds('WindowPastBuffer')[1])
    close(pixels('RangedScene'), reference)
    passed('overlapping rule order is preserved')
    assert replay.SetReplayConstantOverrides([extended], 0xffffffff)
    close(pixels('RangedScene'), reference)
    passed('invalid target event rejected without replacing active overrides')
    assert not replay.ClearReplayConstantOverrides()
    taa_rule = rule('TAA_PS', [('Scene', 'TAA_PS')])
    target = bounds('TAA_PS')[1]
    assert not replay.SetReplayOverrides([patch], [taa_rule], target)
    close(pixels('TAA_PS'), reference)
    passed('combined camera/pass transaction reaches target with both overrides installed')
    bad = rd.ReplayConstantOverride(); bad.shader = shader
    bad.firstEvent, bad.lastEvent = bounds('Scene'); bad.data = b''
    assert replay.SetReplayOverrides([bad], [], target)
    at('TAA_PS'); close(pixels('TAA_PS'), reference)
    invalid_pass = rule('TAA_PS', [('Scene', 'TAA_PS')])
    invalid_colour = invalid_pass.colours[0]; invalid_colour.destinationMip = 999
    invalid_pass.colours = [invalid_colour]
    assert replay.SetReplayOverrides([], [invalid_pass], target)
    at('TAA_PS'); close(pixels('TAA_PS'), reference)
    passed('combined validation failure preserves camera and pass rules')
    bad.data = patch.data; bad.byteOffset = 60000
    assert replay.SetReplayOverrides([bad], [], target)
    close(pixels('TAA_PS'), reference)
    at('TAA_PS'); close(pixels('TAA_PS'), reference)
    passed('combined runtime binding failure restores previous rules and output')
    assert not replay.SetReplayOverrides([], [], target)
    close(pixels('TAA_PS'), original_taa, 0)
    passed('clearing combined overrides restores exact original frame')
    print(json.dumps({'status': 'PASS', 'checks': results, 'capture': sys.argv[2]}, indent=2), flush=True)
finally:
    replay.Shutdown(); cap.Shutdown(); rd.ShutdownReplay()
