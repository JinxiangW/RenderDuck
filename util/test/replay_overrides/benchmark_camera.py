"""Compare identical camera patches with separate application runtimes, on owned RDCs.

Invocation: pyhost benchmark_camera.py RUNTIME CAPTURE CONFIG OUTPUT [COUNT=30]
The capture is read-only. No GUI or MCP instance is accessed.
"""
import hashlib
import importlib.util
import json
import math
import os
import statistics
import struct
import sys
import time

runtime, capture, config_path, output = sys.argv[1:5]
count = int(sys.argv[5]) if len(sys.argv) > 5 else 30
sys.path.insert(0, os.path.join(runtime, 'pymodules'))
import riderduck as rd
source = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../qriderduck/Resources/freecamera_common.py'))
spec = importlib.util.spec_from_file_location('camera_math', source)
maths = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maths)
with open(config_path) as f:
    document = json.load(f)
config = document.get('configuration', document.get('config', document))
os.makedirs(output, exist_ok=True)
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
assert cap.OpenFile(capture, '', None) == rd.ResultCode.Succeeded
status, r = cap.OpenCapture(rd.ReplayOptions(), None)
assert status == rd.ResultCode.Succeeded, str(status)


def digest(resource):
    return hashlib.sha256(bytes(r.GetTextureData(resource, rd.Subresource()))).hexdigest()


def percentile(values):
    return sorted(values)[max(0, int(math.ceil(len(values) * .95)) - 1)]


