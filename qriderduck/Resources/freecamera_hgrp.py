"""HGRP camera discovery. Runs on the replay thread, using only public replay APIs.

Scanner.step() is bounded so the host can cancel between batches. The host owns
replay restoration and application; discovery never installs overrides.
"""
import json
import math
import re
import struct

import riderduck as rd


from freecamera_common import (transpose, multiply, inverse, close, IDENTITY, STAGES, registers, flatten)

LAYOUT = ((0, 'View', None), (64, 'InverseView', None),
          (128, 'Projection', 'jittered'), (192, 'InverseProjection', 'jittered'),
          (256, 'ViewProjection', 'jittered'),
          (320, 'RelativeViewProjection', 'jittered'),
          (384, 'InverseViewProjection', 'jittered'),
          (448, 'ViewProjection', 'stable'),
          (512, 'RelativeViewProjection', 'stable'),
          (576, 'InverseViewProjection', 'stable'), (704, 'CameraPosition', None))



def decode(data):
    if len(data) < 716:
        raise ValueError('Camera binding is smaller than the supported HGRP layout')
    values = struct.unpack('<{}f'.format(len(data) // 4), data[:len(data) // 4 * 4])
    def matrix(offset):
        return transpose(values[offset // 4:offset // 4 + 16])
    view, pose, projection = matrix(0), matrix(64), matrix(128)
    if not close(multiply(view, pose), IDENTITY):
        raise ValueError('View / InverseView do not agree')
    if not close(view[12:16], [0, 0, 0, 1], 0.0001):
        raise ValueError('View is not affine')
    if not close([pose[3], pose[7], pose[11]], values[176:179]):
        raise ValueError('Camera position does not agree with InverseView')
    # HGRP uses a perspective projection; identity/object transforms are not seeds.
    if abs(projection[15]) > 0.0001 or abs(abs(projection[14]) - 1) > 0.0001:
        raise ValueError('Not a supported perspective camera')
    relative_view = list(view)
    relative_view[3] = relative_view[7] = relative_view[11] = 0.0
    stable = multiply(matrix(512), inverse(relative_view))
    for p, vp, relative_vp, inv_vp in ((projection, 256, 320, 384),
                                       (stable, 448, 512, 576)):
        if not close(multiply(p, view), matrix(vp)) or not close(
                multiply(p, relative_view), matrix(relative_vp)) or not close(
                inverse(multiply(p, view)), matrix(inv_vp)):
            raise ValueError('HGRP projection / VP variants do not agree')
    if not close(inverse(projection), matrix(192)):
        raise ValueError('Projection / InverseProjection do not agree')
    return {'view': view, 'projection': projection, 'jittered': projection,
            'stable': stable, 'position': list(values[176:179])}




class Scanner:
    def __init__(self, controller, seed):
        self.r = controller
        all_actions = list(flatten(controller.GetRootActions()))
        self.actions = [a for a in all_actions if a.flags &
                        (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch)]
        if not any(a.eventId == seed and a.flags & rd.ActionFlags.Drawcall for a in self.actions):
            raise ValueError('Select a representative main-scene draw first')
        self.index = 0
        self.shader_cache = {}
        self.fields = []
        self.active = {}
        self.issues = []
        self.history = set()
        self.other_cameras = set()
        self.matches = 0
        self.output = None
        presents = [a for a in all_actions if a.flags & rd.ActionFlags.Present and
                    a.copyDestination != rd.ResourceId.Null()]
        textures = {str(t.resourceId) for t in controller.GetTextures()}
        if presents and len({str(a.copyDestination) for a in presents}) == 1:
            last = presents[-1]
            if str(last.copyDestination) in textures:
                self.output = (last.eventId, str(last.copyDestination))
        controller.SetFrameEvent(seed, True)
        seeds = []
        for stage, label, shader, slot, data in self.bindings():
            try:
                camera = decode(data)
            except ValueError:
                continue
            regs, dynamic = self.shader_cache[(label, shader, slot)]
            if not dynamic and any(reg in regs for reg in range(44)):
                seeds.append(camera)
        if not seeds or any(not close(s['view'], seeds[0]['view'], 0.00001, 0.000001)
                            for s in seeds[1:]):
            raise ValueError('The selected draw has no unique verified HGRP camera')
        self.seed = seeds[0]

    def bindings(self):
        pipe = self.r.GetPipelineState()
        for stage, label in STAGES:
            if (label == 'CS') != self.compute:
                continue
            reflection = pipe.GetShaderReflection(stage)
            if reflection is None:
                continue
            shader = str(pipe.GetShader(stage))
            for bi, block in enumerate(reflection.constantBlocks):
                slot = block.fixedBindNumber
                key = label, shader, slot
                if key not in self.shader_cache:
                    code = self.r.DisassembleShader(rd.ResourceId.Null(), reflection, 'DXBC')
                    self.shader_cache[key] = registers(code, slot)
                descriptor = pipe.GetConstantBlock(stage, bi, 0).descriptor
                data = b''
                if descriptor.resource != rd.ResourceId.Null() and descriptor.byteSize >= 64:
                    data = bytes(self.r.GetBufferData(descriptor.resource, descriptor.byteOffset,
                                                     min(720, descriptor.byteSize)))
                yield stage, label, shader, slot, data

    compute = False

    def step(self, count=16):
        for action in self.actions[self.index:self.index + count]:
            self.compute = bool(action.flags & rd.ActionFlags.Dispatch)
            self.r.SetFrameEvent(action.eventId, True)
            seen = set()
            for stage, label, shader, slot, data in self.bindings():
                key = label, shader, slot
                seen.add(key)
                current = None
                same_view = len(data) >= 64 and close(
                    transpose(struct.unpack('<16f', data[:64])), self.seed['view'], 0.00001, 0.000001)
                if same_view:
                    try:
                        camera = decode(data)
                        regs, dynamic = self.shader_cache[key]
                        if dynamic:
                            raise ValueError('Dynamically indexed camera constants')
                        current = []
                        covered = set()
                        for offset, semantic, variant in LAYOUT:
                            used = set(range(offset // 16, offset // 16 + (1 if offset == 704 else 4)))
                            covered.update(used)
                            if not used.intersection(regs):
                                continue
                            field = dict(semantic=semantic, shader=shader, stage=label, slot=slot,
                                         offset=offset, row_major=True, row_vector=True)
                            if variant:
                                field['projection'] = camera[variant]
                            current.append(field)
                        if any(r >= 45 for r in regs):
                            self.history.add(key)
                        if any(r < 45 and r not in covered for r in regs):
                            raise ValueError('Unclassified constants in the camera layout')
                        if current:
                            self.matches += 1
                    except ValueError as error:
                        self.issues.append('EID {} {} b{}: {}'.format(action.eventId, label, slot, error))
                        current = None
                elif len(data) >= 716:
                    try:
                        decode(data)
                        self.other_cameras.add(key)
                    except ValueError:
                        pass
                signature = json.dumps(current, sort_keys=True) if current else None
                old = self.active.get(key)
                if signature is not None:
                    if old and old[0] == signature:
                        for field in old[1]:
                            field['last_event'] = action.eventId
                    else:
                        for field in current:
                            field.update(first_event=action.eventId, last_event=action.eventId)
                        self.fields.extend(current)
                        self.active[key] = signature, current
                else:
                    self.active.pop(key, None)
            # A reflected block disappearing from a shader must also break its interval.
            pipe = self.r.GetPipelineState()
            for key in list(self.active):
                stage = next(s for s, label in STAGES if label == key[0])
                if str(pipe.GetShader(stage)) == key[1] and key not in seen:
                    del self.active[key]
            self.index += 1
        done = self.index == len(self.actions)
        result = {'done': done, 'progress': self.index, 'total': len(self.actions)}
        if done:
            config = dict(view=self.seed['view'], projection=self.seed['projection'],
                          fields=self.fields, forward='-Z', enabled=True, show=False)
            if self.output:
                config.update(output_rid=self.output[1], flip_y=False)
                result['event_id'] = self.output[0]
            result.update(config=config, partial=bool(self.issues or self.history),
                          report='{} fields, {} camera bindings.\n{} other-camera shader bindings excluded.\n'
                          '{} shader bindings read history/auxiliary constants; these are not rewritten.\n{}'.format(
                              len(self.fields), self.matches, len(self.other_cameras), len(self.history),
                              '\n'.join(self.issues[:100])))
            if not self.output:
                result['report'] += '\nNo unique Present output; keeping the current display.'
            if not self.fields:
                raise ValueError('No confirmed camera fields were found')
        return result