try:
    resources = {str(x.resourceId): rd.ResourceId(x.resourceId) for x in r.GetResources()}
    actions = list(maths.flatten(r.GetRootActions()))
    event = config.get('event_id', document.get('event_id', actions[-1].eventId))
    colour = resources[config['output_rid']]
    r.SetFrameEvent(event, True)
    # Last covered graphics draw supplies a relevant scene depth view, before Present unbinds it.
    covered = [a for a in actions if a.flags & rd.ActionFlags.Drawcall and
               any(f['first_event'] <= a.eventId <= f['last_event'] and f['stage'] != 'CS'
                   for f in config['fields'])]
    depth_event = 0
    depth = rd.ResourceId.Null()
    for action in reversed(covered):
        r.SetFrameEvent(action.eventId, True)
        depth = rd.ResourceId(r.GetPipelineState().GetDepthTarget().resource)
        if depth != rd.ResourceId.Null():
            depth_event = action.eventId
            break
    r.SetFrameEvent(event, True)
    for _ in range(3):
        r.SetFrameEvent(event, True)
        digest(colour)
    original = digest(colour)
    original_depth = None
    if depth_event:
        r.SetFrameEvent(depth_event, True)
        original_depth = digest(depth)
    stages = dict(maths.STAGES)
    stages = {label: stage for stage, label in maths.STAGES}
    base_pose = maths.inverse(config['view'])

    def patches_for(index):
        angle = (index + 1) * .001
        rotation = [math.cos(angle), 0, math.sin(angle), 0, 0, 1, 0, 0,
                    -math.sin(angle), 0, math.cos(angle), 0, 0, 0, 0, 1]
        pose = maths.multiply(rotation, base_pose)
        pose[3], pose[7], pose[11] = base_pose[3] + index * .01, base_pose[7], base_pose[11]
        view = maths.inverse(pose)
        relative = list(view)
        relative[3] = relative[7] = relative[11] = 0
        cache = {}
        patches = []
        for field in config['fields']:
            proj = field.get('projection', config['projection'])
            key = tuple(proj)
            if key not in cache:
                vp, relative_vp = maths.multiply(proj, view), maths.multiply(proj, relative)
                cache[key] = dict(View=view, InverseView=pose, Projection=proj,
                                  InverseProjection=maths.inverse(proj), ViewProjection=vp,
                                  InverseViewProjection=maths.inverse(vp),
                                  RelativeViewProjection=relative_vp,
                                  InverseRelativeViewProjection=maths.inverse(relative_vp))
            semantic = field['semantic']
            if semantic == 'CameraPosition':
                data = [pose[3], pose[7], pose[11]]
            else:
                data = cache[key][semantic]
                if field.get('row_vector', False): data = maths.transpose(data)
                if not field.get('row_major', True): data = maths.transpose(data)
            patch = rd.ReplayConstantOverride()
            patch.firstEvent, patch.lastEvent = field['first_event'], field['last_event']
            patch.shader, patch.stage = resources[field['shader']], stages[field['stage']]
            patch.slot, patch.byteOffset = field['slot'], field['offset']
            patch.data = struct.pack('<{}f'.format(len(data)), *data)
            patches.append(patch)
        return patches

    poses = [patches_for(0 if os.environ.get('RIDERDUCK_BENCHMARK_FIXED_CAMERA') else i)
             for i in range(count)]
    has_target = hasattr(r, 'GetReplayOverrideStats')

    def apply(patches, target):
        if has_target:
            error = r.SetReplayConstantOverrides(patches, target)
        else:
            # Existing UI first selected its target, then forced another replay with overrides.
            if target != event:
                r.SetFrameEvent(target, True)
            error = r.SetReplayConstantOverrides(patches)
        assert not error, error

    result = dict(capture=capture, event_id=event, depth_event=depth_event,
                  field_count=len(config['fields']), frames=count, modes={})
    for mode in ('no_override', 'camera'):
        r.ClearReplayConstantOverrides()
        seed = min(f['first_event'] for f in config['fields'])
        r.SetFrameEvent(seed, True)
        t = time.perf_counter()
        if mode == 'camera':
            if has_target:
                apply(poses[0], event)
            else:
                r.SetFrameEvent(event, True)
                apply(poses[0], event)
        else:
            r.SetFrameEvent(event, True)
        first_submit = (time.perf_counter() - t) * 1000
        digest(colour)  # Complete GPU work before timed steady-state samples.
        first_complete = (time.perf_counter() - t) * 1000
        for _ in range(3):
            if mode == 'camera': apply(poses[0], event)
            else: r.SetFrameEvent(event, True)
            digest(colour)
        times, complete, stats, hashes = [], [], [], []
        for i, patches in enumerate(poses):
            t = time.perf_counter()
            if mode == 'camera': apply(patches, event)
            else: r.SetFrameEvent(event, True)
            times.append((time.perf_counter() - t) * 1000)
            hashes.append(digest(colour))
            complete.append((time.perf_counter() - t) * 1000)
            if os.environ.get('RIDERDUCK_BENCHMARK_DUMP'):
                with open(os.path.join(output, '{}-{}.bin'.format(mode, i)), 'wb') as stream:
                    stream.write(bytes(r.GetTextureData(colour, rd.Subresource())))
            if has_target and mode == 'camera': stats.append(json.loads(r.GetReplayOverrideStats()))
            print('{} {}/{} {:.1f} ms'.format(mode, i + 1, count, complete[-1]), flush=True)
        result['modes'][mode] = dict(first_apply_ms=first_submit, first_apply_completed_ms=first_complete,
                                    median_ms=statistics.median(times),
                                    p95_ms=percentile(times), completed_median_ms=statistics.median(complete),
                                    completed_p95_ms=percentile(complete), samples_ms=times,
                                    completed_samples_ms=complete, colour_hashes=hashes, stats=stats)
    depth_hashes = []
    if depth_event:
        for patches in (poses[0], poses[count // 2], poses[-1]):
            if not has_target: r.SetFrameEvent(depth_event, True)
            apply(patches, depth_event)
            depth_hashes.append(digest(depth))
    result['depth_hashes'] = depth_hashes
    r.ClearReplayConstantOverrides()
    r.SetFrameEvent(event, True)
    reset_colour = digest(colour)
    if depth_event:
        r.SetFrameEvent(depth_event, True)
        reset_depth = digest(depth)
    else:
        reset_depth = None
    result.update(reset_exact=reset_colour == original and reset_depth == original_depth,
                  reset_colour_hash=reset_colour, reset_depth_hash=reset_depth,
                  original_colour_hash=original, original_depth_hash=original_depth)
    with open(os.path.join(output, 'result.json'), 'w') as f:
        json.dump(result, f, indent=2)
finally:
    r.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
